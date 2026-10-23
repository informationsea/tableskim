#ifndef _CELL_FORMAT_HXX_
#define _CELL_FORMAT_HXX_

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "histogram.hxx"
#include "table.hxx"

// Helpers shared by the GUI and TUI table views.

// One-line text for drawing a cell: at most max_bytes bytes (cut at a UTF-8
// character boundary and followed by "…"), with line breaks turned into spaces.
std::string cellDisplayText(const CellValue &value, size_t max_bytes = 256);

// Numbers, including text that parses as one, are right-aligned.
bool isNumericCell(const CellValue &value);

// Columns hidden in a sheet's view, by model column. They keep their
// place, so sorts, filters, search hits and statistics are unaffected: the
// UIs skip them when drawing, moving the cursor and copying.
class HiddenColumns
{
private:
    std::set<size_t> columns_;

public:
    bool contains(size_t column) const { return columns_.contains(column); }
    bool empty() const { return columns_.empty(); }
    size_t size() const { return columns_.size(); }
    // Hides the columns [first, last] of column_count, except that one stays
    // visible if all of them would be hidden. Returns how many were hidden.
    size_t hide(size_t first, size_t last, size_t column_count);
    void show(size_t column) { columns_.erase(column); }
    void clear() { columns_.clear(); }
    // Whether hide(first, last, column_count) would hide all of them.
    bool hidesAll(size_t first, size_t last, size_t column_count) const;
    // How many of the columns [first, last] are not hidden (copied, say).
    size_t visibleIn(size_t first, size_t last) const;
    // `column` if it is visible, else the nearest visible one in `direction`
    // (+1 or -1), else the nearest one the other way.
    size_t nearestVisible(size_t column, int direction, size_t column_count) const;
    bool operator==(const HiddenColumns &) const = default;
};

struct TsvCopyResult
{
    std::string text;
    size_t copied_rows = 0;
    size_t skipped_rows = 0;
};

// A single cell is copied verbatim, with no added newline. For multiple cells,
// replace tabs/CR/LF inside each cell with spaces and separate cells/rows as TSV.
// Skip whole rows with unavailable cells; loaded NULL and ragged cells stay empty.
// Hidden columns are left out.
TsvCopyResult rangeToTsv(TableModel &model, size_t row_top, size_t col_left, size_t row_bottom, size_t col_right,
                         const HiddenColumns *hidden = nullptr);

// Text from the clipboard as rows of cells: tab-separated, one row per line
// (a last line break ends the last row), with fields in double quotes
// holding tabs, line breaks and doubled quotes, as spreadsheets copy them.
std::vector<std::vector<std::string>> parseTsv(std::string_view text);

// The cells a paste into the selection [row_top, row_bottom] x [col_left,
// col_right] changes, at most row_count rows and column_count columns:
// `values` from the top-left cell on (one value fills the whole selection),
// leaving out hidden columns, which take no value.
std::vector<TableModel::CellText> pastedCells(const std::vector<std::vector<std::string>> &values, size_t row_top,
                                              size_t col_left, size_t row_bottom, size_t col_right, size_t row_count,
                                              size_t column_count, const HiddenColumns *hidden = nullptr);

struct ColumnWidthMetrics
{
    std::function<int(const std::string &)> measure; // display width of a text
    int header_padding = 0;
    int cell_padding = 0;
    int min_width = 0;
    int max_width = 0;
    size_t sample_rows = 100;
};

// Width that fits the column header and the loaded cells among the first
// sample_rows rows, clamped to [min_width, max_width].
int fittingColumnWidth(TableModel &model, size_t column, const ColumnWidthMetrics &metrics);

// The sort after a click on a column header (GUI) or `o` (TUI): by that
// column ascending, then descending, then file order. Sorting by one column
// replaces any other sort.
std::vector<SortKey> nextSortCycle(const std::vector<SortKey> &sort, size_t column);

// "100 rows", "≥512 rows" while counting, "? rows" when nothing is known.
std::string rowCountText(const RowCount &count);
// rowCountText(rows), plus " of N" when a filter hides some (unfiltered is
// TableModel::unfilteredRowCount()).
std::string filteredRowCountText(const RowCount &rows, const RowCount &unfiltered, bool filtered);

// Filter operators in the order the filter editors list them.
inline constexpr FilterOp FILTER_OPS[] = {
    FilterOp::Contains,  FilterOp::NotContains, FilterOp::Equals,       FilterOp::NotEquals,
    FilterOp::Less,      FilterOp::LessEqual,   FilterOp::Greater,      FilterOp::GreaterEqual,
    FilterOp::IsEmpty,   FilterOp::IsNotEmpty,  FilterOp::Matches,      FilterOp::NotMatches,
    FilterOp::IsNumber,  FilterOp::IsDate,
};

// Short name of an operator, e.g. "≥" or "contains"; "matches" takes a
// regular expression.
const char *filterOpLabel(FilterOp op);
// IsEmpty, IsNotEmpty, IsNumber and IsDate ignore FilterCondition::value.
bool filterOpTakesValue(FilterOp op);

// The quick filter on a cell: equal (or not) to its text, or, for an empty
// cell, "is empty" (or not).
FilterCondition cellFilterCondition(size_t column, const CellValue &value, bool equal);
// Adds a condition to the view's filter unless it is already there. A new
// filter matches all of its conditions.
void addFilterCondition(ViewSpec &spec, FilterCondition condition);
// Whether a condition of the view's filter is on the column.
bool filtersColumn(const ViewSpec &spec, size_t column);
// One line such as `Price ≥ "1000" and Name contains "Tokyo"`.
std::string filterSummary(const TableModel &model, const Filter &filter);

// "12.5%": count out of total, with one decimal; "0%" when total is 0.
std::string percentText(size_t count, size_t total);

// Histogram texts. An edge as numbers with the decimals the bin widths need,
// or as a time down to the unit of the bins ("2024", "2024-03",
// "2024-03-05", "2024-03-05 13:00", "2024-03-05 13:00:05").
std::string histogramEdgeText(const Histogram &histogram, double edge);
// A bin, short: "10 – 20", a single integer "7", or the start of a bin of
// times ("2024-03").
std::string histogramBinLabel(const Histogram &histogram, size_t bin);
// A bin with its bounds: "[10, 20)", "[90, 100]" for the last one.
std::string histogramBinRange(const Histogram &histogram, size_t bin);
// Conditions that keep the rows of a bin: "is a number" (or "is a date"),
// ≥ its lower edge and < its upper one (≤ for the last bin of numbers).
std::vector<FilterCondition> histogramBinConditions(const Histogram &histogram, size_t column, size_t bin);
// "53 numbers in 10 bins, 2 other values, 1 empty"
std::string histogramSummary(const Histogram &histogram);

// "100 MB" below 1 GB, else "2.0 GB" (binary units, as memory is counted).
std::string memorySizeText(size_t bytes);

struct StatsLine
{
    std::string label;
    std::string value;
    bool operator==(const StatsLine &) const = default;
};
// Column statistics as lines such as {"Empty", "3 (1.5%)"} for the
// sidebars; lines that do not apply (e.g. Mean of text) are left out.
std::vector<StatsLine> columnStatsLines(const ColumnStats &stats);

// Where Go To (GUI: Edit > Go To..., TUI: Ctrl+G) moves the cursor; a part
// left unset keeps the cursor's row or column. 0-based.
struct GoToTarget
{
    std::optional<size_t> row;
    std::optional<size_t> column;
    bool operator==(const GoToTarget &) const = default;
};
struct GoToResult
{
    std::optional<GoToTarget> target;
    std::string error; // why there is no target
};
// Parses a row number ("120"), a cell in A1 ("B12") or R1C1 ("R12C2")
// notation, a column letter ("AB") or a column's header name (`names`,
// empty without a header row, matched exactly, then ignoring ASCII case);
// rows and columns count from 1. A header name wins over a column letter. Rows past an exact row count, and columns past
// column_count, are errors.
GoToResult parseGoTo(std::string_view text, const std::vector<std::string> &names, size_t column_count,
                     const RowCount &rows);
// The same for a sheet: its header names, columns and row count in the view.
GoToResult parseGoTo(std::string_view text, const TableModel &model);

#endif // _CELL_FORMAT_HXX_
