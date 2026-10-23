#include "xlsx_writer.hxx"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <zlib.h>

namespace
{
    // Sheet XML collected before it is compressed.
    constexpr size_t FLUSH_BYTES = 256 * 1024;

    // Excel's serial number of 1970-01-01. Serial numbers count days from
    // 1899-12-30, except before 1900-03-01: Excel counts a 1900-02-29.
    constexpr int64_t EPOCH_SERIAL = 25569;
    constexpr int64_t FIRST_DAY = -25567;      // 1900-01-01, serial 1
    constexpr int64_t LEAP_BUG_DAY = -25508;   // 1900-03-01, serial 61
    constexpr int64_t LAST_DAY = 2932896;      // 9999-12-31
    constexpr int64_t MICROS_PER_DAY = 86400LL * 1000000;

    int64_t daySerial(int64_t day)
    {
        return day + EPOCH_SERIAL - (day < LEAP_BUG_DAY ? 1 : 0);
    }

    // Cell formats in styles.xml.
    constexpr int STYLE_DATE = 1;
    constexpr int STYLE_DATETIME = 2;
    constexpr int STYLE_HEADER = 3;

    const char CONTENT_TYPES[] =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
        R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
        R"(<Default Extension="xml" ContentType="application/xml"/>)"
        R"(<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>)"
        R"(<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>)"
        R"(<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>)"
        R"(</Types>)";

    const char PACKAGE_RELS[] =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>)"
        R"(</Relationships>)";

    const char WORKBOOK_RELS[] =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>)"
        R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>)"
        R"(</Relationships>)";

    const char STYLES[] =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        R"(<numFmts count="2"><numFmt numFmtId="164" formatCode="yyyy-mm-dd"/>)"
        R"(<numFmt numFmtId="165" formatCode="yyyy-mm-dd hh:mm:ss"/></numFmts>)"
        R"(<fonts count="2"><font><sz val="11"/><name val="Calibri"/><family val="2"/></font>)"
        R"(<font><b/><sz val="11"/><name val="Calibri"/><family val="2"/></font></fonts>)"
        R"(<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>)"
        R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
        R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
        R"(<cellXfs count="4"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>)"
        R"(<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
        R"(<xf numFmtId="165" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
        R"(<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>)"
        R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>)"
        R"(</styleSheet>)";

    bool isHex(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    // Length of the valid UTF-8 sequence at `text[i]`, or 0.
    size_t utf8Length(std::string_view text, size_t i)
    {
        const auto byte = [&](size_t k)
        { return static_cast<unsigned char>(text[k]); };
        const unsigned char c = byte(i);
        size_t length = 0;
        uint32_t code = 0;
        if (c < 0x80)
            return 1;
        if (c >= 0xC2 && c <= 0xDF)
            length = 2, code = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            length = 3, code = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            length = 4, code = c & 0x07;
        else
            return 0;
        if (i + length > text.size())
            return 0;
        for (size_t k = 1; k < length; ++k)
        {
            if ((byte(i + k) & 0xC0) != 0x80)
                return 0;
            code = (code << 6) | (byte(i + k) & 0x3F);
        }
        // Overlong forms, surrogates, beyond U+10FFFF, and U+FFFE/U+FFFF,
        // which XML does not allow.
        static const uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (code < minimum[length] || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF || code == 0xFFFE ||
            code == 0xFFFF)
            return 0;
        return length;
    }

    // Escapes text for XML. Control characters, which XML cannot hold (and
    // CR, which it turns into LF), are written as Excel's _xHHHH_ (so a literal "_x0041_" escapes its "_"),
    // and invalid UTF-8 becomes U+FFFD.
    void appendXmlText(std::string &out, std::string_view text, size_t max_units = SIZE_MAX)
    {
        size_t units = 0; // UTF-16 code units, which Excel counts
        for (size_t i = 0; i < text.size();)
        {
            const char c = text[i];
            const size_t length = utf8Length(text, i);
            const size_t width = length == 4 ? 2 : 1;
            if (units + width > max_units)
                break;
            units += width;
            if (length == 0)
            {
                out += "\xEF\xBF\xBD";
                ++i;
                continue;
            }
            switch (c)
            {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '_':
                if (i + 6 < text.size() && text[i + 1] == 'x' && isHex(text[i + 2]) && isHex(text[i + 3]) &&
                    isHex(text[i + 4]) && isHex(text[i + 5]) && text[i + 6] == '_')
                    out += "_x005F_";
                else
                    out += c;
                break;
            default:
                // A raw CR would be read back as LF (XML line-end handling).
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n')
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "_x%04X_", static_cast<unsigned>(c));
                    out += buf;
                }
                else
                    out.append(text.substr(i, length));
            }
            i += length;
        }
    }

    void appendNumber(std::string &out, double value)
    {
        char buf[64];
        const auto result = std::to_chars(buf, buf + sizeof(buf), value);
        out.append(buf, result.ptr);
    }

    void put16(std::string &out, uint32_t value)
    {
        out += static_cast<char>(value & 0xFF);
        out += static_cast<char>((value >> 8) & 0xFF);
    }

    void put32(std::string &out, uint64_t value)
    {
        if (value > 0xFFFFFFFFu)
            throw std::runtime_error("The workbook is too large");
        put16(out, static_cast<uint32_t>(value & 0xFFFF));
        put16(out, static_cast<uint32_t>(value >> 16));
    }

    void put64(std::string &out, uint64_t value)
    {
        put32(out, value & 0xFFFFFFFFu);
        put32(out, value >> 32);
    }
} // namespace

// A ZIP archive written entry by entry, each deflated as it is written.
// Sizes follow each entry in a data descriptor, so nothing is seeked back.
// Beyond 4 GiB (a big sheet) the sizes and offsets take ZIP64's 8 bytes, as
// Go's archive/zip writes them: in the data descriptor and the central
// directory, the local header being written before the size is known.
class XlsxWriter::Zip
{
private:
    struct Entry
    {
        std::string name;
        uint32_t crc = 0;
        uint64_t compressed = 0;
        uint64_t size = 0;
        uint64_t offset = 0;

        bool zip64() const { return compressed > MAX32 || size > MAX32 || offset > MAX32; }
    };

    static constexpr uint64_t MAX32 = 0xFFFFFFFFu;
    // Flags: sizes in a data descriptor, UTF-8 names.
    static constexpr uint32_t FLAGS = 0x0008 | 0x0800;
    static constexpr uint32_t DEFLATE = 8;
    static constexpr uint32_t VERSION = 20;
    static constexpr uint32_t VERSION_ZIP64 = 45;
    // 1980-01-01 00:00, the earliest DOS date: the files carry no time.
    static constexpr uint32_t DOS_TIME = 0;
    static constexpr uint32_t DOS_DATE = (1 << 5) | 1;

    std::ofstream out_;
    uint64_t offset_ = 0;
    std::vector<Entry> entries_;
    z_stream z_{};
    bool deflating_ = false;
    std::vector<char> chunk_ = std::vector<char>(64 * 1024);

    void writeRaw(const std::string &bytes)
    {
        out_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out_)
            throw std::runtime_error("Failed to write the workbook");
        offset_ += bytes.size();
    }

    void deflateSome(const char *data, size_t size, int flush)
    {
        Entry &entry = entries_.back();
        z_.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
        z_.avail_in = static_cast<uInt>(size);
        do
        {
            z_.next_out = reinterpret_cast<Bytef *>(chunk_.data());
            z_.avail_out = static_cast<uInt>(chunk_.size());
            if (deflate(&z_, flush) == Z_STREAM_ERROR)
                throw std::runtime_error("Failed to compress the workbook");
            const size_t produced = chunk_.size() - z_.avail_out;
            out_.write(chunk_.data(), static_cast<std::streamsize>(produced));
            if (!out_)
                throw std::runtime_error("Failed to write the workbook");
            offset_ += produced;
            entry.compressed += produced;
        } while (z_.avail_out == 0);
    }

public:
    explicit Zip(const std::filesystem::path &path) : out_(path, std::ios::binary | std::ios::trunc)
    {
        if (!out_)
            throw std::runtime_error("Cannot create " + std::string(reinterpret_cast<const char *>(path.u8string().c_str())));
    }

    ~Zip()
    {
        if (deflating_)
            deflateEnd(&z_);
    }

    void begin(const std::string &name)
    {
        entries_.push_back({name, 0, 0, 0, offset_});
        std::string header;
        put32(header, 0x04034b50);
        put16(header, VERSION);
        put16(header, FLAGS);
        put16(header, DEFLATE);
        put16(header, DOS_TIME);
        put16(header, DOS_DATE);
        put32(header, 0); // CRC and sizes: in the data descriptor
        put32(header, 0);
        put32(header, 0);
        put16(header, static_cast<uint32_t>(name.size()));
        put16(header, 0);
        header += name;
        writeRaw(header);
        z_ = {};
        if (deflateInit2(&z_, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            throw std::runtime_error("Failed to compress the workbook");
        deflating_ = true;
    }

    void write(std::string_view data)
    {
        Entry &entry = entries_.back();
        // zlib takes at most 4 GiB at a time.
        while (!data.empty())
        {
            const size_t size = std::min<size_t>(data.size(), 1u << 30);
            entry.crc = static_cast<uint32_t>(crc32(entry.crc, reinterpret_cast<const Bytef *>(data.data()),
                                                    static_cast<uInt>(size)));
            entry.size += size;
            deflateSome(data.data(), size, Z_NO_FLUSH);
            data.remove_prefix(size);
        }
    }

    void end()
    {
        deflateSome(nullptr, 0, Z_FINISH);
        deflateEnd(&z_);
        deflating_ = false;
        const Entry &entry = entries_.back();
        std::string descriptor;
        put32(descriptor, 0x08074b50);
        put32(descriptor, entry.crc);
        if (entry.compressed > MAX32 || entry.size > MAX32)
        {
            put64(descriptor, entry.compressed);
            put64(descriptor, entry.size);
        }
        else
        {
            put32(descriptor, entry.compressed);
            put32(descriptor, entry.size);
        }
        writeRaw(descriptor);
    }

    void add(const std::string &name, std::string_view data)
    {
        begin(name);
        write(data);
        end();
    }

    void close()
    {
        const uint64_t directory_offset = offset_;
        std::string directory;
        for (const auto &entry : entries_)
        {
            const bool zip64 = entry.zip64();
            put32(directory, 0x02014b50);
            put16(directory, zip64 ? VERSION_ZIP64 : VERSION);
            put16(directory, zip64 ? VERSION_ZIP64 : VERSION);
            put16(directory, FLAGS);
            put16(directory, DEFLATE);
            put16(directory, DOS_TIME);
            put16(directory, DOS_DATE);
            put32(directory, entry.crc);
            // With ZIP64, all three are in the extra field instead.
            put32(directory, zip64 ? MAX32 : entry.compressed);
            put32(directory, zip64 ? MAX32 : entry.size);
            put16(directory, static_cast<uint32_t>(entry.name.size()));
            put16(directory, zip64 ? 28 : 0); // extra field
            put16(directory, 0);              // comment
            put16(directory, 0);              // disk
            put16(directory, 0);              // internal attributes
            put32(directory, 0);              // external attributes
            put32(directory, zip64 ? MAX32 : entry.offset);
            directory += entry.name;
            if (zip64)
            {
                put16(directory, 0x0001);
                put16(directory, 24);
                put64(directory, entry.size);
                put64(directory, entry.compressed);
                put64(directory, entry.offset);
            }
        }
        writeRaw(directory);
        const uint64_t directory_size = directory.size();
        const bool zip64 = directory_offset > MAX32 || directory_size > MAX32 || entries_.size() > 0xFFFF;
        std::string end;
        if (zip64)
        {
            const uint64_t record_offset = offset_;
            put32(end, 0x06064b50); // ZIP64 end of central directory record
            put64(end, 44);         // its size after this field
            put16(end, VERSION_ZIP64);
            put16(end, VERSION_ZIP64);
            put32(end, 0);
            put32(end, 0);
            put64(end, entries_.size());
            put64(end, entries_.size());
            put64(end, directory_size);
            put64(end, directory_offset);
            put32(end, 0x07064b50); // its locator
            put32(end, 0);
            put64(end, record_offset);
            put32(end, 1);
        }
        put32(end, 0x06054b50);
        put16(end, 0);
        put16(end, 0);
        put16(end, static_cast<uint32_t>(std::min<size_t>(entries_.size(), 0xFFFF)));
        put16(end, static_cast<uint32_t>(std::min<size_t>(entries_.size(), 0xFFFF)));
        put32(end, zip64 ? MAX32 : directory_size);
        put32(end, zip64 ? MAX32 : directory_offset);
        put16(end, 0);
        writeRaw(end);
        out_.close();
        if (!out_)
            throw std::runtime_error("Failed to write the workbook");
    }
};

XlsxWriter::XlsxWriter(const std::filesystem::path &path, const std::string &sheet_name, bool header)
    : zip_(std::make_unique<Zip>(path)), header_(header)
{
    zip_->add("[Content_Types].xml", CONTENT_TYPES);
    zip_->add("_rels/.rels", PACKAGE_RELS);
    std::string workbook =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" )"
        R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)"
        R"(<sheets><sheet name=")";
    std::string name;
    appendXmlText(name, xlsxSheetName(sheet_name));
    for (const char c : name)
        workbook += c == '"' ? std::string("&quot;") : std::string(1, c);
    workbook += R"(" sheetId="1" r:id="rId1"/></sheets></workbook>)";
    zip_->add("xl/workbook.xml", workbook);
    zip_->add("xl/_rels/workbook.xml.rels", WORKBOOK_RELS);
    zip_->add("xl/styles.xml", STYLES);

    // The sheet goes last, as it is written row by row.
    zip_->begin("xl/worksheets/sheet1.xml");
    buffer_ =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)";
    if (header_)
        buffer_ += R"(<sheetViews><sheetView workbookViewId="0">)"
                   R"(<pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>)"
                   R"(</sheetView></sheetViews>)";
    buffer_ += "<sheetData>";
}

XlsxWriter::~XlsxWriter() = default;

void XlsxWriter::flush()
{
    zip_->write(buffer_);
    buffer_.clear();
}

void XlsxWriter::addRow(std::span<const CellValue> cells)
{
    if (rows_ >= MAX_ROWS)
        throw std::runtime_error("An Excel sheet holds at most 1,048,576 rows");
    if (cells.size() > MAX_COLUMNS)
        throw std::runtime_error("An Excel sheet holds at most 16,384 columns");
    ++rows_;
    const std::string row_number = std::to_string(rows_);
    const bool header = header_ && rows_ == 1;
    buffer_ += "<row r=\"" + row_number + "\">";
    for (size_t c = 0; c < cells.size(); ++c)
    {
        const CellValue &value = cells[c];
        if (std::holds_alternative<std::monostate>(value))
            continue;
        while (letters_.size() <= c)
            letters_.push_back(columnLetter(letters_.size()));
        const std::string ref = letters_[c] + row_number;
        auto text = [&](std::string_view s)
        {
            buffer_ += "<c r=\"" + ref + "\" t=\"inlineStr\"";
            if (header)
                buffer_ += " s=\"" + std::to_string(STYLE_HEADER) + "\"";
            buffer_ += "><is><t xml:space=\"preserve\">";
            appendXmlText(buffer_, s, MAX_TEXT);
            buffer_ += "</t></is></c>";
        };
        auto number = [&](double v, int style)
        {
            buffer_ += "<c r=\"" + ref + "\"";
            if (style)
                buffer_ += " s=\"" + std::to_string(style) + "\"";
            buffer_ += "><v>";
            appendNumber(buffer_, v);
            buffer_ += "</v></c>";
        };
        if (const auto *s = std::get_if<std::string>(&value))
            text(*s);
        else if (const auto *i = std::get_if<int64_t>(&value))
        {
            // Excel keeps 15 digits of a number: longer integers (IDs, card
            // numbers) would lose their last digits.
            if (*i > MAX_EXACT_INTEGER || *i < -MAX_EXACT_INTEGER)
                text(std::to_string(*i));
            else
                buffer_ += "<c r=\"" + ref + "\"><v>" + std::to_string(*i) + "</v></c>";
        }
        else if (const auto *d = std::get_if<double>(&value))
        {
            if (std::isfinite(*d))
                number(*d, 0);
            else
                text(formatCell(value));
        }
        else if (const auto *b = std::get_if<bool>(&value))
            buffer_ += "<c r=\"" + ref + "\" t=\"b\"><v>" + (*b ? "1" : "0") + "</v></c>";
        else if (const auto *date = std::get_if<Date>(&value))
        {
            if (date->days_since_epoch >= FIRST_DAY && date->days_since_epoch <= LAST_DAY)
                number(static_cast<double>(daySerial(date->days_since_epoch)), STYLE_DATE);
            else
                text(formatCell(value));
        }
        else if (const auto *time = std::get_if<DateTime>(&value))
        {
            const int64_t micros = time->micros_since_epoch;
            const int64_t day = micros >= 0 ? micros / MICROS_PER_DAY : -((-micros - 1) / MICROS_PER_DAY) - 1;
            if (day >= FIRST_DAY && day <= LAST_DAY)
                number(static_cast<double>(daySerial(day)) +
                           static_cast<double>(micros - day * MICROS_PER_DAY) / static_cast<double>(MICROS_PER_DAY),
                       STYLE_DATETIME);
            else
                text(formatCell(value));
        }
    }
    buffer_ += "</row>";
    if (buffer_.size() >= FLUSH_BYTES)
        flush();
}

void XlsxWriter::finish()
{
    if (finished_)
        return;
    buffer_ += "</sheetData></worksheet>";
    flush();
    zip_->end();
    zip_->close();
    finished_ = true;
}

std::string xlsxSheetName(const std::string &name)
{
    std::string result;
    size_t characters = 0;
    for (size_t i = 0; i < name.size();)
    {
        const size_t length = utf8Length(name, i);
        if (length == 0)
        {
            ++i;
            continue;
        }
        // Excel counts UTF-16 code units.
        const size_t width = length == 4 ? 2 : 1;
        if (characters + width > 31)
            break;
        characters += width;
        const char c = name[i];
        if (length == 1 && (std::string_view("[]:*?/\\").find(c) != std::string_view::npos ||
                            static_cast<unsigned char>(c) < 0x20))
            result += '_';
        else
            result.append(name, i, length);
        i += length;
    }
    // Names cannot start or end with an apostrophe.
    while (!result.empty() && result.front() == '\'')
        result.erase(result.begin());
    while (!result.empty() && result.back() == '\'')
        result.pop_back();
    return result.empty() ? "Sheet1" : result;
}
