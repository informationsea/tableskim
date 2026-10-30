#include "histogram.hxx"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "cell_format.hxx"

namespace
{
    constexpr int64_t MICROS_PER_SECOND = 1000000;
    constexpr int64_t MICROS_PER_DAY = 86400 * MICROS_PER_SECOND;
    // More bins than this are never made, whatever the target.
    constexpr size_t MAX_BINS = 10000;

    struct Classified
    {
        enum class Kind
        {
            Empty,
            Number,
            Time,
            Other,
        };
        Kind kind = Kind::Other;
        double number = 0;
        int64_t micros = 0;
        bool date = false; // a time without a time of day
    };

    Classified classify(const CellValue &value)
    {
        using Kind = Classified::Kind;
        if (isEmptyCell(value))
            return {Kind::Empty};
        std::optional<double> number;
        if (const auto *integer = std::get_if<int64_t>(&value))
            number = static_cast<double>(*integer);
        else if (const auto *real = std::get_if<double>(&value))
            number = *real;
        else if (const auto *date = std::get_if<Date>(&value))
            return {Kind::Time, 0, static_cast<int64_t>(date->days_since_epoch) * MICROS_PER_DAY, true};
        else if (const auto *time = std::get_if<DateTime>(&value))
            return {Kind::Time, 0, time->micros_since_epoch, false};
        else if (const auto *text = std::get_if<std::string>(&value))
        {
            number = parseNumber(*text);
            if (!number)
            {
                if (const auto temporal = parseTemporal(*text))
                    return {Kind::Time, 0, temporal->micros, !temporal->has_time};
                return {Kind::Other};
            }
        }
        if (!number || !std::isfinite(*number))
            return {Kind::Other};
        return {Kind::Number, *number};
    }

    int64_t floorDiv(int64_t a, int64_t b)
    {
        const int64_t q = a / b;
        return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
    }

    // 1, 2 or 5 times a power of ten, at least `raw`.
    double niceStep(double raw)
    {
        if (!(raw > 0))
            return 1;
        const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
        for (const double m : {1.0, 2.0, 5.0, 10.0})
        {
            if (m * magnitude >= raw * (1 - 1e-12))
                return m * magnitude;
        }
        return 10 * magnitude;
    }

    int64_t monthStartMicros(int64_t month_index)
    {
        using namespace std::chrono;
        const year_month_day ymd{year{static_cast<int>(floorDiv(month_index, 12))},
                                 month{static_cast<unsigned>(month_index - floorDiv(month_index, 12) * 12 + 1)},
                                 day{1}};
        return static_cast<int64_t>(sys_days{ymd}.time_since_epoch().count()) * MICROS_PER_DAY;
    }

    // The edge as a bin's filter writes it ("%.*f", see histogramEdgeText())
    // and reads it back, so that a bin counts exactly the rows it keeps.
    double decimalEdge(double edge, int digits)
    {
        const int size = std::snprintf(nullptr, 0, "%.*f", digits, edge);
        if (size <= 0)
            return edge;
        std::string text(static_cast<size_t>(size), '\0');
        std::snprintf(text.data(), text.size() + 1, "%.*f", digits, edge);
        return parseNumber(text).value_or(edge);
    }

    int64_t monthIndexOf(int64_t micros)
    {
        using namespace std::chrono;
        const year_month_day ymd{sys_days{days{floorDiv(micros, MICROS_PER_DAY)}}};
        return static_cast<int64_t>(static_cast<int>(ymd.year())) * 12 + static_cast<unsigned>(ymd.month()) - 1;
    }
} // namespace

std::vector<double> numberHistogramEdges(double min, double max, bool integers, size_t target, int *decimals)
{
    if (max < min)
        std::swap(min, max);
    target = std::clamp<size_t>(target, 1, MAX_BINS);
    int digits = 0;
    std::vector<double> edges;
    // Integers spanning no more values than bins wanted get a bin each. Up
    // to one more: steps of 1 would otherwise end on the maximum, making
    // the last bin of width 1 hold two values.
    if (integers && max - min <= static_cast<double>(target))
    {
        const size_t bins = static_cast<size_t>(max - min) + 1;
        for (size_t i = 0; i <= bins; ++i)
        {
            const double edge = min + static_cast<double>(i);
            if (edges.empty() || edge > edges.back())
                edges.push_back(edge);
        }
        // Beyond double's exact integer range, adding one may not advance.
        if (edges.back() <= max)
            edges.push_back(std::nextafter(max, std::numeric_limits<double>::infinity()));
    }
    else
    {
        const double range = max - min;
        double step = range > 0 ? niceStep(range / static_cast<double>(target))
                                : niceStep(std::max(std::fabs(min), 1.0) / 10);
        if (integers)
            step = std::max(step, 1.0);
        // The edges are multiples of the step. Past 2^50 steps from zero,
        // their indexes are no longer exact and neighbouring edges can be
        // equal, so that the edges no longer cover the range (1e15 + 0.125).
        if (const double extent = std::max(std::fabs(min), std::fabs(max)); extent > 0 && std::isfinite(extent))
            step = std::max(step, niceStep(extent / 0x1p50));
        if (!std::isfinite(step) || !std::isfinite(min / step) || !std::isfinite(max / step))
        {
            // Too wide for steps: one bin.
            edges = {min, max};
        }
        else
        {
            digits = std::max(0, static_cast<int>(-std::floor(std::log10(step) + 1e-9)));
            // Multiples of a step like 0.1 are not exact (3 * 0.1 > 0.3).
            auto edge = [&](double index)
            {
                const double raw = index * step;
                return digits > 0 ? decimalEdge(raw, digits) : raw;
            };
            double first = std::floor(min / step);
            // Rounding: the first edge must not lie above the minimum.
            if (edge(first) > min)
                --first;
            auto bins = static_cast<size_t>(std::max(1.0, std::ceil(max / step - first)));
            bins = std::min(bins, MAX_BINS);
            for (size_t i = 0; i <= bins; ++i)
                edges.push_back(edge(first + static_cast<double>(i)));
            // Rounding: the last edge must not fall short of the maximum.
            if (edges.back() < max)
                edges.push_back(edge(first + static_cast<double>(bins + 1)));
        }
    }
    if (decimals)
        *decimals = digits;
    return edges;
}

std::vector<double> timeHistogramEdges(int64_t min, int64_t max, bool dates, size_t target, TimeUnit *unit)
{
    struct Step
    {
        TimeUnit unit;
        int64_t count;
        int64_t micros; // approximate for months and years
    };
    constexpr int64_t S = MICROS_PER_SECOND;
    constexpr int64_t D = MICROS_PER_DAY;
    static const Step steps[] = {
        {TimeUnit::Second, 1, S},           {TimeUnit::Second, 5, 5 * S},       {TimeUnit::Second, 15, 15 * S},
        {TimeUnit::Second, 30, 30 * S},     {TimeUnit::Minute, 1, 60 * S},      {TimeUnit::Minute, 5, 300 * S},
        {TimeUnit::Minute, 15, 900 * S},    {TimeUnit::Minute, 30, 1800 * S},   {TimeUnit::Hour, 1, 3600 * S},
        {TimeUnit::Hour, 3, 3 * 3600 * S},  {TimeUnit::Hour, 6, 6 * 3600 * S},  {TimeUnit::Hour, 12, 12 * 3600 * S},
        {TimeUnit::Day, 1, D},              {TimeUnit::Day, 2, 2 * D},          {TimeUnit::Day, 7, 7 * D},
        {TimeUnit::Month, 1, 30 * D},       {TimeUnit::Month, 3, 91 * D},       {TimeUnit::Month, 6, 182 * D},
        {TimeUnit::Year, 1, 365 * D},       {TimeUnit::Year, 2, 730 * D},       {TimeUnit::Year, 5, 1826 * D},
        {TimeUnit::Year, 10, 3652 * D},     {TimeUnit::Year, 20, 7305 * D},     {TimeUnit::Year, 50, 18262 * D},
        {TimeUnit::Year, 100, 36524 * D},   {TimeUnit::Year, 1000, 365242 * D},
    };
    if (max < min)
        std::swap(min, max);
    target = std::clamp<size_t>(target, 1, MAX_BINS);
    const double range = static_cast<double>(max) - static_cast<double>(min);
    const Step *chosen = &steps[std::size(steps) - 1];
    for (const auto &step : steps)
    {
        if (dates && step.unit < TimeUnit::Day)
            continue;
        if (range / static_cast<double>(step.micros) <= static_cast<double>(target))
        {
            chosen = &step;
            break;
        }
    }
    if (unit)
        *unit = chosen->unit;

    std::vector<double> edges;
    if (chosen->unit == TimeUnit::Month || chosen->unit == TimeUnit::Year)
    {
        const int64_t months = chosen->count * (chosen->unit == TimeUnit::Year ? 12 : 1);
        int64_t index = floorDiv(monthIndexOf(min), months) * months;
        while (edges.size() <= MAX_BINS)
        {
            const int64_t edge = monthStartMicros(index);
            edges.push_back(static_cast<double>(edge));
            if (edge > max)
                break;
            index += months;
        }
    }
    else
    {
        const int64_t step = chosen->count * (chosen->unit == TimeUnit::Second   ? S
                                              : chosen->unit == TimeUnit::Minute ? 60 * S
                                              : chosen->unit == TimeUnit::Hour   ? 3600 * S
                                                                                 : D);
        // Weeks start on Monday (1970-01-05).
        const int64_t offset = chosen->unit == TimeUnit::Day && chosen->count == 7 ? 4 * D : 0;
        int64_t edge = floorDiv(min - offset, step) * step + offset;
        while (edges.size() <= MAX_BINS)
        {
            edges.push_back(static_cast<double>(edge));
            if (edge > max)
                break;
            edge += step;
        }
    }
    return edges;
}

std::optional<size_t> histogramBin(const std::vector<double> &edges, double x)
{
    if (edges.size() < 2 || !(x >= edges.front()) || x > edges.back())
        return std::nullopt;
    const size_t bins = edges.size() - 1;
    const auto it = std::upper_bound(edges.begin(), edges.end(), x);
    const size_t index = static_cast<size_t>(it - edges.begin()) - 1;
    return std::min(index, bins - 1);
}

void HistogramRange::add(const CellValue &value, size_t count)
{
    if (count == 0)
        return;
    stats_.add(value, count);
    const Classified c = classify(value);
    if (c.kind == Classified::Kind::Number)
    {
        // Rounded doubles must bound the original integer too; otherwise
        // the first edge can start above a minimum such as 2^53 + 3.
        const CellValue rounded{c.number};
        const int cmp = compareCells(value, rounded);
        const double lower = cmp < 0 ? std::nextafter(c.number, -std::numeric_limits<double>::infinity()) : c.number;
        const double upper = cmp > 0 ? std::nextafter(c.number, std::numeric_limits<double>::infinity()) : c.number;
        if (numbers_ == 0 || lower < min_number_)
            min_number_ = lower;
        if (numbers_ == 0 || upper > max_number_)
            max_number_ = upper;
        if (integers_ && c.number != std::floor(c.number))
            integers_ = false;
        numbers_ += count;
    }
    else if (c.kind == Classified::Kind::Time)
    {
        if (times_ == 0 || c.micros < min_time_)
            min_time_ = c.micros;
        if (times_ == 0 || c.micros > max_time_)
            max_time_ = c.micros;
        if (!c.date)
            dates_ = false;
        times_ += count;
    }
}

Histogram HistogramRange::layout(size_t target) const
{
    const ColumnStats stats = stats_.result();
    Histogram histogram;
    histogram.rows = stats.rows;
    histogram.empty = stats.empty;
    if (numbers_ > 0 && numbers_ >= times_)
    {
        histogram.kind = HistogramKind::Number;
        histogram.integers = integers_;
        histogram.edges = numberHistogramEdges(min_number_, max_number_, integers_, target, &histogram.decimals);
        histogram.values = numbers_;
    }
    else if (times_ > 0)
    {
        histogram.kind = HistogramKind::Time;
        histogram.dates = dates_;
        histogram.edges = timeHistogramEdges(min_time_, max_time_, dates_, target, &histogram.unit);
        histogram.values = times_;
    }
    else
        histogram.top = stats.top;
    histogram.others = stats.rows - stats.empty - histogram.values;
    histogram.counts.assign(histogram.edges.size() > 1 ? histogram.edges.size() - 1 : 0, 0);
    return histogram;
}

HistogramCounter::HistogramCounter(Histogram layout) : histogram_(std::move(layout))
{
    if (histogram_.kind == HistogramKind::Number)
    {
        for (size_t i = 0; i < histogram_.binCount(); ++i)
        {
            auto conditions = histogramBinConditions(histogram_, 0, i);
            number_lower_bounds_.push_back(std::move(conditions[1]));
            number_upper_bound_ = std::move(conditions[2]);
        }
    }
    std::fill(histogram_.counts.begin(), histogram_.counts.end(), 0);
    histogram_.values = 0;
    histogram_.others = 0;
    histogram_.empty = 0;
    histogram_.rows = 0;
}

void HistogramCounter::add(const CellValue &value, size_t count)
{
    histogram_.rows += count;
    if (isEmptyCell(value))
    {
        histogram_.empty += count;
        return;
    }
    if (const auto bin = binOf(value))
    {
        histogram_.counts[*bin] += count;
        histogram_.values += count;
    }
    else
        histogram_.others += count;
}

std::optional<size_t> HistogramCounter::binOf(const CellValue &value) const
{
    const Classified c = classify(value);
    if (c.kind == Classified::Kind::Number && histogram_.kind == HistogramKind::Number)
    {
        // Use the same comparisons as filtering the bin: converting an int64
        // to double first can move it across a boundary above 2^53.
        const auto it = std::partition_point(number_lower_bounds_.begin(), number_lower_bounds_.end(),
                                             [&](const FilterCondition &condition)
                                             { return cellMatchesCondition(value, condition); });
        if (it != number_lower_bounds_.begin() && cellMatchesCondition(value, number_upper_bound_))
            return static_cast<size_t>(it - number_lower_bounds_.begin()) - 1;
    }
    else if (c.kind == Classified::Kind::Time && histogram_.kind == HistogramKind::Time)
    {
        // A time bin's filter ends before its upper edge (histogramBinConditions()),
        // so a value appended on the last edge belongs to no bin.
        if (!histogram_.edges.empty() && static_cast<double>(c.micros) >= histogram_.edges.back())
            return std::nullopt;
        return histogramBin(histogram_.edges, static_cast<double>(c.micros));
    }
    return std::nullopt;
}

bool HistogramCounter::fits(const CellValue &value) const
{
    const auto coordinate = histogramCoordinate(histogram_.kind, value);
    if (!coordinate)
        return true;
    // A fraction makes a new layout's bins those of non-integers.
    if (histogram_.integers && *coordinate != std::floor(*coordinate))
        return false;
    return binOf(value).has_value();
}

std::optional<double> histogramCoordinate(HistogramKind kind, const CellValue &value)
{
    const Classified c = classify(value);
    if (c.kind == Classified::Kind::Number && kind == HistogramKind::Number)
        return c.number;
    if (c.kind == Classified::Kind::Time && kind == HistogramKind::Time)
        return static_cast<double>(c.micros);
    return std::nullopt;
}
