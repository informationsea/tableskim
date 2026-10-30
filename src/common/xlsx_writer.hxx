#ifndef _XLSX_WRITER_HXX_
#define _XLSX_WRITER_HXX_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "table_types.hxx"

// Writes a workbook with one sheet, a row at a time. Cells go straight into
// the compressed sheet (strings inline, not in a shared table), so memory
// use does not grow with the rows. Throws std::runtime_error.
class XlsxWriter
{
public:
    static constexpr size_t MAX_ROWS = 1048576;
    static constexpr size_t MAX_COLUMNS = 16384;
    static constexpr size_t MAX_TEXT = 32767; // characters in a cell
    // Larger integers have more digits than Excel keeps.
    static constexpr int64_t MAX_EXACT_INTEGER = 999999999999999;

    // `header`: the first row is drawn bold and stays in place when scrolling.
    XlsxWriter(const std::filesystem::path &path, const std::string &sheet_name, bool header);
    ~XlsxWriter();
    XlsxWriter(const XlsxWriter &) = delete;
    XlsxWriter &operator=(const XlsxWriter &) = delete;

    // NULL cells are left empty. Dates and times are stored as Excel serial
    // numbers with a date format. Stored as text instead: dates outside
    // Excel's calendar (1900-01-01 to 9999-12-31), non-finite numbers, and
    // integers beyond MAX_EXACT_INTEGER. Longer text is cut at MAX_TEXT.
    void addRow(std::span<const CellValue> cells);
    size_t rows() const { return rows_; }
    // Completes the file; without it, the file is not a valid workbook.
    void finish();

private:
    class Zip;
    std::unique_ptr<Zip> zip_;
    std::string buffer_;
    std::vector<std::string> letters_; // column letters, for cell references
    size_t rows_ = 0;
    bool header_ = false;
    bool finished_ = false;

    void flush();
};

// Excel writes characters that XML cannot hold as _xHHHH_, and the "_" of
// text that looks like such an escape as _x005F_. Whether one starts at
// text[pos].
bool isXlsxEscapePattern(std::string_view text, size_t pos);
// Whether a byte is written as _xHHHH_: control characters but tab and line
// feed (a raw CR would be read back as LF, by XML's line-end handling).
inline bool needsXlsxEscape(unsigned char c)
{
    return c < 0x20 && c != '\t' && c != '\n';
}
// Appends the byte as _xHHHH_.
void appendXlsxEscape(std::string &out, unsigned char c);

// A valid Excel sheet name: no []:*?/\, at most 31 characters, not empty.
std::string xlsxSheetName(const std::string &name);

#endif // _XLSX_WRITER_HXX_
