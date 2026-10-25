#include "table_csv.hxx"

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <span>
#include <streambuf>
#include <vector>

#include <csv.hpp>

#include "compress.hxx"
#include "decompress.hxx"
#include "text_encoding.hxx"

namespace
{
    std::string lowerExtension(const std::filesystem::path &path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    std::optional<char> delimiterFor(const std::filesystem::path &path, const OpenOptions &options)
    {
        if (options.delimiter)
            return options.delimiter;
        const std::string ext = lowerExtension(stripCompressionExtension(path));
        if (ext == ".csv")
            return ',';
        if (ext == ".tsv" || ext == ".tab" || ext == ".vcf")
            return '\t';
        return std::nullopt;
    }

    bool isVcf(const std::filesystem::path &path)
    {
        return lowerExtension(stripCompressionExtension(path)) == ".vcf";
    }

    // Drops lines starting with "##" (VCF meta-information) before the CSV
    // parser sees them: they may contain quotes and delimiters of their own.
    class MetaLineFilterBuf : public std::streambuf
    {
    private:
        static constexpr size_t CHUNK_SIZE = 64 * 1024;
        std::unique_ptr<std::istream> source_;
        std::vector<char> in_;
        std::vector<char> out_;
        bool at_line_start_ = true;
        bool pending_hash_ = false; // a '#' at the start of a line, held back
        bool skipping_ = false;

    protected:
        int_type underflow() override
        {
            if (gptr() < egptr())
                return traits_type::to_int_type(*gptr());
            out_.clear();
            while (out_.empty())
            {
                const std::streamsize n = source_->rdbuf()->sgetn(in_.data(), static_cast<std::streamsize>(in_.size()));
                if (n <= 0)
                {
                    if (pending_hash_)
                    {
                        out_.push_back('#');
                        pending_hash_ = false;
                    }
                    break;
                }
                for (std::streamsize i = 0; i < n; ++i)
                {
                    const char c = in_[static_cast<size_t>(i)];
                    if (skipping_)
                    {
                        if (c == '\n')
                        {
                            skipping_ = false;
                            at_line_start_ = true;
                        }
                        continue;
                    }
                    if (pending_hash_)
                    {
                        pending_hash_ = false;
                        if (c == '#')
                        {
                            skipping_ = true;
                            continue;
                        }
                        out_.push_back('#');
                    }
                    else if (at_line_start_ && c == '#')
                    {
                        pending_hash_ = true;
                        at_line_start_ = false;
                        continue;
                    }
                    out_.push_back(c);
                    at_line_start_ = c == '\n';
                }
            }
            if (out_.empty())
                return traits_type::eof();
            setg(out_.data(), out_.data(), out_.data() + out_.size());
            return traits_type::to_int_type(*gptr());
        }

    public:
        explicit MetaLineFilterBuf(std::unique_ptr<std::istream> source)
            : source_(std::move(source)), in_(CHUNK_SIZE)
        {
            out_.reserve(CHUNK_SIZE);
        }
    };

    class MetaLineFilterStream : public std::istream
    {
    private:
        MetaLineFilterBuf buf_;

    public:
        explicit MetaLineFilterStream(std::unique_ptr<std::istream> source)
            : std::istream(nullptr), buf_(std::move(source))
        {
            rdbuf(&buf_);
            // Like the decompressing stream: read errors must not look like the end of the file.
            exceptions(std::ios::badbit);
        }
    };

    // Counts the bytes of the file read through it and keeps the last few,
    // so that a follow knows where the file was read up to (readAppended()).
    class CountingBuf : public std::streambuf
    {
    public:
        static constexpr size_t TAIL_BYTES = 64;

    private:
        static constexpr size_t CHUNK_SIZE = 64 * 1024;
        std::unique_ptr<std::istream> source_;
        std::vector<char> buffer_;
        uint64_t count_ = 0;
        std::string tail_;

    protected:
        int_type underflow() override
        {
            if (gptr() < egptr())
                return traits_type::to_int_type(*gptr());
            const std::streamsize n = source_->rdbuf()->sgetn(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            if (n <= 0)
                return traits_type::eof();
            count_ += static_cast<uint64_t>(n);
            tail_.append(buffer_.data(), static_cast<size_t>(n));
            if (tail_.size() > TAIL_BYTES)
                tail_.erase(0, tail_.size() - TAIL_BYTES);
            setg(buffer_.data(), buffer_.data(), buffer_.data() + n);
            return traits_type::to_int_type(*gptr());
        }

    public:
        explicit CountingBuf(std::unique_ptr<std::istream> source) : source_(std::move(source)), buffer_(CHUNK_SIZE) {}
        uint64_t count() const { return count_; }
        const std::string &tail() const { return tail_; }
    };

    class CountingStream : public std::istream
    {
    private:
        CountingBuf buf_;

    public:
        explicit CountingStream(std::unique_ptr<std::istream> source) : std::istream(nullptr), buf_(std::move(source))
        {
            rdbuf(&buf_);
            exceptions(std::ios::badbit);
        }
        const CountingBuf &counter() const { return buf_; }
    };

    // Whether appended rows can be read on their own: from a byte offset of
    // the file, with line breaks that are single bytes (not UTF-16).
    bool followable(Compression compression, TextEncoding encoding)
    {
        return compression == Compression::None && (encoding == TextEncoding::Utf8 || encoding == TextEncoding::Cp932);
    }

    // The length of the whole records at the start of `text`: up to the last
    // line break outside a quoted field. As csv-parser reads them, a quote
    // starts a quoted field only at the start of a field; anywhere else
    // (12" pizza) it is an ordinary character.
    size_t wholeRecordsLength(std::string_view text, char delimiter)
    {
        enum class State
        {
            FieldStart,
            Unquoted,
            Quoted,
            QuoteInQuoted, // a quote in a quoted field: its end, or the first of two
        };
        State state = State::FieldStart;
        size_t length = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            switch (state)
            {
            case State::FieldStart:
            case State::Unquoted:
                if (c == '\n')
                {
                    length = i + 1;
                    state = State::FieldStart;
                }
                else if (c == delimiter)
                    state = State::FieldStart;
                else if (c == '"' && state == State::FieldStart)
                    state = State::Quoted;
                else
                    state = State::Unquoted;
                break;
            case State::Quoted:
                if (c == '"')
                    state = State::QuoteInQuoted;
                break;
            case State::QuoteInQuoted:
                if (c == '"')
                    state = State::Quoted; // a doubled quote
                else if (c == '\n')
                {
                    length = i + 1;
                    state = State::FieldStart;
                }
                else
                    state = c == delimiter ? State::FieldStart : State::Unquoted;
                break;
            }
        }
        return length;
    }

    // path::string() uses the ANSI code page on Windows; sheet names must be UTF-8.
    std::string utf8FileName(const std::filesystem::path &path)
    {
        const std::u8string name = path.filename().u8string();
        return std::string(name.begin(), name.end());
    }

    // The bytes of the file, decompressed if needed.
    std::unique_ptr<std::istream> openRawStream(const std::filesystem::path &path, Compression compression)
    {
        if (compression != Compression::None)
            return openDecompressingStream(path, compression);
        auto file = std::make_unique<std::ifstream>(path, std::ios::binary);
        if (!*file)
            throw std::runtime_error("Cannot open CSV file: " + utf8FileName(path));
        return file;
    }

    // How much of the (decompressed) file the encoding is guessed from.
    constexpr size_t DETECT_ENCODING_BYTES = 64 * 1024;

    TextEncoding detectFileEncoding(const std::filesystem::path &path)
    {
        try
        {
            auto stream = openRawStream(path, detectCompression(path));
            std::vector<char> head(DETECT_ENCODING_BYTES);
            const std::streamsize n = stream->rdbuf()->sgetn(head.data(), static_cast<std::streamsize>(head.size()));
            return detectTextEncoding(std::as_bytes(std::span<const char>(head.data(), static_cast<size_t>(std::max<std::streamsize>(n, 0)))));
        }
        catch (const std::exception &)
        {
            // Unreadable or corrupt: reported when the rows are read.
            return TextEncoding::Utf8;
        }
    }

    // How the sheet was read, for saving it: the sheet source finds out.
    struct CsvLayout
    {
        bool known = false;
        char delimiter = ',';
        bool header = false;
    };

    // Reads text character by character, as TextDecoder splits it, keeping
    // the bytes.
    class CharacterReader
    {
    private:
        static constexpr size_t CHUNK_SIZE = 64 * 1024;
        std::istream &in_;
        TextEncoding encoding_;
        std::string buffer_;
        size_t pos_ = 0;
        bool eof_ = false;

        void fill()
        {
            buffer_.erase(0, pos_);
            pos_ = 0;
            const size_t old_size = buffer_.size();
            buffer_.resize(old_size + CHUNK_SIZE);
            in_.read(buffer_.data() + old_size, static_cast<std::streamsize>(CHUNK_SIZE));
            const auto n = static_cast<size_t>(in_.gcount());
            buffer_.resize(old_size + n);
            if (n == 0)
                eof_ = true;
        }

    public:
        CharacterReader(std::istream &in, TextEncoding encoding) : in_(in), encoding_(encoding) {}

        // The character `ahead` bytes past the current one; length 0 at the end.
        TextCharacter peek(size_t ahead = 0)
        {
            for (;;)
            {
                const size_t at = pos_ + ahead;
                if (at < buffer_.size())
                {
                    const TextCharacter c = textCharacterAt(encoding_, reinterpret_cast<const unsigned char *>(buffer_.data()),
                                                            buffer_.size(), at, eof_);
                    if (c.length > 0)
                        return c;
                }
                else if (eof_)
                    return {};
                fill();
            }
        }

        // Whether the text starts with these bytes (a byte order mark).
        bool startsWith(std::string_view bytes)
        {
            while (buffer_.size() - pos_ < bytes.size() && !eof_)
                fill();
            return std::string_view(buffer_).substr(pos_).starts_with(bytes);
        }

        // The next bytes, valid until the next call.
        std::string_view take(size_t bytes)
        {
            const std::string_view taken = std::string_view(buffer_).substr(pos_, bytes);
            pos_ += bytes;
            return taken;
        }
    };

    // The fields of one record's text, as csv-parser reads them.
    std::vector<std::string> parseRecord(std::string_view text, char delimiter)
    {
        std::vector<std::string> fields;
        if (text.empty())
            return fields;
        std::string field;
        bool quoted = false;
        bool field_empty = true;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (quoted)
            {
                if (c != '"')
                    field += c;
                else if (i + 1 < text.size() && text[i + 1] == '"')
                {
                    field += '"';
                    ++i;
                }
                else if (i + 1 == text.size() || text[i + 1] == delimiter || text[i + 1] == '\r' || text[i + 1] == '\n')
                    quoted = false;
                else
                    field += '"'; // a stray quote stays
            }
            else if (c == delimiter)
            {
                fields.push_back(std::move(field));
                field.clear();
                field_empty = true;
            }
            else if (c == '"' && field_empty)
            {
                quoted = true;
                field_empty = false;
            }
            else
            {
                field += c;
                field_empty = false;
            }
        }
        fields.push_back(std::move(field));
        return fields;
    }

    // A record as RFC 4180 has it, quoting only fields that need it.
    std::string formatRecord(const TableRow &row, char delimiter, bool skip_meta_lines)
    {
        std::string text;
        for (size_t column = 0; column < row.size(); ++column)
        {
            if (column > 0)
                text += delimiter;
            const std::string value = formatCell(row[column]);
            const bool quote = value.find_first_of(std::string{delimiter, '"', '\r', '\n'}) != std::string::npos ||
                               // A single empty field would read as an empty record.
                               (row.size() == 1 && value.empty()) ||
                               // Not a "##" line.
                               (column == 0 && value.starts_with("##"));
            if (!quote)
            {
                text += value;
                continue;
            }
            text += '"';
            for (const char c : value)
            {
                if (c == '"')
                    text += '"';
                text += c;
            }
            text += '"';
        }
        if (skip_meta_lines && text.find("\n##") != std::string::npos)
            throw std::runtime_error("A line of the edited cell would start with \"##\", which is skipped when read");
        return text;
    }

    std::string rowText(size_t row)
    {
        return "row " + std::to_string(row + 1);
    }

    // Copies the file to `target` record by record, with the edited records
    // written anew; the others keep their bytes (quotes, line breaks,
    // characters the encoding has twice). Records are split as csv-parser
    // splits the text after the "##" filter.
    void rewriteCsv(const std::filesystem::path &source, const std::filesystem::path &target, TextEncoding encoding,
                    bool skip_meta_lines, const CsvLayout &layout, const std::vector<SheetEdits::Row> &rows,
                    std::stop_token stop)
    {
        std::map<size_t, const SheetEdits::Row *> edited; // by record
        const size_t header_records = layout.header ? 1 : 0;
        for (const auto &row : rows)
            edited[row.row + header_records] = &row;

        const Compression compression = detectCompression(source);
        auto in = openRawStream(source, compression);
        CharacterReader reader(*in, encoding);
        auto out = openCompressedWriter(target, compression, compression == Compression::Gzip && isBgzf(source));
        std::string pending; // output, written in large pieces
        auto flush = [&](bool always)
        {
            if (always || pending.size() >= (1u << 20))
            {
                out->write(pending);
                pending.clear();
            }
        };

        // csv-parser drops the byte order mark, and so does the decoder.
        if (const std::string_view bom = byteOrderMark(encoding); !bom.empty() && reader.startsWith(bom))
            pending += reader.take(bom.size());

        const char delimiter = layout.delimiter;
        size_t record = 0;
        auto it = edited.begin();
        bool wanted = it != edited.end() && it->first == record;
        std::string text;      // of a wanted record, without its line break
        bool has_meta = false; // a wanted record has a "##" line inside
        bool in_record = false;
        bool quoted = false;
        bool field_empty = true;
        bool line_start = true;

        auto finishRecord = [&](std::string_view line_break)
        {
            if (wanted)
            {
                const SheetEdits::Row &row = *it->second;
                if (has_meta)
                    throw std::runtime_error("Cannot save " + rowText(row.row) + ": it has a line starting with \"##\" inside");
                std::string decoded;
                if (encoding == TextEncoding::Utf8)
                    decoded = text;
                else
                {
                    TextDecoder decoder(encoding);
                    decoder.decode(text, decoded);
                    decoder.finish(decoded);
                }
                const std::vector<std::string> fields = parseRecord(decoded, delimiter);
                const bool matches = fields.size() == row.original.size() &&
                                     std::equal(fields.begin(), fields.end(), row.original.begin(),
                                                [](const std::string &field, const CellValue &value)
                                                { return field == formatCell(value); });
                if (!matches)
                    throw std::runtime_error("Cannot save: " + rowText(row.row) + " of the file is not what was read");
                const auto encoded = encodeText(formatRecord(row.edited, delimiter, skip_meta_lines), encoding);
                if (!encoded)
                    throw std::runtime_error("Cannot save " + rowText(row.row) + ": it has characters that " +
                                             textEncodingName(encoding) + " cannot hold");
                pending += *encoded;
                pending += line_break;
                ++it;
            }
            else
                pending += line_break;
            ++record;
            wanted = it != edited.end() && it->first == record;
            text.clear();
            has_meta = false;
            in_record = false;
            quoted = false;
            field_empty = true;
            flush(false);
        };
        // A character of the record.
        auto add = [&](std::string_view bytes)
        {
            if (wanted)
                text += bytes;
            else
                pending += bytes;
            in_record = true;
        };

        for (size_t count = 0;; ++count)
        {
            if ((count & 0xFFFF) == 0 && stop.stop_requested())
                throw std::runtime_error("Saving was cancelled");
            const TextCharacter c = reader.peek();
            if (c.length == 0)
                break;
            if (skip_meta_lines && line_start && c.ascii == '#' && reader.peek(c.length).ascii == '#')
            {
                // The filter drops the line before csv-parser sees it.
                if (in_record && wanted)
                    has_meta = true;
                for (TextCharacter m = c; m.length > 0; m = reader.peek())
                {
                    const std::string_view bytes = reader.take(m.length);
                    if (!(in_record && wanted))
                        pending += bytes;
                    if (m.ascii == '\n')
                        break;
                }
                continue;
            }
            line_start = c.ascii == '\n';
            if (quoted)
            {
                std::string bytes(reader.take(c.length));
                if (c.ascii == '"')
                {
                    const TextCharacter next = reader.peek();
                    if (next.ascii == '"')
                        bytes += reader.take(next.length);
                    else if (next.length == 0 || next.ascii == delimiter || next.ascii == '\r' || next.ascii == '\n')
                        quoted = false;
                }
                add(bytes);
                continue;
            }
            if (c.ascii == '\r' || c.ascii == '\n')
            {
                std::string line_break(reader.take(c.length));
                if (c.ascii == '\r')
                {
                    if (const TextCharacter next = reader.peek(); next.ascii == '\n')
                    {
                        line_break += reader.take(next.length);
                        line_start = true;
                    }
                }
                finishRecord(line_break);
                continue;
            }
            if (c.ascii == delimiter)
                field_empty = true;
            else
            {
                if (c.ascii == '"' && field_empty)
                    quoted = true;
                field_empty = false;
            }
            add(reader.take(c.length));
        }
        if (in_record)
            finishRecord({});
        if (it != edited.end())
            throw std::runtime_error("Cannot save: " + rowText(it->second->row) + " is not in the file any more");
        flush(true);
        out->finish();
    }

    bool readCsvRow(csv::CSVReader &reader, TableRow &out)
    {
        csv::CSVRow row;
        if (!reader.read_row(row))
            return false;
        out.clear();
        out.reserve(row.size());
        for (size_t i = 0; i < row.size(); ++i)
            out.emplace_back(std::string(row[i].get<csv::string_view>()));
        return true;
    }

    class CsvSheetSource : public SheetSource
    {
    private:
        // Appended bytes read at a time; the rest waits for the next follow.
        static constexpr size_t MAX_APPENDED_BYTES = size_t{64} << 20;

        std::unique_ptr<csv::CSVReader> reader_;
        std::vector<ColumnInfo> columns_;
        std::deque<TableRow> pending_; // rows read ahead while detecting the header, or appended
        size_t next_row_ = 0;
        bool eof_ = false;
        // Following the file: where it was read up to, and the bytes before.
        std::filesystem::path path_;
        TextEncoding encoding_;
        bool skip_meta_lines_;
        char delimiter_ = ',';
        const CountingBuf *counter_ = nullptr; // the file's bytes, if it can be followed
        uint64_t offset_ = 0;
        std::string tail_;
        // A file without rows yet takes its header from the first rows
        // appended, as it would have when read.
        OpenOptions::Header header_option_;
        CsvLayout *layout_;
        bool header_decided_ = false;

        // Takes the first pending row as the header if the option says so.
        void decideHeader()
        {
            if (pending_.empty())
                return;
            header_decided_ = true;
            const TableRow *second = pending_.size() > 1 ? &pending_[1] : nullptr;
            const bool header = header_option_ == OpenOptions::Header::Yes ||
                                (header_option_ == OpenOptions::Header::Auto && looksLikeHeaderRow(pending_.front(), second));
            if (!header)
                return;
            layout_->header = true;
            for (auto &cell : pending_.front())
                columns_.push_back({std::move(std::get<std::string>(cell))});
            pending_.pop_front();
        }

        bool readRow(TableRow &out)
        {
            if (readCsvRow(*reader_, out))
                return true;
            eof_ = true;
            if (counter_)
            {
                offset_ = counter_->count();
                tail_ = counter_->tail();
            }
            return false;
        }

    public:
        CsvSheetSource(const std::filesystem::path &path, const OpenOptions &options, bool skip_meta_lines, TextEncoding encoding,
                       CsvLayout &layout)
            : path_(path), encoding_(encoding), skip_meta_lines_(skip_meta_lines), header_option_(options.header),
              layout_(&layout)
        {
            csv::CSVFormat format;
            if (const auto delimiter = delimiterFor(path, options))
                format.delimiter(*delimiter);
            else
                format.delimiter({',', '\t', ';', '|'});
            format.quote('"').no_header().variable_columns(csv::VariableColumnPolicy::KEEP);
            // Compression is detected from the content so that misnamed files still open.
            const Compression compression = detectCompression(path);
            // The parser drops a UTF-8 BOM itself, but "##" lines must be
            // recognised after it.
            const bool decode = encoding != TextEncoding::Utf8 || skip_meta_lines;
            // A mapped file can fault if another process truncates it before
            // Auto Revert notices. Streams also accept native Windows paths.
            auto stream = openRawStream(path, compression);
            if (followable(compression, encoding))
            {
                auto counting = std::make_unique<CountingStream>(std::move(stream));
                counter_ = &counting->counter();
                stream = std::move(counting);
            }
            if (decode)
                stream = openDecodingStream(std::move(stream), encoding);
            if (skip_meta_lines)
                stream = std::make_unique<MetaLineFilterStream>(std::move(stream));
            reader_ = std::make_unique<csv::CSVReader>(std::move(stream), format);
            layout = {.known = true, .delimiter = reader_->get_format().get_delim()};
            delimiter_ = layout.delimiter;

            // Read the first two rows: the second one helps to tell a title from a header.
            for (int i = 0; i < 2 && !eof_; ++i)
            {
                TableRow row;
                if (readRow(row))
                    pending_.push_back(std::move(row));
            }
            decideHeader();
        }

        SourceCapabilities capabilities() const override
        {
            return {.random_access = false, .query_pushdown = false, .edits = CellEditKind::Text};
        }
        std::vector<ColumnInfo> columns() override { return columns_; }
        RowCount rowCount() override
        {
            const size_t known = next_row_ + pending_.size();
            if (eof_)
                return {known, RowCount::Kind::Exact};
            return {known, known > 0 ? RowCount::Kind::AtLeast : RowCount::Kind::Unknown};
        }

        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token) override
        {
            if (first_row != next_row_)
                throw std::logic_error("CSV source must be read sequentially");
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            block->rows.reserve(max_rows);
            while (!pending_.empty() && block->rows.size() < max_rows)
            {
                block->rows.push_back(std::move(pending_.front()));
                pending_.pop_front();
            }
            while (block->rows.size() < max_rows && !eof_)
            {
                TableRow row;
                if (!readRow(row))
                    break;
                block->rows.push_back(std::move(row));
            }
            next_row_ += block->rows.size();
            return block;
        }

        AppendResult readAppended(std::stop_token, bool &caught_up) override
        {
            caught_up = false;
            if (!counter_)
                return AppendResult::Changed;
            if (!eof_ || !pending_.empty())
                return AppendResult::None;
            std::error_code ec;
            const uint64_t size = std::filesystem::file_size(path_, ec);
            if (ec)
                return AppendResult::None; // being replaced, say
            if (size < offset_)
                return AppendResult::Changed;
            caught_up = size == offset_;
            if (caught_up)
                return AppendResult::None;
            // A file written anew is read again, not followed: the bytes
            // before the offset must be the ones read, and the last row
            // must have ended (more of it would be taken for a new row).
            if (offset_ > 0 && (tail_.empty() || tail_.back() != '\n'))
                return AppendResult::Changed;
            std::ifstream in(path_, std::ios::binary);
            if (!in)
                return AppendResult::None;
            in.seekg(static_cast<std::streamoff>(offset_ - tail_.size()));
            std::string before(tail_.size(), '\0');
            in.read(before.data(), static_cast<std::streamsize>(before.size()));
            if (!in || before != tail_)
                return AppendResult::Changed;
            std::string appended(static_cast<size_t>(std::min<uint64_t>(size - offset_, MAX_APPENDED_BYTES)), '\0');
            in.read(appended.data(), static_cast<std::streamsize>(appended.size()));
            appended.resize(static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0)));
            // A row being written waits for its line break.
            appended.resize(wholeRecordsLength(appended, delimiter_));
            if (appended.empty())
                return AppendResult::None;

            std::unique_ptr<std::istream> stream = std::make_unique<std::istringstream>(appended);
            if (encoding_ != TextEncoding::Utf8)
                stream = openDecodingStream(std::move(stream), encoding_);
            if (skip_meta_lines_)
                stream = std::make_unique<MetaLineFilterStream>(std::move(stream));
            csv::CSVFormat format;
            format.delimiter(delimiter_).quote('"').no_header().variable_columns(csv::VariableColumnPolicy::KEEP);
            csv::CSVReader reader(std::move(stream), format);
            TableRow row;
            while (readCsvRow(reader, row))
                pending_.push_back(std::move(row));
            offset_ += appended.size();
            caught_up = offset_ == size;
            tail_ += appended.substr(appended.size() - std::min(appended.size(), CountingBuf::TAIL_BYTES));
            if (tail_.size() > CountingBuf::TAIL_BYTES)
                tail_.erase(0, tail_.size() - CountingBuf::TAIL_BYTES);
            if (!header_decided_ && next_row_ == 0)
                decideHeader();
            return pending_.empty() ? AppendResult::None : AppendResult::Appended;
        }
    };

    class CsvDataSource : public DataSource
    {
    private:
        std::filesystem::path path_;
        OpenOptions options_;
        bool skip_meta_lines_;
        TextEncoding encoding_;
        CsvLayout layout_; // set by openSheet()

    public:
        CsvDataSource(std::filesystem::path path, OpenOptions options)
            : path_(std::move(path)), options_(options),
              skip_meta_lines_(options.skip_meta_lines.value_or(isVcf(path_))),
              encoding_(options.encoding ? *options.encoding : detectFileEncoding(path_)) {}

        std::vector<std::string> sheetNames() override { return {utf8FileName(path_)}; }
        bool usesHeaderOption() const override { return true; }
        bool isTextFormat() const override { return true; }
        bool skipsMetaLines() const override { return skip_meta_lines_; }
        std::optional<TextEncoding> textEncoding() const override { return encoding_; }
        bool supportsEditing() const override { return true; }
        bool readsAppendedRows() const override { return followable(detectCompression(path_), encoding_); }

        std::unique_ptr<SheetSource> openSheet(const std::string &sheet_name) override
        {
            if (sheet_name != utf8FileName(path_))
                return nullptr;
            return std::make_unique<CsvSheetSource>(path_, options_, skip_meta_lines_, encoding_, layout_);
        }

        void saveEdits(const std::vector<SheetEdits> &edits, const std::filesystem::path &target,
                       std::stop_token stop) override
        {
            std::vector<SheetEdits::Row> rows;
            for (const SheetEdits &sheet : edits)
            {
                if (sheet.sheet != utf8FileName(path_))
                    throw std::runtime_error("No such sheet: " + sheet.sheet);
                rows.insert(rows.end(), sheet.rows.begin(), sheet.rows.end());
            }
            if (!rows.empty() && !layout_.known)
                throw std::runtime_error("The sheet has not been read");
            rewriteCsv(path_, target, encoding_, skip_meta_lines_, layout_, rows, stop);
        }
    };
} // namespace

std::unique_ptr<DataSource> openCsvDataSource(const std::filesystem::path &path, const OpenOptions &options)
{
    return std::make_unique<CsvDataSource>(path, options);
}

void registerCsvBackend(BackendRegistry &registry)
{
    registry.add({
        .name = "CSV/TSV",
        .probe = [](const std::filesystem::path &path, std::span<const std::byte> head) -> int
        {
            const std::string ext = lowerExtension(stripCompressionExtension(path));
            if (ext == ".csv" || ext == ".tsv" || ext == ".tab" || ext == ".vcf")
                return 100;
            if (ext == ".txt")
                return 50;
            // The decompressed contents are not inspected here.
            if (detectCompression(head) != Compression::None)
                return 1;
            // Fallback for unknown extensions: anything that looks like text.
            const bool binary = std::any_of(head.begin(), head.end(), [](std::byte b)
                                            { return b == std::byte{0}; });
            if (!binary)
                return 1;
            const TextEncoding encoding = detectTextEncoding(head);
            return encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE ? 1 : 0;
        },
        .open = [](const std::filesystem::path &path, const OpenOptions &options)
        { return openCsvDataSource(path, options); },
    });
}
