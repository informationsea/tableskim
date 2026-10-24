#ifndef _GROUP_BY_HXX_
#define _GROUP_BY_HXX_

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "column_stats.hxx"
#include "table_types.hxx"

class TableModel;

// What is computed for each value column of a group, with the semantics of
// the column statistics (ColumnStats): empty cells are left out, and numeric
// text counts as a number.
enum class Aggregate
{
    Count,    // non-empty values
    Distinct, // distinct non-empty values
    Sum,
    Mean,
    Min, // in sort order, like the statistics
    Max,
    StdDev, // sample standard deviation
};
inline constexpr Aggregate AGGREGATES[] = {
    Aggregate::Count, Aggregate::Distinct, Aggregate::Sum, Aggregate::Mean,
    Aggregate::Min,   Aggregate::Max,      Aggregate::StdDev,
};
// "count", "distinct", "sum", "mean", "min", "max", "std dev"
const char *aggregateName(Aggregate aggregate);

struct GroupBySpec
{
    // Rows with equal values in these columns form a group; none: one group
    // of every row.
    std::vector<size_t> keys;
    // Each aggregate of each of these columns becomes a column.
    std::vector<size_t> values;
    std::vector<Aggregate> aggregates;
    bool operator==(const GroupBySpec &) const = default;
};

struct GroupByLimits
{
    size_t max_groups = 1000000;
};

// Groups rows one at a time. Not thread-safe.
class GroupByAccumulator
{
public:
    explicit GroupByAccumulator(GroupBySpec spec, GroupByLimits limits = {});

    // Throws std::runtime_error once there are more than max_groups groups.
    void add(const TableRow &row);
    size_t groups() const { return groups_.size(); }
    // One row per group, in the sort order of its key values: the keys, the
    // group's row count ("count"), then "<column> <aggregate>" for each
    // value column and aggregate. `names` are the sheet's column names.
    ResultSheet result(const std::function<std::string(size_t column)> &names) const;

private:
    // Keys in sort order (compareCells()); values that sort alike but differ
    // ("1" and "01") are told apart by their type and text.
    struct KeyLess
    {
        bool operator()(const std::vector<CellValue> &a, const std::vector<CellValue> &b) const;
    };
    struct Group
    {
        size_t rows = 0;
        std::vector<ColumnStatsAccumulator> values; // one per value column
    };

    GroupBySpec spec_;
    GroupByLimits limits_;
    ColumnStatsLimits stats_limits_;
    std::map<std::vector<CellValue>, Group, KeyLess> groups_;
    std::vector<CellValue> key_; // reused for lookups
};

// Groups the rows of the model's view (sorted and filtered as shown), read
// through TableModel::readView() like an export. Returns nullopt if stopped.
// `progress` is called with the rows read so far, on the calling thread.
// Throws std::runtime_error, e.g. if the view changes meanwhile.
std::optional<ResultSheet> groupByView(const std::shared_ptr<TableModel> &model, const GroupBySpec &spec,
                                       std::stop_token stop = {}, const std::function<void(size_t rows)> &progress = {},
                                       GroupByLimits limits = {});

#endif // _GROUP_BY_HXX_
