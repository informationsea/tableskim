#ifndef _CELL_FORMAT_HXX_
#define _CELL_FORMAT_HXX_

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "histogram.hxx"
#include "scatter.hxx"
#include "table.hxx"

// Helpers shared by the GUI and TUI table views.

// How a column shows its dates and date-times (with the time of day after
// the date, as it is).
enum class DateStyle
{
    Iso,          // 2024-01-31
    Slash,        // 2024/01/31
    MonthDayYear, // 01/31/2024
    DayMonthYear, // 31/01/2024
    Japanese,     // 2024年1月31日
    MonthName,    // Jan 31, 2024
};
inline constexpr DateStyle DATE_STYLES[] = {
    DateStyle::Iso,          DateStyle::Slash,    DateStyle::MonthDayYear,
    DateStyle::DayMonthYear, DateStyle::Japanese, DateStyle::MonthName,
};
// 2024-01-31 in the style, as the format menus show it.
std::string dateStyleExample(DateStyle style);

inline constexpr int MAX_DECIMALS = 6;

// How a column shows its values. Only the display changes: filters,
// copying and exports keep the values as they are.
struct ColumnFormat
{
    // Numbers (numeric text too): thousands separated by commas, and a
    // fixed number of decimals (0 to MAX_DECIMALS, rounded; nullopt: as
    // stored).
    bool thousands = false;
    std::optional<int> decimals;
    // Dates and date-times (ISO date text too).
    DateStyle date = DateStyle::Iso;

    bool isDefault() const { return *this == ColumnFormat{}; }
    bool operator==(const ColumnFormat &) const = default;
};

// The text of a value in a format; other values (text, booleans) as formatCell().
std::string formatCellAs(const CellValue &value, const ColumnFormat &format);

// The formats of a sheet's columns in a view, by model column, like
// HiddenColumns.
class ColumnFormats
{
private:
    std::map<size_t, ColumnFormat> formats_;

public:
    // nullptr if the column has the default format.
    const ColumnFormat *find(size_t column) const;
    ColumnFormat get(size_t column) const;
    // The default format takes the column off the list.
    void set(size_t column, const ColumnFormat &format);
    bool empty() const { return formats_.empty(); }
    void clear() { formats_.clear(); }
    bool operator==(const ColumnFormats &) const = default;
};

// One-line text for drawing a cell: at most max_bytes bytes (cut at a UTF-8
// character boundary and followed by "…"), with line breaks turned into
// spaces, in the column's format if given.
std::string cellDisplayText(const CellValue &value, size_t max_bytes = 256, const ColumnFormat *format = nullptr);

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
// replace tabs/CR/LF inside each cell with spaces, quote cells that start with a
// double quote (doubling the quotes in them) and separate cells/rows as TSV.
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

// Scatter plot texts. A coordinate on an axis (see histogramCoordinate()):
// the number, or the time as a date or date-time.
std::string scatterValueText(const Histogram &axis, double coordinate);
// Conditions that keep the rows of a cell of the density grid, by
// histogramBinConditions() on each column.
std::vector<FilterCondition> densityCellConditions(const ScatterPlot &plot, size_t x_column, size_t y_column,
                                                   size_t x_bin, size_t y_bin);
// Conditions that keep the rows whose values lie in [x_low, x_high] ×
// [y_low, y_high] (coordinates; rounded a little finer than the density
// bins, as for a rectangle drawn with the mouse).
std::vector<FilterCondition> scatterRangeConditions(const ScatterPlot &plot, size_t x_column, size_t y_column,
                                                    double x_low, double x_high, double y_low, double y_high);
// "x [10, 20) × y [5, 6): 42 rows (3.1%)", or "Osaka × y [5, 6): ..." for
// a categorical X.
std::string densityCellText(const ScatterPlot &plot, size_t x_bin, size_t y_bin);
// A category of a categorical X: its value as text, or "(other)".
std::string scatterCategoryText(const ScatterPlot &plot, size_t category);
// Conditions that keep the rows of a category of a categorical X: equal to
// its value, and Y of the kind plotted. None for "(other)".
std::vector<FilterCondition> scatterCategoryConditions(const ScatterPlot &plot, size_t x_column, size_t y_column,
                                                       size_t category);
// "Osaka: 9 rows; min 20, Q1 25, median 30, Q3 40, max 57; mean 32.3; 1 outlier"
std::string boxPlotText(const ScatterPlot &plot, size_t category);
// "r = 0.8123, R² = 0.6598, y = 2641.23 x + 744351": Pearson's correlation,
// its square and the least-squares line (only when both axes are numbers;
// times would be in microseconds). Empty without two points.
std::string scatterStatsText(const ScatterPlot &plot);

// Whether the loaded cells of a column in view rows [first_row, last_row)
// are mostly numbers or times, as a scatter plot reads them. Cells not
// loaded yet are skipped (and requested); false if none is loaded.
bool columnLooksPlottable(TableModel &model, size_t column, size_t first_row, size_t last_row);
struct ScatterColumns
{
    size_t x = 0;
    size_t y = 0;
};
// The columns to plot when none are chosen: Y is `column`, or the visible
// column nearest to it that looks plottable; X the nearest such column to
// the left of Y, else to its right. Without plottable columns, the visible
// column left of `column` (right of it for the first) against `column`.
ScatterColumns defaultScatterColumns(TableModel &model, size_t column, const HiddenColumns &hidden, size_t first_row,
                                     size_t last_row);

// A count of a density cell on the colour scale, from 0 to 1: linear, or
// logarithmic (log(1 + count)), relative to the largest count.
double densityScale(size_t count, size_t most, bool log_scale);
// The colour of a position on the scale (Viridis: perceptually even, and
// readable on light and dark backgrounds), as RGB.
std::array<unsigned char, 3> densityColor(double t);
// "1200 rows plotted, 3 skipped (empty or not numbers or dates)", and with
// points, "a sample of 200000 points" when there are more rows.
std::string scatterSummary(const ScatterPlot &plot, bool points);

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
