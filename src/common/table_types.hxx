#ifndef _TABLE_TYPES_HXX_
#define _TABLE_TYPES_HXX_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common.hxx"

// ---------------------------------------------------------------------------
// Cell values
//
// Cells are plain values (no virtual functions, no per-cell heap object) so
// that a million-row block does not turn into millions of allocations.
// ---------------------------------------------------------------------------

struct Date
{
    int32_t days_since_epoch; // 1970-01-01 == 0
    bool operator==(const Date &) const = default;
};

struct DateTime
{
    int64_t micros_since_epoch; // 1970-01-01T00:00:00 == 0
    bool operator==(const DateTime &) const = default;
};

// std::monostate means NULL / missing value.
using CellValue = std::variant<std::monostate, std::string, int64_t, double, bool, Date, DateTime>;

std::string formatCell(const CellValue &value);

// Parses the whole string as a number; used for numeric-aware sort/filter of text cells.
std::optional<double> parseNumber(const std::string &text);

// Floating-point std::from_chars / std::to_chars need macOS 26.0 / 13.3, so
// these stand in for them with the same results (NaN aside: "nan" / "-nan").
// The whole text as a double, without spaces, '+' or hexadecimal.
std::optional<double> parseDouble(std::string_view text);
// The shortest text that reads back as the value ("5e+05", "0.1").
std::string shortestDoubleText(double value);

// "A", "B", ..., "Z", "AA", ...
std::string columnLetter(size_t column);

// A date or date-time typed into a filter, or held as text in a cell.
struct TemporalValue
{
    int64_t micros = 0;    // since 1970-01-01T00:00:00
    bool has_time = false; // a date alone stands for the whole day
};
// Parses an ISO date (2024-01-31) or date-time (2024-01-31 12:34,
// 12:34:56 or 12:34:56.789, with ' ' or 'T' before the time); surrounding
// spaces are ignored.
std::optional<TemporalValue> parseTemporal(std::string_view text);

// TRUE / FALSE, yes / no, 1 / 0 in any case, for filters on boolean cells.
std::optional<bool> parseBoolean(std::string_view text);

// NULL or empty text: what filters call empty.
bool isEmptyCell(const CellValue &value);

// The sort order: empty values last, then numbers (numeric text, booleans
// and dates included), then text bytewise. Negative, zero or positive.
int compareCells(const CellValue &a, const CellValue &b);

// How a source stores the cells that are edited (SourceCapabilities::edits).
enum class CellEditKind
{
    None, // cells cannot be edited
    Text, // every cell is text (CSV): what is typed is kept as is
    Typed, // cells have types (Excel): see editedCellValue()
};

// The value of a cell after `text` is typed into it (`original` is its value
// before). Text sources keep the text. Typed sources keep the cell's type
// where the text reads as one: numbers (and empty cells) take a number,
// booleans TRUE / FALSE, dates and date-times an ISO date or date-time;
// anything else, and text cells, become text. Empty text clears the cell.
CellValue editedCellValue(const std::string &text, const CellValue &original, CellEditKind kind);

// ---------------------------------------------------------------------------
// Row blocks: immutable once built, shared between the worker and the UI.
// ---------------------------------------------------------------------------

using TableRow = std::vector<CellValue>; // rows may have different lengths (ragged)

struct RowBlock
{
    size_t first_row = 0; // row index in the source
    std::vector<TableRow> rows;
    // Editable sources: cells that cannot be edited (e.g. Excel formulas),
    // as (index into rows, column), sorted.
    std::vector<std::pair<size_t, size_t>> locked_cells;

    size_t approxBytes() const;
};

using RowBlockPtr = std::shared_ptr<const RowBlock>;

// Heuristic for OpenOptions::Header::Auto: every cell is a non-empty,
// non-numeric, unique text value. A single cell above a wider next_row is
// taken as a title, not a header.
bool looksLikeHeaderRow(const TableRow &row, const TableRow *next_row = nullptr);

struct RowCount
{
    enum class Kind
    {
        Unknown, // nothing known yet
        AtLeast, // value rows exist, maybe more
        Exact,
    };
    size_t value = 0;
    Kind kind = Kind::Unknown;

    bool isExact() const { return kind == Kind::Exact; }
    bool operator==(const RowCount &) const = default;
};

struct ColumnInfo
{
    std::string name;
};

// The rows of a sheet computed in memory, e.g. by a Group By (see
// TableBook::addResultSheet()).
struct ResultSheet
{
    std::vector<ColumnInfo> columns;
    std::vector<TableRow> rows;
};

// Merged cells are a property of the sheet, not of individual cells.
struct CellRange
{
    size_t row = 0;
    size_t col = 0;
    size_t rows = 1;
    size_t cols = 1;
};

// ---------------------------------------------------------------------------
// Sort / filter / search
// ---------------------------------------------------------------------------

struct SortKey
{
    size_t column = 0;
    bool ascending = true;
    bool operator==(const SortKey &) const = default;
};

enum class FilterOp
{
    Equals,
    NotEquals,
    Contains,
    NotContains,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    IsEmpty,
    IsNotEmpty,
    Matches,    // a regular expression found in the cell's text (RE2, ignoring case)
    NotMatches,
    // The kinds a histogram counts (ignoring the value): finite numbers and
    // numeric text, not booleans or dates; dates, date-times and ISO date
    // text that is not a number.
    IsNumber,
    IsDate,
};

struct FilterCondition
{
    size_t column = 0;
    FilterOp op = FilterOp::Contains;
    std::string value;
    bool operator==(const FilterCondition &) const = default;
};

// The in-memory filter semantics for one cell (condition.column is ignored),
// for backends that evaluate filters themselves (e.g. as an SQL function).
// Comparisons follow the cell's type: numbers (numeric text too) compare
// numerically; dates and date-times (ISO date text too) against a date or
// date-time value compare in time, a date value meaning its whole day (so
// "= 2024-01-31" matches any time that day); booleans against TRUE / FALSE
// (yes / no, 1 / 0); anything else compares its text bytewise.
bool cellMatchesCondition(const CellValue &cell, const FilterCondition &condition);

struct Filter
{
    enum class Combine
    {
        And,
        Or,
    };
    Combine combine = Combine::And;
    std::vector<FilterCondition> conditions;
    // Chart selections further restrict the editable filter, regardless of
    // its combine mode. Every narrowing condition is joined with AND.
    std::vector<FilterCondition> narrowing;
    bool empty() const { return conditions.empty() && narrowing.empty(); }
    bool operator==(const Filter &) const = default;
};

// Why the filter cannot be applied (an invalid regular expression), if so.
std::optional<std::string> filterError(const Filter &filter);

struct ViewSpec
{
    std::vector<SortKey> sort;
    std::optional<Filter> filter;

    bool filtered() const { return filter && !filter->empty(); }
    bool empty() const { return sort.empty() && !filtered(); }
    bool operator==(const ViewSpec &) const = default;
};

struct SearchSpec
{
    std::string text;
    bool case_sensitive = false;
    // text is an RE2 regular expression, matched anywhere in the cell text.
    bool regex = false;
    std::optional<size_t> column; // nullopt: all columns
};

struct CellPosition
{
    size_t row = 0; // view row
    size_t col = 0;
    bool operator==(const CellPosition &) const = default;
};

#endif // _TABLE_TYPES_HXX_
