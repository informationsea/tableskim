#ifndef _COLUMN_STATS_HXX_
#define _COLUMN_STATS_HXX_

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "table_types.hxx"

struct ValueCount
{
    CellValue value;
    size_t count = 0;
    bool operator==(const ValueCount &) const = default;
};

// Statistics of one column, with the semantics of the in-memory sort and
// filter: empty means NULL or "", and numeric text counts as a number.
struct ColumnStats
{
    size_t rows = 0;  // values seen, empty ones included
    size_t empty = 0; // NULL, "" or no cell at all (ragged rows)
    // Distinct non-empty values. Once there are too many to count, this is a
    // lower bound, and `top` may miss values that are seen often later on.
    size_t distinct = 0;
    bool distinct_exact = true;
    // The first and last non-empty values in ascending sort order.
    std::optional<CellValue> min;
    std::optional<CellValue> max;
    // Non-empty values that are numbers: integers, doubles and numeric text.
    // The others are over these values alone.
    size_t numbers = 0;
    std::optional<double> sum;
    std::optional<double> mean;
    std::optional<double> stddev; // sample standard deviation; needs two numbers
    // The most frequent non-empty values: by count, then in sort order.
    std::vector<ValueCount> top;
};

struct ColumnStatsLimits
{
    size_t max_distinct = 200000; // distinct values counted
    size_t top_values = 100;      // values in ColumnStats::top
};

// Builds ColumnStats one value at a time, in a single pass. Not thread-safe.
class ColumnStatsAccumulator
{
public:
    explicit ColumnStatsAccumulator(ColumnStatsLimits limits = {}) : limits_(limits) {}

    // Adds `count` cells holding the value (e.g. a GROUP BY row).
    void add(const CellValue &value, size_t count = 1);
    // The statistics so far; may be called again after more values.
    ColumnStats result() const;

private:
    struct Entry
    {
        CellValue value;
        size_t count = 0;
    };

    void addNumber(double number, size_t count);
    void countText(const std::string &text, size_t count);
    void countOther(const CellValue &value, size_t count);

    ColumnStatsLimits limits_;
    size_t rows_ = 0;
    size_t empty_ = 0;
    size_t numbers_ = 0;
    size_t finite_ = 0; // of numbers_; the running sums are over these
    size_t nan_ = 0;
    size_t positive_infinity_ = 0;
    size_t negative_infinity_ = 0;
    // The running mean and population standard deviation (without squaring
    // large values), and Neumaier's compensated sum.
    double mean_ = 0;
    double population_stddev_ = 0;
    double sum_ = 0;
    double sum_compensation_ = 0;
    // Text cells that are not numbers sort after everything else, so they
    // are tracked apart and compared without copies.
    std::optional<CellValue> min_value_;
    std::optional<CellValue> max_value_;
    std::optional<std::string> min_text_;
    std::optional<std::string> max_text_;
    // Text cells are looked up by their text, without a copy. Other values
    // (numbers, dates, ... from typed sources) by type and display text.
    std::unordered_map<std::string, size_t> text_counts_;
    std::unordered_map<std::string, Entry> other_counts_;
    bool distinct_exact_ = true;
};

#endif // _COLUMN_STATS_HXX_
