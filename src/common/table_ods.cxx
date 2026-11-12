#include "table_ods.hxx"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <deque>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <expat.h>

#include "file_util.hxx"
#include "zip_reader.hxx"

namespace
{
    // LibreOffice's largest sheet ("jumbo sheets"). Repeated rows and
    // columns past it are dropped, so that a tiny file cannot claim
    // billions of cells.
    constexpr size_t kMaxRows = size_t{1} << 24;
    constexpr size_t kMaxColumns = 16384;
    constexpr size_t kMaxMerges = size_t{1} << 20;
    // One row is held whole, and number-columns-repeated copies a cell's
    // text: a row larger than this (by RowBlock's estimate) is refused, so
    // that a long text repeated over a row cannot take gigabytes.
    constexpr size_t kMaxRowBytes = size_t{64} << 20;
    // Reads of a file are dear in a browser: parse in large pieces.
    constexpr size_t kParseChunk = 1 << 20;
    constexpr std::string_view kMimeType = "application/vnd.oasis.opendocument.spreadsheet";

    // Never in a namespace URI: it is no XML character.
    constexpr char kNamespaceSeparator = '\x01';
    constexpr std::string_view kOdfNamespace = "urn:oasis:names:tc:opendocument:xmlns:";

    enum class Ns : uint8_t
    {
        Other,
        Office,
        Table,
        Text,
        Drawing,
    };

    struct XmlName
    {
        Ns ns = Ns::Other;
        std::string_view local;
    };

    XmlName splitName(const XML_Char *name)
    {
        const std::string_view full(name);
        const size_t separator = full.find(kNamespaceSeparator);
        if (separator == std::string_view::npos)
            return {Ns::Other, full};
        std::string_view uri = full.substr(0, separator);
        XmlName result{Ns::Other, full.substr(separator + 1)};
        if (uri.starts_with(kOdfNamespace))
        {
            uri.remove_prefix(kOdfNamespace.size());
            if (uri == "table:1.0")
                result.ns = Ns::Table;
            else if (uri == "office:1.0")
                result.ns = Ns::Office;
            else if (uri == "text:1.0")
                result.ns = Ns::Text;
            else if (uri == "drawing:1.0")
                result.ns = Ns::Drawing;
        }
        return result;
    }

    // number-columns-repeated and the like; anything but a positive count is 1.
    size_t parseCount(std::string_view text)
    {
        size_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size() || value == 0)
            return 1;
        return value;
    }

    // office:value: integers (as LibreOffice writes them, without a point)
    // stay integers, like the integers of an .xlsx.
    CellValue numberValue(std::string_view text)
    {
        int64_t integer = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), integer);
        if (result.ec == std::errc() && result.ptr == text.data() + text.size())
            return integer;
        if (const auto number = parseDouble(text))
            return *number;
        return std::string(text);
    }

    // office:date-value: an xsd:date or xsd:dateTime.
    std::optional<CellValue> dateValue(std::string_view text)
    {
        const auto temporal = parseTemporal(text);
        if (!temporal)
            return std::nullopt;
        if (temporal->has_time)
            return DateTime{temporal->micros};
        constexpr int64_t micros_per_day = 86400LL * 1000000;
        return Date{static_cast<int32_t>(temporal->micros / micros_per_day)};
    }

    // office:time-value: an ISO 8601 duration such as PT12H30M15.25S, shown
    // as a time ("12:30:15.250"), hours past a day included ("50:03:04").
    std::optional<std::string> timeValue(std::string_view text)
    {
        bool negative = false;
        if (!text.empty() && text.front() == '-')
        {
            negative = true;
            text.remove_prefix(1);
        }
        if (text.empty() || text.front() != 'P')
            return std::nullopt;
        text.remove_prefix(1);
        bool in_time = false;
        bool any = false;
        long long millis = 0;
        constexpr long long kMaxMillis = 1'000'000'000LL * 1000; // a sane bound against overflow
        while (!text.empty())
        {
            if (text.front() == 'T')
            {
                if (in_time)
                    return std::nullopt;
                in_time = true;
                text.remove_prefix(1);
                continue;
            }
            long long whole = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), whole);
            if (result.ec != std::errc() || whole < 0 || whole > kMaxMillis)
                return std::nullopt;
            text.remove_prefix(static_cast<size_t>(result.ptr - text.data()));
            long long fraction_millis = 0;
            if (!text.empty() && (text.front() == '.' || text.front() == ','))
            {
                text.remove_prefix(1);
                // Milliseconds, rounded on the fourth digit; further digits are dropped.
                long long scale = 100;
                size_t digits = 0;
                while (!text.empty() && text.front() >= '0' && text.front() <= '9')
                {
                    const int digit = text.front() - '0';
                    if (digits < 3)
                        fraction_millis += digit * scale;
                    else if (digits == 3 && digit >= 5)
                        ++fraction_millis;
                    scale /= 10;
                    ++digits;
                    text.remove_prefix(1);
                }
                if (digits == 0)
                    return std::nullopt;
            }
            if (text.empty())
                return std::nullopt;
            const char unit = text.front();
            text.remove_prefix(1);
            long long unit_millis = 0;
            if (!in_time && unit == 'D')
                unit_millis = 86400LL * 1000;
            else if (in_time && unit == 'H')
                unit_millis = 3600LL * 1000;
            else if (in_time && unit == 'M')
                unit_millis = 60LL * 1000;
            else if (in_time && unit == 'S')
                unit_millis = 1000;
            else
                return std::nullopt;
            if (fraction_millis != 0 && unit != 'S')
                return std::nullopt;
            millis += whole * unit_millis + fraction_millis;
            if (millis > kMaxMillis)
                return std::nullopt;
            any = true;
        }
        if (!any)
            return std::nullopt;
        const long long seconds = millis / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld", negative && millis != 0 ? "-" : "", seconds / 3600,
                      seconds / 60 % 60, seconds % 60);
        std::string result = buf;
        if (millis % 1000 != 0)
        {
            std::snprintf(buf, sizeof(buf), ".%03lld", millis % 1000);
            result += buf;
        }
        return result;
    }

    // What a pass over content.xml is told.
    class ContentListener
    {
    public:
        virtual ~ContentListener() = default;
        // A sheet (table:table) starts; returns whether its rows are wanted.
        virtual bool beginSheet(size_t index, std::string_view name) = 0;
        // `repeat` rows equal to `row` (empty: empty rows) start at sheet
        // row `index`; rows come in order, without gaps. `row` may be taken.
        // Returns whether to go on: after false, parsing pauses until the
        // next ContentParser::resume().
        virtual bool row(TableRow &row, size_t index, size_t repeat) = 0;
        // A merged range, in sheet rows (before the row it starts in).
        virtual void merge(const CellRange &range) = 0;
        virtual void endSheet() = 0;
    };

    // Streams content.xml into a ContentListener.
    class ContentParser
    {
    public:
        explicit ContentParser(ContentListener &listener)
            : parser_(XML_ParserCreateNS(nullptr, kNamespaceSeparator)), listener_(listener)
        {
            if (!parser_)
                throw std::bad_alloc();
            XML_SetUserData(parser_, this);
            XML_SetElementHandler(parser_, &ContentParser::onStart, &ContentParser::onEnd);
            XML_SetCharacterDataHandler(parser_, &ContentParser::onText);
            // A document has no DTD; refusing one keeps entity expansion out.
            XML_SetStartDoctypeDeclHandler(parser_, &ContentParser::onDoctype);
        }
        ~ContentParser() { XML_ParserFree(parser_); }
        ContentParser(const ContentParser &) = delete;
        ContentParser &operator=(const ContentParser &) = delete;

        // Parses the next piece of the document (`last`: its end), which
        // must stay as it is while the parser is paused. Throws
        // std::runtime_error.
        void parse(const char *data, size_t size, bool last)
        {
            check(XML_Parse(parser_, data, static_cast<int>(size), last ? XML_TRUE : XML_FALSE));
        }

        // Whether a listener paused the parser inside the last piece.
        bool paused() const { return paused_; }

        // Goes on with the piece the parser was paused in.
        void resume() { check(XML_ResumeParser(parser_)); }

    private:
        struct CellAttributes
        {
            std::string value_type;
            std::string value;
            std::string date_value;
            std::string time_value;
            std::string boolean_value;
            std::string string_value;
            bool has_string_value = false;
            size_t columns_repeated = 1;
            size_t columns_spanned = 1;
            size_t rows_spanned = 1;
        };

        XML_Parser parser_;
        ContentListener &listener_;
        std::exception_ptr exception_;
        std::string error_;
        bool paused_ = false;

        size_t depth_ = 0;
        // The depth of the open element of each kind; 0 when none is open.
        size_t spreadsheet_depth_ = 0;
        size_t sheet_depth_ = 0;
        size_t row_depth_ = 0;
        size_t cell_depth_ = 0;
        size_t paragraph_depth_ = 0;
        size_t skip_depth_ = 0; // a subtree ignored entirely
        size_t sheet_count_ = 0;
        size_t row_index_ = 0;
        size_t merge_count_ = 0;

        TableRow row_;
        size_t row_bytes_ = 0; // of row_'s cells
        size_t row_repeat_ = 1;
        size_t column_ = 0;
        size_t pending_empty_ = 0; // empty cells not added yet: the row may end
        std::vector<CellRange> row_merges_;

        CellAttributes cell_;
        std::string text_;
        bool has_paragraph_ = false;
        bool paragraph_has_text_ = false;
        bool space_pending_ = false;

        void check(XML_Status status)
        {
            paused_ = status == XML_STATUS_SUSPENDED;
            if (status != XML_STATUS_ERROR)
                return;
            if (exception_)
                std::rethrow_exception(exception_);
            if (!error_.empty())
                throw std::runtime_error(error_);
            throw std::runtime_error("Cannot read the ODS file: " +
                                     std::string(XML_ErrorString(XML_GetErrorCode(parser_))) + " at line " +
                                     std::to_string(XML_GetCurrentLineNumber64(parser_)) + " of content.xml");
        }

        template <typename F>
        void guarded(F &&f)
        {
            if (exception_)
                return;
            try
            {
                f();
            }
            catch (...)
            {
                // Exceptions must not unwind through expat's C frames.
                exception_ = std::current_exception();
                XML_StopParser(parser_, XML_FALSE);
            }
        }

        static void XMLCALL onStart(void *data, const XML_Char *name, const XML_Char **attributes)
        {
            auto *self = static_cast<ContentParser *>(data);
            self->guarded([&]
                          { self->start(name, attributes); });
        }
        static void XMLCALL onEnd(void *data, const XML_Char *)
        {
            auto *self = static_cast<ContentParser *>(data);
            self->guarded([&]
                          { self->end(); });
        }
        static void XMLCALL onText(void *data, const XML_Char *text, int size)
        {
            auto *self = static_cast<ContentParser *>(data);
            if (self->cell_depth_ && self->paragraph_depth_ && !self->skip_depth_)
                self->guarded([&]
                              { self->addText(std::string_view(text, static_cast<size_t>(size))); });
        }
        static void XMLCALL onDoctype(void *data, const XML_Char *, const XML_Char *, const XML_Char *, int)
        {
            auto *self = static_cast<ContentParser *>(data);
            self->error_ = "Cannot read the ODS file: content.xml has a document type declaration";
            XML_StopParser(self->parser_, XML_FALSE);
        }

        void start(const XML_Char *name, const XML_Char **attributes)
        {
            ++depth_;
            if (skip_depth_)
                return;
            const XmlName element = splitName(name);
            if (cell_depth_)
            {
                // Comments and shapes anchored to the cell are not its text.
                if ((element.ns == Ns::Office && element.local == "annotation") || element.ns == Ns::Drawing ||
                    (element.ns == Ns::Text && element.local == "note"))
                {
                    skip_depth_ = depth_;
                    return;
                }
                if (element.ns != Ns::Text)
                    return;
                if (!paragraph_depth_)
                {
                    if (element.local == "p" || element.local == "h")
                    {
                        if (has_paragraph_)
                            text_ += '\n';
                        has_paragraph_ = true;
                        paragraph_has_text_ = false;
                        space_pending_ = false;
                        paragraph_depth_ = depth_;
                    }
                }
                else if (element.local == "s")
                {
                    size_t count = 1;
                    for (const XML_Char **a = attributes; *a; a += 2)
                    {
                        const XmlName attribute = splitName(a[0]);
                        if (attribute.ns == Ns::Text && attribute.local == "c")
                            count = std::min<size_t>(parseCount(a[1]), 65536);
                    }
                    addExplicit(std::string(count, ' '));
                }
                else if (element.local == "tab")
                    addExplicit("\t");
                else if (element.local == "line-break")
                    addExplicit("\n");
                return;
            }
            if (row_depth_)
            {
                if (element.ns == Ns::Table && (element.local == "table-cell" || element.local == "covered-table-cell"))
                    startCell(attributes);
                return;
            }
            if (sheet_depth_)
            {
                if (element.ns != Ns::Table)
                    return;
                if (element.local == "table-row")
                    startRow(attributes);
                else if (element.local == "table" || element.local == "shapes")
                    skip_depth_ = depth_;
                return;
            }
            if (spreadsheet_depth_)
            {
                if (element.ns == Ns::Table && element.local == "table" && depth_ == spreadsheet_depth_ + 1)
                    startSheet(attributes);
                return;
            }
            if (element.ns == Ns::Office && element.local == "spreadsheet")
                spreadsheet_depth_ = depth_;
        }

        void end()
        {
            if (skip_depth_)
            {
                if (depth_ == skip_depth_)
                    skip_depth_ = 0;
            }
            else if (depth_ == paragraph_depth_)
            {
                // A run of spaces at the end stays one space, as LibreOffice
                // reads it (and writes a single trailing space as is).
                if (space_pending_)
                    text_ += ' ';
                space_pending_ = false;
                paragraph_depth_ = 0;
            }
            else if (depth_ == cell_depth_)
            {
                endCell();
                cell_depth_ = 0;
            }
            else if (depth_ == row_depth_)
            {
                endRow();
                row_depth_ = 0;
            }
            else if (depth_ == sheet_depth_)
            {
                sheet_depth_ = 0;
                listener_.endSheet();
            }
            else if (depth_ == spreadsheet_depth_)
                spreadsheet_depth_ = 0;
            --depth_;
        }

        void startSheet(const XML_Char **attributes)
        {
            std::string_view name;
            for (const XML_Char **a = attributes; *a; a += 2)
            {
                const XmlName attribute = splitName(a[0]);
                if (attribute.ns == Ns::Table && attribute.local == "name")
                    name = a[1];
            }
            if (!listener_.beginSheet(sheet_count_++, name))
            {
                skip_depth_ = depth_;
                return;
            }
            sheet_depth_ = depth_;
            row_index_ = 0;
            merge_count_ = 0;
        }

        void startRow(const XML_Char **attributes)
        {
            row_depth_ = depth_;
            row_.clear();
            row_bytes_ = 0;
            row_merges_.clear();
            column_ = 0;
            pending_empty_ = 0;
            row_repeat_ = 1;
            for (const XML_Char **a = attributes; *a; a += 2)
            {
                const XmlName attribute = splitName(a[0]);
                if (attribute.ns == Ns::Table && attribute.local == "number-rows-repeated")
                    row_repeat_ = parseCount(a[1]);
            }
        }

        void endRow()
        {
            const size_t repeat = row_index_ >= kMaxRows ? 0 : std::min(row_repeat_, kMaxRows - row_index_);
            if (repeat == 0)
                return;
            for (size_t r = 0; r < repeat && !row_merges_.empty(); ++r)
            {
                for (CellRange range : row_merges_)
                {
                    if (merge_count_ >= kMaxMerges)
                        break;
                    range.row = row_index_ + r;
                    listener_.merge(range);
                    ++merge_count_;
                }
                if (merge_count_ >= kMaxMerges)
                    break;
            }
            const bool go_on = listener_.row(row_, row_index_, repeat);
            row_index_ += repeat;
            // The rest of the piece waits in expat: without the pause, a
            // megabyte of XML would become all its rows at once, each as
            // wide as its repeated cells make it.
            if (!go_on)
                XML_StopParser(parser_, XML_TRUE);
        }

        void startCell(const XML_Char **attributes)
        {
            cell_depth_ = depth_;
            cell_.value_type.clear();
            cell_.value.clear();
            cell_.date_value.clear();
            cell_.time_value.clear();
            cell_.boolean_value.clear();
            cell_.string_value.clear();
            cell_.has_string_value = false;
            cell_.columns_repeated = cell_.columns_spanned = cell_.rows_spanned = 1;
            text_.clear();
            has_paragraph_ = false;
            for (const XML_Char **a = attributes; *a; a += 2)
            {
                const XmlName attribute = splitName(a[0]);
                const char *value = a[1];
                if (attribute.ns == Ns::Office)
                {
                    if (attribute.local == "value-type")
                        cell_.value_type = value;
                    else if (attribute.local == "value")
                        cell_.value = value;
                    else if (attribute.local == "date-value")
                        cell_.date_value = value;
                    else if (attribute.local == "time-value")
                        cell_.time_value = value;
                    else if (attribute.local == "boolean-value")
                        cell_.boolean_value = value;
                    else if (attribute.local == "string-value")
                    {
                        cell_.string_value = value;
                        cell_.has_string_value = true;
                    }
                }
                else if (attribute.ns == Ns::Table)
                {
                    if (attribute.local == "number-columns-repeated")
                        cell_.columns_repeated = parseCount(value);
                    else if (attribute.local == "number-columns-spanned")
                        cell_.columns_spanned = parseCount(value);
                    else if (attribute.local == "number-rows-spanned")
                        cell_.rows_spanned = parseCount(value);
                }
            }
        }

        // ODF collapses runs of white space in paragraphs to one space and
        // drops it at their start; other spaces are written as text:s.
        void addText(std::string_view text)
        {
            for (const char c : text)
            {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    space_pending_ = paragraph_has_text_;
                    continue;
                }
                if (space_pending_)
                    text_ += ' ';
                space_pending_ = false;
                text_ += c;
                paragraph_has_text_ = true;
            }
        }

        void addExplicit(std::string_view text)
        {
            if (space_pending_)
                text_ += ' ';
            space_pending_ = false;
            text_ += text;
            paragraph_has_text_ = true;
        }

        CellValue cellValue()
        {
            const std::string &type = cell_.value_type;
            const bool has_text = has_paragraph_;
            auto text = [&]() -> CellValue
            {
                if (has_text)
                    return std::move(text_);
                return {};
            };
            if (type.empty())
                return has_text && !text_.empty() ? text() : CellValue{};
            if (type == "float" || type == "percentage" || type == "currency")
                return cell_.value.empty() ? text() : numberValue(cell_.value);
            if (type == "date")
            {
                if (auto value = dateValue(cell_.date_value))
                    return *value;
                return text();
            }
            if (type == "time")
            {
                if (auto value = timeValue(cell_.time_value))
                    return *value;
                return text();
            }
            if (type == "boolean")
            {
                if (cell_.boolean_value == "true")
                    return true;
                if (cell_.boolean_value == "false")
                    return false;
                return text();
            }
            // "string", or a type of a later version: the text shown.
            if (has_text)
                return text();
            if (cell_.has_string_value)
                return std::move(cell_.string_value);
            return {};
        }

        void endCell()
        {
            const size_t repeat = column_ >= kMaxColumns ? 0 : std::min(cell_.columns_repeated, kMaxColumns - column_);
            if (repeat == 0)
                return;
            if (cell_.columns_spanned > 1 || cell_.rows_spanned > 1)
            {
                for (size_t k = 0; k < repeat && row_merges_.size() < kMaxColumns; ++k)
                {
                    const size_t column = column_ + k;
                    row_merges_.push_back({.row = 0,
                                           .col = column,
                                           .rows = std::min(cell_.rows_spanned, kMaxRows),
                                           .cols = std::min(cell_.columns_spanned, kMaxColumns - column)});
                }
            }
            CellValue value = cellValue();
            if (std::holds_alternative<std::monostate>(value))
                pending_empty_ += repeat;
            else
            {
                // As RowBlock::approxBytes() counts a cell.
                size_t cell_bytes = sizeof(CellValue);
                if (const auto *text = std::get_if<std::string>(&value))
                    cell_bytes += text->capacity();
                if (cell_bytes > (kMaxRowBytes - row_bytes_) / repeat)
                    throw std::runtime_error("Cannot read the ODS file: row " + std::to_string(row_index_ + 1) +
                                             " of a sheet is too large");
                row_bytes_ += cell_bytes * repeat;
                row_.resize(row_.size() + pending_empty_);
                pending_empty_ = 0;
                row_.insert(row_.end(), repeat - 1, value);
                row_.push_back(std::move(value));
            }
            column_ += repeat;
        }
    };

    // Streams one entry of the archive through a parser.
    class ContentStream
    {
    public:
        ContentStream(const ZipArchive &archive, const ZipArchive::Entry &entry, ContentListener &listener)
            : reader_(archive, entry), parser_(listener), buffer_(kParseChunk)
        {
        }

        // Parses on, to the end of the next piece or until the listener
        // pauses; false once the whole document is parsed.
        bool step()
        {
            if (parser_.paused())
                parser_.resume();
            else
            {
                if (done_)
                    return false;
                const size_t got = reader_.read(buffer_.data(), buffer_.size());
                done_ = got == 0;
                parser_.parse(buffer_.data(), got, done_);
            }
            return parser_.paused() || !done_;
        }

    private:
        ZipEntryReader reader_;
        ContentParser parser_;
        std::vector<char> buffer_;
        bool done_ = false;
    };

    struct SheetInfo
    {
        std::string name;
        size_t rows = 0; // the last row with a value, or covered by a merge
        std::vector<CellRange> merged;
    };

    class IndexListener : public ContentListener
    {
    public:
        std::vector<SheetInfo> sheets;

        bool beginSheet(size_t, std::string_view name) override
        {
            sheets.emplace_back().name = name;
            return true;
        }
        bool row(TableRow &row, size_t index, size_t repeat) override
        {
            if (!row.empty())
                sheets.back().rows = std::max(sheets.back().rows, index + repeat);
            return true;
        }
        void merge(const CellRange &range) override
        {
            SheetInfo &sheet = sheets.back();
            sheet.merged.push_back(range);
            sheet.rows = std::max(sheet.rows, std::min(range.row + range.rows, kMaxRows));
        }
        void endSheet() override {}
    };

    struct OdsWorkbook
    {
        ZipArchive archive;
        const ZipArchive::Entry *content = nullptr;
        std::vector<SheetInfo> sheets;

        explicit OdsWorkbook(const std::filesystem::path &path) : archive(path) {}
    };

    class OdsSheetSource : public SheetSource, private ContentListener
    {
    private:
        struct Pending
        {
            TableRow row;
            size_t count = 0;
        };

        std::shared_ptr<const OdsWorkbook> book_;
        size_t index_;
        size_t total_rows_; // sheet rows shown, the header included
        // Closed at the end of the sheet: the file handle would keep the
        // file from being replaced on Windows while the sheet is shown.
        std::optional<ContentStream> stream_;
        std::deque<Pending> pending_;
        size_t available_ = 0; // rows in pending_
        size_t wanted_ = 0;    // rows fill() is parsing for
        bool sheet_done_ = false;
        size_t next_sheet_row_ = 0; // sheet rows taken from pending_ (or padded)
        size_t next_row_ = 0;       // next data row index
        bool header_ = false;
        std::vector<ColumnInfo> columns_;
        std::vector<CellRange> merged_;

        bool beginSheet(size_t index, std::string_view) override { return index == index_; }
        bool row(TableRow &row, size_t, size_t repeat) override
        {
            pending_.push_back({std::move(row), repeat});
            available_ += repeat;
            return available_ < wanted_;
        }
        void merge(const CellRange &) override {}
        void endSheet() override { sheet_done_ = true; }

        // Parses until `rows` rows are waiting, or the sheet has ended.
        void fill(size_t rows)
        {
            wanted_ = rows;
            while (available_ < rows && !sheet_done_ && stream_ && stream_->step())
            {
            }
            if (sheet_done_ || available_ < rows)
                stream_.reset(); // nothing more to come
        }

        // The next sheet row; past the rows parsed, an empty row (merged
        // ranges reaching past the last value).
        TableRow takeRow()
        {
            fill(1);
            ++next_sheet_row_;
            if (pending_.empty())
                return {};
            Pending &front = pending_.front();
            --available_;
            if (--front.count == 0)
            {
                TableRow row = std::move(front.row);
                pending_.pop_front();
                return row;
            }
            return front.row;
        }

        const TableRow *peek(size_t offset)
        {
            fill(offset + 1);
            for (const Pending &pending : pending_)
            {
                if (offset < pending.count)
                    return &pending.row;
                offset -= pending.count;
            }
            return nullptr;
        }

    public:
        OdsSheetSource(std::shared_ptr<const OdsWorkbook> book, size_t index, const OpenOptions &options)
            : book_(std::move(book)), index_(index), total_rows_(book_->sheets[index].rows),
              stream_(std::in_place, book_->archive, *book_->content, static_cast<ContentListener &>(*this))
        {
            if (options.header != OpenOptions::Header::No && total_rows_ > 0)
            {
                const TableRow *first = peek(0);
                if (first && !first->empty())
                {
                    const TableRow *second = total_rows_ > 1 ? peek(1) : nullptr;
                    static const TableRow empty;
                    if (total_rows_ > 1 && !second)
                        second = &empty;
                    if (options.header == OpenOptions::Header::Yes || looksLikeHeaderRow(*first, second))
                    {
                        header_ = true;
                        for (const auto &cell : *peek(0))
                            columns_.push_back({formatCell(cell)});
                        takeRow();
                    }
                }
            }
            const size_t header_rows = header_ ? 1 : 0;
            for (const CellRange &range : book_->sheets[index].merged)
            {
                if (range.row < header_rows)
                    continue; // merges inside the header row are not data
                CellRange shifted = range;
                shifted.row -= header_rows;
                merged_.push_back(shifted);
            }
        }

        SourceCapabilities capabilities() const override
        {
            return {.random_access = false, .query_pushdown = false};
        }
        std::vector<ColumnInfo> columns() override { return columns_; }
        RowCount rowCount() override { return {total_rows_ - (header_ ? 1 : 0), RowCount::Kind::Exact}; }
        std::vector<CellRange> mergedCells() override { return merged_; }

        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token) override
        {
            if (first_row != next_row_)
                throw std::logic_error("ODS source must be read sequentially");
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            block->rows.reserve(std::min(max_rows, total_rows_ - next_sheet_row_));
            while (block->rows.size() < max_rows && next_sheet_row_ < total_rows_)
                block->rows.push_back(takeRow());
            next_row_ += block->rows.size();
            // The rows counted when the file was opened are all taken: only
            // empty rows can follow, so the end tag need not be parsed.
            if (next_sheet_row_ >= total_rows_)
            {
                stream_.reset();
                pending_.clear();
                available_ = 0;
            }
            return block;
        }
    };

    class OdsDataSource : public DataSource
    {
    private:
        std::shared_ptr<OdsWorkbook> book_;
        OpenOptions options_;
        std::vector<std::string> sheet_names_;

    public:
        OdsDataSource(const std::filesystem::path &path, const OpenOptions &options)
            : book_(std::make_shared<OdsWorkbook>(path)), options_(options)
        {
            const ZipArchive &archive = book_->archive;
            if (const auto *mimetype = archive.find("mimetype"))
            {
                const std::string type = ZipEntryReader(archive, *mimetype).readAll(4096);
                // Templates (.ots) hold a spreadsheet too.
                if (type != kMimeType && type != std::string(kMimeType) + "-template")
                    throw std::runtime_error("Not an OpenDocument spreadsheet: " + type);
            }
            // ODF encrypts the entries itself, which ZIP does not know of;
            // the manifest tells.
            if (const auto *manifest = archive.find("META-INF/manifest.xml"))
            {
                const std::string text = ZipEntryReader(archive, *manifest).readAll(16 << 20);
                if (text.find("encryption-data") != std::string::npos)
                    throw std::runtime_error("Password-protected ODS files are not supported");
            }
            book_->content = archive.find("content.xml");
            if (!book_->content)
                throw std::runtime_error("Not an OpenDocument spreadsheet: content.xml is missing");

            IndexListener index;
            ContentStream stream(archive, *book_->content, index);
            while (stream.step())
            {
            }
            book_->sheets = std::move(index.sheets);
            for (const SheetInfo &sheet : book_->sheets)
                sheet_names_.push_back(sheet.name);
        }

        std::vector<std::string> sheetNames() override { return sheet_names_; }
        bool usesHeaderOption() const override { return true; }
        // Rows are read as they are asked for and repeated rows are small
        // in the file, so a sheet can take far more memory than its file.
        bool pausesAtMemoryCheckpoints() const override { return true; }

        std::unique_ptr<SheetSource> openSheet(const std::string &sheet_name) override
        {
            const auto it = std::find(sheet_names_.begin(), sheet_names_.end(), sheet_name);
            if (it == sheet_names_.end())
                return nullptr;
            return std::make_unique<OdsSheetSource>(book_, static_cast<size_t>(it - sheet_names_.begin()), options_);
        }
    };
} // namespace

std::unique_ptr<DataSource> openOdsDataSource(const std::filesystem::path &path, const OpenOptions &options)
{
    return std::make_unique<OdsDataSource>(path, options);
}

void registerOdsBackend(BackendRegistry &registry)
{
    registry.add({
        .name = "ODS",
        .probe = [](const std::filesystem::path &path, std::span<const std::byte> head) -> int
        {
            const bool zip = head.size() >= 4 && head[0] == std::byte{'P'} && head[1] == std::byte{'K'} &&
                             head[2] == std::byte{3} && head[3] == std::byte{4};
            if (!zip)
                return 0;
            // ODF puts an uncompressed "mimetype" entry first, so that the
            // type can be told from the leading bytes.
            constexpr size_t kNameOffset = 30;
            const std::string_view name = "mimetype";
            const auto bytes = [&](size_t offset, size_t size)
            {
                return std::string_view(reinterpret_cast<const char *>(head.data()) + offset, size);
            };
            if (head.size() >= kNameOffset + name.size() + kMimeType.size() && bytes(kNameOffset, name.size()) == name)
            {
                const size_t extra = static_cast<size_t>(head[28]) | (static_cast<size_t>(head[29]) << 8);
                const size_t value = kNameOffset + name.size() + extra;
                if (head.size() >= value + kMimeType.size() && bytes(value, kMimeType.size()) == kMimeType)
                    return 100;
            }
            const std::string ext = lowerExtension(path);
            return ext == ".ods" || ext == ".ots" ? 50 : 0;
        },
        .open = [](const std::filesystem::path &path, const OpenOptions &options)
        { return openOdsDataSource(path, options); },
    });
}
