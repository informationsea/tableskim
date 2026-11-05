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

// The groups a column's values are told apart by colour in: its most
// frequent non-empty cell texts, then "(other)" for its other values and
// "(empty)" for empty cells, when there are any.
struct ValueGroups
{
    std::vector<CellValue> values;
    bool other = false;
    bool empty = false;

    // At most `max` of the most frequent values of the column's statistics.
    static ValueGroups fromStats(const ColumnStats &stats, size_t max);
    size_t count() const { return values.size() + (other ? 1 : 0) + (empty ? 1 : 0); }
    bool isOther(size_t group) const { return other && group == values.size(); }
    bool isEmpty(size_t group) const { return empty && group + 1 == count(); }
    // The group of a value of the column; nullopt for one it has no group for.
    std::optional<size_t> groupOf(const CellValue &value) const;
    bool operator==(const ValueGroups &) const = default;
};

// Groups told apart by colour, at most (the categorical palette's slots).
constexpr size_t MAX_VALUE_GROUPS = 8;

// Group identity is the full cell text, as used by a group's exact filter.
// Typed values with the same text therefore share a group too.
class ValueGroupAccumulator
{
public:
    explicit ValueGroupAccumulator(ColumnStatsLimits limits = {}) : stats_(limits) {}
    void add(const CellValue &value, size_t count = 1);
    ColumnStats result() const { return stats_.result(); }

private:
    ColumnStatsAccumulator stats_;
};

struct Histogram
{
    HistogramKind kind = HistogramKind::None;
    // Bin i holds [edges[i], edges[i + 1]); the last one also its upper edge,
    // unless it holds one integer (see numberHistogramEdges()).
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
    // Stacked by a second column (bins only), by the groups of its values.
    // group_counts has the rows of each bin and group, at groupIndex(); a
    // bin's add up to its count.
    bool grouped = false;
    ValueGroups groups;
    std::vector<size_t> group_counts;

    size_t binCount() const { return counts.size(); }
    size_t groupCount() const { return grouped ? groups.count() : 0; }
    size_t groupIndex(size_t bin, size_t group) const { return bin * groupCount() + group; }
    size_t groupCount(size_t bin, size_t group) const { return group_counts[groupIndex(bin, group)]; }
    // Which group an index stands for.
    bool isOtherGroup(size_t group) const { return groups.isOther(group); }
    bool isEmptyGroup(size_t group) const { return groups.isEmpty(group); }
};

// The first pass: finds the kind of the column, its range, and the most
// frequent values. Not thread-safe.
class HistogramRange
{
public:
    // `grouped`: stacked by a second column, whose values addGroup() takes.
    explicit HistogramRange(ColumnStatsLimits limits = {}, bool grouped = false)
        : stats_(limits), groups_(limits), grouped_(grouped)
    {
    }
    void add(const CellValue &value, size_t count = 1);
    // A value of the column the bins are stacked by. Its values may come
    // apart from those of the binned column (the distinct values of each).
    void addGroup(const CellValue &group, size_t count = 1);
    // Bins (about `target` of them) with zero counts, the totals, and the
    // most frequent values; `values` and `others` are those of the kind.
    // Grouped bins get up to `max_groups` groups.
    Histogram layout(size_t target, size_t max_groups = MAX_VALUE_GROUPS) const;

private:
    ColumnStatsAccumulator stats_;
    ValueGroupAccumulator groups_;
    bool grouped_ = false;
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
    // A row of a grouped layout: its value and that of the column the bins
    // are stacked by.
    void addGrouped(const CellValue &value, const CellValue &group, size_t count = 1);
    const Histogram &result() const { return histogram_; }
    // The group that addGrouped() counts `group` in; nullopt for a value
    // the layout has no group for.
    std::optional<size_t> groupOf(const CellValue &group) const;
    // The bin that add() counts `value` in; nullopt for empty values and
    // values of another kind.
    std::optional<size_t> binOf(const CellValue &value) const;
    // False for a value of the layout's kind outside its bins, which a
    // layout taken with it would have covered (rows added later, say).
    bool fits(const CellValue &value) const;
    // Grouped: also false for a row of a bin whose group the layout lacks.
    bool fits(const CellValue &value, const CellValue &group) const;

private:
    Histogram histogram_;
    std::vector<FilterCondition> number_lower_bounds_;
    FilterCondition number_upper_bound_;
};

// Bins of "nice" widths (1, 2 or 5 times a power of ten) covering [min, max],
// about `target` of them; integers get widths of at least 1, and a bin per
// value when they span at most `target` + 1 values. A bin of integers of
// width 1 holds only its lower edge, the last one too.
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
