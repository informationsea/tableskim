#ifndef _HISTOGRAM_HXX_
#define _HISTOGRAM_HXX_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "column_stats.hxx"
#include "table_types.hxx"

// What a histogram counts: numbers (integers, doubles and numeric text, as
// in ColumnStats) or points in time (dates, date-times and ISO date text, as
// in filters). A column holding neither has no bins, only its most frequent
// values.
enum class HistogramKind
{
    None,
    Number,
    Time,
};

// The width of a bin of times; bins of months and years follow the calendar.
enum class TimeUnit
{
    Second,
    Minute,
    Hour,
    Day,
    Month,
    Year,
};

struct Histogram
{
    HistogramKind kind = HistogramKind::None;
    // Bin i holds [edges[i], edges[i + 1]); the last one also its upper edge.
    // Times are microseconds since 1970-01-01 (UTC).
    std::vector<double> edges;
    std::vector<size_t> counts; // one per bin
    // Numbers: digits after the decimal point that tell the edges apart.
    int decimals = 0;
    bool integers = false; // Number: every value is an integer
    bool dates = false;    // Time: every value is a date, without a time of day
    TimeUnit unit = TimeUnit::Day;
    size_t values = 0; // counted in the bins
    size_t others = 0; // non-empty values of another kind (and NaN, infinity)
    size_t empty = 0;
    size_t rows = 0;
    // The most frequent non-empty values, for columns of kind None.
    std::vector<ValueCount> top;

    size_t binCount() const { return counts.size(); }
};

// The first pass: finds the kind of the column, its range, and the most
// frequent values. Not thread-safe.
class HistogramRange
{
public:
    explicit HistogramRange(ColumnStatsLimits limits = {}) : stats_(limits) {}
    void add(const CellValue &value, size_t count = 1);
    // Bins (about `target` of them) with zero counts, the totals, and the
    // most frequent values; `values` and `others` are those of the kind.
    Histogram layout(size_t target) const;

private:
    ColumnStatsAccumulator stats_;
    size_t numbers_ = 0;
    size_t times_ = 0;
    double min_number_ = 0;
    double max_number_ = 0;
    bool integers_ = true;
    int64_t min_time_ = 0;
    int64_t max_time_ = 0;
    bool dates_ = true;
};

// The second pass: counts values into the bins of a layout. Not thread-safe.
class HistogramCounter
{
public:
    explicit HistogramCounter(Histogram layout);
    void add(const CellValue &value, size_t count = 1);
    const Histogram &result() const { return histogram_; }
    // The bin that add() counts `value` in; nullopt for empty values and
    // values of another kind.
    std::optional<size_t> binOf(const CellValue &value) const;

private:
    Histogram histogram_;
    std::vector<FilterCondition> number_lower_bounds_;
    FilterCondition number_upper_bound_;
};

// Bins of "nice" widths (1, 2 or 5 times a power of ten) covering [min, max],
// about `target` of them; integers get widths of at least 1, and a bin per
// value when they span at most `target` values.
std::vector<double> numberHistogramEdges(double min, double max, bool integers, size_t target, int *decimals = nullptr);
// Bins of whole seconds, minutes, hours, days, weeks (from Monday), months
// or years covering [min, max] (microseconds), about `target` of them.
std::vector<double> timeHistogramEdges(int64_t min, int64_t max, bool dates, size_t target, TimeUnit *unit = nullptr);

// Where a value lies on an axis of `kind`: the number, or the time in
// microseconds since 1970-01-01 (UTC); nullopt for other values.
std::optional<double> histogramCoordinate(HistogramKind kind, const CellValue &value);

// The bin that holds `x`, if any.
std::optional<size_t> histogramBin(const std::vector<double> &edges, double x);

#endif // _HISTOGRAM_HXX_
