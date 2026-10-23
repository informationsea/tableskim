#include "table_excel.hxx"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <stdexcept>

#include <OpenXLSX.hpp>
#include <XLXmlParser.hpp>
#include <pugixml.hpp>

namespace
{
    // Serial number of 1970-01-01 in the 1900 date system.
    constexpr double kUnixEpochSerial = 25569.0;

    enum class DateKind : uint8_t
    {
        None,
        Date,
        DateTime,
        Time,    // time of day, e.g. "hh:mm:ss"; shown as text
        Elapsed, // elapsed time, e.g. "[h]:mm:ss"; shown as text
    };

    std::string lowerExtension(const std::filesystem::path &path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    // Built-in number formats (ECMA-376 18.8.30), including the CJK ones.
    DateKind builtinDateKind(uint32_t id)
    {
        if ((id >= 14 && id <= 17) || (id >= 27 && id <= 31) || id == 36 || (id >= 50 && id <= 58))
            return DateKind::Date;
        if (id == 22)
            return DateKind::DateTime;
        if (id == 46)
            return DateKind::Elapsed; // [h]:mm:ss
        if ((id >= 18 && id <= 21) || (id >= 32 && id <= 35) || id == 45 || id == 47)
            return DateKind::Time;
        return DateKind::None;
    }

    // Classifies a custom format code such as "yyyy/mm/dd hh:mm" by the
    // date/time tokens outside of quoted text, escapes and [color] sections.
    DateKind customDateKind(const std::string &code)
    {
        bool date = false, time = false, elapsed = false, month_or_minute = false;
        for (size_t i = 0; i < code.size(); ++i)
        {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(code[i])));
            if (c == ';')
                break; // only the first section matters
            if (c == '"')
            {
                i = code.find('"', i + 1);
                if (i == std::string::npos)
                    break;
            }
            else if (c == '\\' || c == '_' || c == '*')
            {
                ++i;
            }
            else if (c == '[')
            {
                const size_t close = code.find(']', i);
                if (close == std::string::npos)
                    break;
                const std::string inner = code.substr(i + 1, close - i - 1);
                // [h], [mm], [ss]: elapsed time
                if (!inner.empty() && inner.find_first_not_of("hHmMsS") == std::string::npos)
                    elapsed = true;
                i = close;
            }
            else if (c == 'y' || c == 'd')
            {
                date = true;
            }
            else if (c == 'h' || c == 's')
            {
                time = true;
            }
            else if (c == 'm')
            {
                month_or_minute = true;
            }
        }
        if (elapsed)
            return DateKind::Elapsed;
        if (month_or_minute && !time)
            date = true;
        if (date && time)
            return DateKind::DateTime;
        if (date)
            return DateKind::Date;
        if (time)
            return DateKind::Time;
        return DateKind::None;
    }

    constexpr long long kMillisPerDay = 86400LL * 1000;

    // Serials are doubles; Excel itself only keeps millisecond precision, so
    // round there to avoid values like 09:30:14.999999.
    long long serialToMillis(double serial)
    {
        return std::llround(serial * static_cast<double>(kMillisPerDay));
    }

    long long floorDiv(long long a, long long b)
    {
        return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
    }

    // "h:mm:ss" with a total hour count, e.g. 50:03:04 for [h]:mm:ss.
    std::string formatElapsed(double serial)
    {
        const long long total = serialToMillis(serial);
        const long long millis = total < 0 ? -total : total;
        const long long seconds = millis / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%lld:%02lld:%02lld", total < 0 ? "-" : "", seconds / 3600, seconds / 60 % 60,
                      seconds % 60);
        std::string result = buf;
        if (millis % 1000 != 0)
        {
            std::snprintf(buf, sizeof(buf), ".%03lld", millis % 1000);
            result += buf;
        }
        return result;
    }

    // "hh:mm:ss" of the time of day (the date part is ignored, as Excel does).
    std::string formatTimeOfDay(double serial)
    {
        const long long total = serialToMillis(serial);
        const long long millis = total - floorDiv(total, kMillisPerDay) * kMillisPerDay;
        const long long seconds = millis / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
        std::string result = buf;
        if (millis % 1000 != 0)
        {
            std::snprintf(buf, sizeof(buf), ".%03lld", millis % 1000);
            result += buf;
        }
        return result;
    }

    struct ExcelWorkbook
    {
        OpenXLSX::XLDocument document;
        std::vector<DateKind> style_kinds; // indexed by cell format (xf) index
        double unix_epoch_serial = kUnixEpochSerial;
        bool date1904 = false;
        std::map<std::string, bool> header_rows; // sheets opened: whether the first row was the header
    };

    // Milliseconds since 1970-01-01 for a date serial.
    long long serialToUnixMillis(double serial, const ExcelWorkbook &book)
    {
        // The 1900 system counts a nonexistent 1900-02-29 (serial 60), so
        // serials 1..59 (1900-01-01..1900-02-28) are one day too small.
        if (!book.date1904 && serial >= 1.0 && serial < 60.0)
            serial += 1.0;
        return serialToMillis(serial - book.unix_epoch_serial);
    }

    // The serial of a date-time; the reverse of serialToUnixMillis().
    double unixMicrosToSerial(long long micros, const ExcelWorkbook &book)
    {
        double serial = static_cast<double>(micros) / (kMillisPerDay * 1000.0) + book.unix_epoch_serial;
        if (!book.date1904 && serial >= 2.0 && serial < 61.0)
            serial -= 1.0;
        return serial;
    }

    bool isEscapePattern(std::string_view text, size_t pos)
    {
        if (pos + 6 >= text.size() || text[pos] != '_' || text[pos + 1] != 'x' || text[pos + 6] != '_')
            return false;
        return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(pos) + 2, text.begin() + static_cast<std::ptrdiff_t>(pos) + 6,
                           [](char c)
                           { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
    }

    // The reverse of decodeEscapes(): control characters other than tab and
    // line breaks, and "_" before an escape-like pattern, become _xHHHH_.
    std::string encodeEscapes(std::string_view text)
    {
        std::string result;
        result.reserve(text.size());
        for (size_t pos = 0; pos < text.size(); ++pos)
        {
            const auto c = static_cast<unsigned char>(text[pos]);
            if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || isEscapePattern(text, pos))
            {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "_x%04X_", c);
                result += buf;
            }
            else
                result += static_cast<char>(c);
        }
        return result;
    }

    // Excel stores characters that XML cannot hold (and "_" before such a
    // pattern) as _xHHHH_.
    std::string decodeEscapes(std::string text)
    {
        size_t pos = text.find("_x");
        if (pos == std::string::npos)
            return text;
        auto hex = [](char c) -> int
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        std::string result = text.substr(0, pos);
        while (pos < text.size())
        {
            if (pos + 6 < text.size() && text[pos] == '_' && text[pos + 1] == 'x' && text[pos + 6] == '_' &&
                hex(text[pos + 2]) >= 0 && hex(text[pos + 3]) >= 0 && hex(text[pos + 4]) >= 0 && hex(text[pos + 5]) >= 0)
            {
                const unsigned code = (hex(text[pos + 2]) << 12) | (hex(text[pos + 3]) << 8) |
                                      (hex(text[pos + 4]) << 4) | hex(text[pos + 5]);
                // UTF-8 of a BMP code point; lone surrogates are left as they are.
                if (code >= 0xD800 && code <= 0xDFFF)
                    result.append(text, pos, 7);
                else if (code < 0x80)
                    result += static_cast<char>(code);
                else if (code < 0x800)
                {
                    result += static_cast<char>(0xC0 | (code >> 6));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                }
                else
                {
                    result += static_cast<char>(0xE0 | (code >> 12));
                    result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                }
                pos += 7;
            }
            else
                result += text[pos++];
        }
        return result;
    }

    // `locked`, if given, tells whether the cell cannot be edited: formulas,
    // errors, and times shown as text.
    CellValue convertCell(const OpenXLSX::XLCell &cell, const ExcelWorkbook &book, bool *locked = nullptr)
    {
        OpenXLSX::XLCellValue value = cell.value();
        if (locked)
            *locked = cell.hasFormula() || value.type() == OpenXLSX::XLValueType::Error;
        switch (value.type())
        {
        case OpenXLSX::XLValueType::Empty:
            return {};
        case OpenXLSX::XLValueType::Boolean:
            return value.get<bool>();
        case OpenXLSX::XLValueType::String:
            return decodeEscapes(value.get<std::string>());
        case OpenXLSX::XLValueType::Error:
            try
            {
                return value.getString();
            }
            catch (const std::exception &)
            {
                return std::string("#ERROR");
            }
        case OpenXLSX::XLValueType::Integer:
        case OpenXLSX::XLValueType::Float:
            break;
        }

        const size_t style = cell.cellFormat();
        const DateKind kind = style < book.style_kinds.size() ? book.style_kinds[style] : DateKind::None;
        const double serial = value.get<double>();
        switch (kind)
        {
        case DateKind::Date:
            return Date{static_cast<int32_t>(floorDiv(serialToUnixMillis(serial, book), kMillisPerDay))};
        case DateKind::DateTime:
            return DateTime{serialToUnixMillis(serial, book) * 1000};
        case DateKind::Time:
            if (locked)
                *locked = true;
            return formatTimeOfDay(serial);
        case DateKind::Elapsed:
            if (locked)
                *locked = true;
            return formatElapsed(serial);
        case DateKind::None:
            break;
        }
        if (value.type() == OpenXLSX::XLValueType::Integer)
            return value.get<int64_t>();
        return serial;
    }

    // `locked`, if given, gets the columns that cannot be edited.
    TableRow convertRow(const OpenXLSX::XLRow &row, const ExcelWorkbook &book, std::vector<size_t> *locked = nullptr)
    {
        TableRow out;
        // cells() visits every column from A up to the last stored cell.
        for (const auto &cell : row.cells())
        {
            bool cell_locked = false;
            out.push_back(convertCell(cell, book, locked ? &cell_locked : nullptr));
            if (cell_locked)
                locked->push_back(out.size() - 1);
        }
        while (!out.empty() && std::holds_alternative<std::monostate>(out.back()))
            out.pop_back();
        return out;
    }

    // Lets Excel compute the formulas again when it opens the file. OpenXLSX's
    // setFullCalculationOnLoad() also sets forceFullCalc, which stays in the
    // file (every change computes everything), and does nothing without a
    // calcPr element.
    class SavedWorkbook : public OpenXLSX::XLWorkbook
    {
    public:
        explicit SavedWorkbook(const OpenXLSX::XLWorkbook &book) : OpenXLSX::XLWorkbook(book) {}
        void computeOnLoad()
        {
            pugi::xml_node root = xmlDocument().document_element();
            pugi::xml_node calc = root.child("calcPr");
            if (!calc)
            {
                // Its place in the sequence of CT_Workbook.
                pugi::xml_node after;
                for (const char *name : {"fileVersion", "fileSharing", "workbookPr", "workbookProtection", "bookViews",
                                         "sheets", "functionGroups", "externalReferences", "definedNames"})
                {
                    if (pugi::xml_node node = root.child(name))
                        after = node;
                }
                calc = after ? root.insert_child_after("calcPr", after) : root.prepend_child("calcPr");
            }
            pugi::xml_attribute attribute = calc.attribute("fullCalcOnLoad");
            if (!attribute)
                attribute = calc.append_attribute("fullCalcOnLoad");
            attribute.set_value("1");
        }
    };

    void setCellValue(OpenXLSX::XLCell &cell, const CellValue &value, const ExcelWorkbook &book)
    {
        struct Visitor
        {
            OpenXLSX::XLCell &cell;
            const ExcelWorkbook &book;
            void operator()(std::monostate) const { cell.value().clear(); }
            void operator()(const std::string &v) const { cell.value() = encodeEscapes(v); }
            void operator()(int64_t v) const { cell.value() = v; }
            void operator()(double v) const { cell.value() = v; }
            void operator()(bool v) const { cell.value() = v; }
            // The cell keeps its format, which shows the serial as a date.
            void operator()(Date v) const { cell.value() = unixMicrosToSerial(v.days_since_epoch * kMillisPerDay * 1000, book); }
            void operator()(DateTime v) const { cell.value() = unixMicrosToSerial(v.micros_since_epoch, book); }
        };
        std::visit(Visitor{cell, book}, value);
    }

    class ExcelSheetSource : public SheetSource
    {
    private:
        std::shared_ptr<ExcelWorkbook> book_;
        OpenXLSX::XLWorksheet sheet_;
        std::optional<OpenXLSX::XLRowRange> range_;
        std::optional<OpenXLSX::XLRowIterator> it_;
        uint32_t stored_rows_ = 0; // 1-based row number of the last row stored in the XML
        uint32_t last_row_ = 0;    // last row to show (also covers merged ranges)
        uint32_t next_excel_row_ = 1;
        size_t next_row_ = 0;    // next data row index
        bool header_ = false;
        std::vector<ColumnInfo> columns_;
        std::vector<CellRange> merged_;

        // Returns the next sheet row (an empty row for rows not stored in the
        // file), adding the columns that cannot be edited to `locked`.
        TableRow nextRow(std::vector<size_t> &locked)
        {
            TableRow row;
            if (next_excel_row_ <= stored_rows_)
            {
                if (it_->rowExists())
                    row = convertRow(**it_, *book_, &locked);
                ++*it_;
            }
            ++next_excel_row_;
            return row;
        }

    public:
        ExcelSheetSource(std::shared_ptr<ExcelWorkbook> book, const std::string &name, const OpenOptions &options)
            : book_(std::move(book)),
              sheet_(book_->document.workbook().worksheet(name)) // loads the worksheet XML
        {
            stored_rows_ = last_row_ = sheet_.rowCount();
            if (stored_rows_ > 0)
            {
                range_.emplace(sheet_.rows());
                it_.emplace(range_->begin());
            }

            if (options.header != OpenOptions::Header::No && stored_rows_ > 0 && it_->rowExists())
            {
                // Peek at the first row without consuming it unless it is a header.
                const TableRow first = convertRow(**it_, *book_);
                std::optional<TableRow> second;
                if (stored_rows_ > 1)
                {
                    auto peek = *it_;
                    ++peek;
                    second = peek.rowExists() ? convertRow(*peek, *book_) : TableRow{};
                }
                if (options.header == OpenOptions::Header::Yes ||
                    looksLikeHeaderRow(first, second ? &*second : nullptr))
                {
                    header_ = true;
                    for (const auto &cell : first)
                        columns_.push_back({formatCell(cell)});
                    ++*it_;
                    ++next_excel_row_;
                }
            }

            book_->header_rows[name] = header_;
            auto &merges = sheet_.merges();
            const uint32_t header_rows = header_ ? 1 : 0;
            for (size_t i = 0; i < merges.count(); ++i)
            {
                const std::string ref = merges[static_cast<OpenXLSX::XLMergeIndex>(i)];
                const size_t colon = ref.find(':');
                if (colon == std::string::npos)
                    continue;
                const OpenXLSX::XLCellReference top_left(ref.substr(0, colon));
                const OpenXLSX::XLCellReference bottom_right(ref.substr(colon + 1));
                last_row_ = std::max(last_row_, bottom_right.row());
                if (top_left.row() <= header_rows)
                    continue; // merges inside the header row are not data
                merged_.push_back({.row = top_left.row() - 1 - header_rows,
                                   .col = static_cast<size_t>(top_left.column() - 1),
                                   .rows = static_cast<size_t>(bottom_right.row() - top_left.row() + 1),
                                   .cols = static_cast<size_t>(bottom_right.column() - top_left.column() + 1)});
            }
        }

        SourceCapabilities capabilities() const override
        {
            return {.random_access = false, .query_pushdown = false, .edits = CellEditKind::Typed};
        }
        std::vector<ColumnInfo> columns() override { return columns_; }
        RowCount rowCount() override
        {
            return {last_row_ - (header_ ? 1u : 0u), RowCount::Kind::Exact};
        }
        std::vector<CellRange> mergedCells() override { return merged_; }

        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token) override
        {
            if (first_row != next_row_)
                throw std::logic_error("Excel source must be read sequentially");
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            block->rows.reserve(max_rows);
            std::vector<size_t> locked;
            while (block->rows.size() < max_rows && next_excel_row_ <= last_row_)
            {
                block->rows.push_back(nextRow(locked));
                for (const size_t column : locked)
                    block->locked_cells.emplace_back(block->rows.size() - 1, column);
                locked.clear();
            }
            next_row_ += block->rows.size();
            return block;
        }
    };

    class ExcelDataSource : public DataSource
    {
    private:
        std::shared_ptr<ExcelWorkbook> book_;
        std::filesystem::path path_;
        OpenOptions options_;
        std::vector<std::string> sheet_names_;

    public:
        ExcelDataSource(const std::filesystem::path &path, const OpenOptions &options)
            : book_(std::make_shared<ExcelWorkbook>()), path_(path), options_(options)
        {
            const std::u8string u8 = path.u8string();
            book_->document.open(std::string(u8.begin(), u8.end()));
            sheet_names_ = book_->document.workbook().worksheetNames();

            // OpenXLSX exposes workbook XML through print(), but has no date-system accessor.
            std::ostringstream workbook_xml;
            book_->document.workbook().print(workbook_xml);
            pugi::xml_document metadata;
            if (!metadata.load_string(workbook_xml.str().c_str()))
                throw std::runtime_error("Cannot read Excel workbook properties");
            const auto properties = metadata.select_node("/*/*[local-name()='workbookPr']").node();
            if (properties.attribute("date1904").as_bool())
            {
                book_->date1904 = true;
                book_->unix_epoch_serial = kUnixEpochSerial - 1462.0;
            }

            auto &styles = book_->document.styles();
            std::map<uint32_t, DateKind> custom;
            auto &number_formats = styles.numberFormats();
            for (size_t i = 0; i < number_formats.count(); ++i)
            {
                const auto format = number_formats[i];
                custom[format.numberFormatId()] = customDateKind(format.formatCode());
            }
            auto &cell_formats = styles.cellFormats();
            book_->style_kinds.resize(cell_formats.count(), DateKind::None);
            for (size_t i = 0; i < cell_formats.count(); ++i)
            {
                const uint32_t id = cell_formats[i].numberFormatId();
                const auto it = custom.find(id);
                book_->style_kinds[i] = it != custom.end() ? it->second : builtinDateKind(id);
            }
        }

        std::vector<std::string> sheetNames() override { return sheet_names_; }
        bool usesHeaderOption() const override { return true; }
        bool supportsEditing() const override { return true; }

        // A document of its own: the one being read is not touched, and
        // OpenXLSX writes everything it does not know as it was.
        void saveEdits(const std::vector<SheetEdits> &edits, const std::filesystem::path &target,
                       std::stop_token) override
        {
            try
            {
                OpenXLSX::XLDocument document;
                const std::u8string source = path_.u8string();
                document.open(std::string(source.begin(), source.end()));
                for (const SheetEdits &sheet : edits)
                {
                    const auto header = book_->header_rows.find(sheet.sheet);
                    if (header == book_->header_rows.end())
                        throw std::runtime_error("The sheet has not been read: " + sheet.sheet);
                    auto worksheet = document.workbook().worksheet(sheet.sheet);
                    for (const SheetEdits::Row &row : sheet.rows)
                    {
                        const auto excel_row = static_cast<uint32_t>(row.row + 1 + (header->second ? 1 : 0));
                        const size_t columns = std::max(row.original.size(), row.edited.size());
                        for (size_t column = 0; column < columns; ++column)
                        {
                            static const CellValue missing;
                            const CellValue &before = column < row.original.size() ? row.original[column] : missing;
                            const CellValue &after = column < row.edited.size() ? row.edited[column] : missing;
                            if (before == after)
                                continue;
                            auto cell = worksheet.cell(excel_row, static_cast<uint16_t>(column + 1));
                            bool locked = false;
                            if (!(convertCell(cell, *book_, &locked) == before) || locked)
                                throw std::runtime_error("Cannot save: cell " + OpenXLSX::XLCellReference(excel_row, static_cast<uint16_t>(column + 1)).address() +
                                                         " of sheet " + sheet.sheet + " is not what was read");
                            setCellValue(cell, after, *book_);
                        }
                    }
                }
                // Formulas that use the edited cells keep their old results until computed again.
                if (!edits.empty())
                    SavedWorkbook(document.workbook()).computeOnLoad();
                const std::u8string written = target.u8string();
                document.saveAs(std::string(written.begin(), written.end()), OpenXLSX::XLForceOverwrite);
                document.close();
            }
            catch (const OpenXLSX::XLException &e)
            {
                throw std::runtime_error(std::string("Cannot save the Excel file: ") + e.what());
            }
        }

        std::unique_ptr<SheetSource> openSheet(const std::string &sheet_name) override
        {
            if (std::find(sheet_names_.begin(), sheet_names_.end(), sheet_name) == sheet_names_.end())
                return nullptr;
            return std::make_unique<ExcelSheetSource>(book_, sheet_name, options_);
        }
    };
} // namespace

std::unique_ptr<DataSource> openExcelDataSource(const std::filesystem::path &path, const OpenOptions &options)
{
    try
    {
        return std::make_unique<ExcelDataSource>(path, options);
    }
    catch (const OpenXLSX::XLException &e)
    {
        throw std::runtime_error(std::string("Cannot read Excel file: ") + e.what());
    }
}

void registerExcelBackend(BackendRegistry &registry)
{
    registry.add({
        .name = "Excel",
        .probe = [](const std::filesystem::path &path, std::span<const std::byte> head) -> int
        {
            const std::string ext = lowerExtension(path);
            if (ext != ".xlsx" && ext != ".xlsm")
                return 0;
            // .xlsx is a zip archive ("PK\3\4")
            const bool zip = head.size() >= 4 && head[0] == std::byte{'P'} && head[1] == std::byte{'K'} &&
                             head[2] == std::byte{3} && head[3] == std::byte{4};
            return zip ? 100 : 0;
        },
        .open = [](const std::filesystem::path &path, const OpenOptions &options)
        { return openExcelDataSource(path, options); },
    });
}
