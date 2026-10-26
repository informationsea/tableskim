#include "group_by.hxx"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#include "table.hxx"

namespace
{
    // To the 15 significant digits a double holds, which drops the noise of
    // binary arithmetic (918499.4500000001 is 918499.45). Whole numbers carry
    // no such noise and are exact up to 2^53, which has 16 digits
    // (1234567890123456 stays).
    double roundSignificant(double value)
    {
        if (!std::isfinite(value) || (value == std::trunc(value) && std::fabs(value) <= 0x1p53))
            return value;
        char text[32];
        std::snprintf(text, sizeof(text), "%.15g", value);
        return std::strtod(text, nullptr);
    }
} // namespace

const char *aggregateName(Aggregate aggregate)
{
    switch (aggregate)
    {
    case Aggregate::Count:
        return "count";
    case Aggregate::Distinct:
        return "distinct";
    case Aggregate::Sum:
        return "sum";
    case Aggregate::Mean:
        return "mean";
    case Aggregate::Min:
        return "min";
    case Aggregate::Max:
        return "max";
    case Aggregate::StdDev:
        return "std dev";
    }
    return "?";
}

bool GroupByAccumulator::KeyLess::operator()(const std::vector<CellValue> &a, const std::vector<CellValue> &b) const
{
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        if (const int order = compareCells(a[i], b[i]); order != 0)
            return order < 0;
        if (a[i].index() != b[i].index())
            return a[i].index() < b[i].index();
        // Exact values apart; a NaN equals itself here, so later keys decide.
        if (a[i] != b[i])
        {
            if (const int text = formatCell(a[i]).compare(formatCell(b[i])); text != 0)
                return text < 0;
        }
    }
    return a.size() < b.size();
}

GroupByAccumulator::GroupByAccumulator(GroupBySpec spec, GroupByLimits limits)
    : spec_(std::move(spec)), limits_(limits)
{
    bool distinct = false;
    for (const Aggregate aggregate : spec_.aggregates)
        distinct = distinct || aggregate == Aggregate::Distinct;
    // Distinct values take memory in every group: only counted when asked for.
    stats_limits_.max_distinct = distinct ? std::numeric_limits<size_t>::max() : 0;
    stats_limits_.top_values = 0;
    key_.reserve(spec_.keys.size());
}

void GroupByAccumulator::add(const TableRow &row)
{
    const auto cell = [&row](size_t column) -> const CellValue &
    {
        static const CellValue empty;
        return column < row.size() ? row[column] : empty;
    };
    key_.clear();
    for (const size_t column : spec_.keys)
    {
        // NULL, "" and missing cells are one empty key.
        const CellValue &value = cell(column);
        key_.push_back(isEmptyCell(value) ? CellValue{} : value);
    }
    auto it = groups_.find(key_);
    if (it == groups_.end())
    {
        if (groups_.size() >= limits_.max_groups)
        {
            throw std::runtime_error("There are more than " + std::to_string(limits_.max_groups) +
                                     " groups; group by fewer or other columns");
        }
        Group group;
        group.values.assign(spec_.values.size(), ColumnStatsAccumulator(stats_limits_));
        it = groups_.emplace(key_, std::move(group)).first;
    }
    Group &group = it->second;
    ++group.rows;
    for (size_t i = 0; i < spec_.values.size(); ++i)
        group.values[i].add(cell(spec_.values[i]));
}

ResultSheet GroupByAccumulator::result(const std::function<std::string(size_t column)> &names) const
{
    ResultSheet sheet;
    for (const size_t column : spec_.keys)
        sheet.columns.push_back({names(column)});
    sheet.columns.push_back({"count"});
    for (const size_t column : spec_.values)
    {
        for (const Aggregate aggregate : spec_.aggregates)
            sheet.columns.push_back({names(column) + " " + aggregateName(aggregate)});
    }
    sheet.rows.reserve(groups_.size());
    for (const auto &[key, group] : groups_)
    {
        TableRow row = key;
        row.push_back(static_cast<int64_t>(group.rows));
        for (const ColumnStatsAccumulator &accumulator : group.values)
        {
            const ColumnStats stats = accumulator.result();
            const auto number = [](const std::optional<double> &value)
            { return value ? CellValue{roundSignificant(*value)} : CellValue{}; };
            for (const Aggregate aggregate : spec_.aggregates)
            {
                switch (aggregate)
                {
                case Aggregate::Count:
                    row.push_back(static_cast<int64_t>(stats.rows - stats.empty));
                    break;
                case Aggregate::Distinct:
                    row.push_back(static_cast<int64_t>(stats.distinct));
                    break;
                case Aggregate::Sum:
                    row.push_back(number(stats.sum));
                    break;
                case Aggregate::Mean:
                    // From the compensated sum, as a spreadsheet's AVERAGE.
                    row.push_back(number(stats.sum ? std::optional(*stats.sum / static_cast<double>(stats.numbers))
                                                   : std::nullopt));
                    break;
                case Aggregate::Min:
                    row.push_back(stats.min.value_or(CellValue{}));
                    break;
                case Aggregate::Max:
                    row.push_back(stats.max.value_or(CellValue{}));
                    break;
                case Aggregate::StdDev:
                    row.push_back(number(stats.stddev));
                    break;
                }
            }
        }
        sheet.rows.push_back(std::move(row));
    }
    return sheet;
}

std::optional<ResultSheet> groupByView(const std::shared_ptr<TableModel> &model, const GroupBySpec &spec,
                                       std::stop_token stop, const std::function<void(size_t rows)> &progress,
                                       GroupByLimits limits)
{
    GroupByAccumulator accumulator(spec, limits);
    const std::unique_ptr<ViewReader> reader = model->readView();
    size_t rows = 0;
    while (true)
    {
        const std::optional<ViewRows> chunk = reader->next(stop);
        if (!chunk)
            return std::nullopt;
        if (chunk->rows.empty())
            break;
        for (const TableRow *row : chunk->rows)
            accumulator.add(*row);
        rows += chunk->rows.size();
        if (progress)
            progress(rows);
        if (stop.stop_requested())
            return std::nullopt;
    }
    return accumulator.result([&model](size_t column)
                              { return model->columnName(column); });
}
