#include "scatter.hxx"

#include <algorithm>
#include <cmath>

namespace
{
    // Ticks wanted on an axis of a scatter plot.
    constexpr size_t AXIS_TICKS = 8;
    // Any fixed seed: the same rows give the same sample on every platform
    // (std::mt19937_64 is fully specified, unlike the distributions).
    constexpr uint64_t SAMPLE_SEED = 20261024;
} // namespace

void ScatterStats::add(double x, double y)
{
    ++n;
    const double dx = x - mean_x;
    mean_x += dx / static_cast<double>(n);
    const double dy = y - mean_y;
    mean_y += dy / static_cast<double>(n);
    m2_x += dx * (x - mean_x);
    m2_y += dy * (y - mean_y);
    c_xy += dx * (y - mean_y);
}

std::optional<double> ScatterStats::correlation() const
{
    if (n < 2 || !(m2_x > 0) || !(m2_y > 0))
        return std::nullopt;
    // Multiplying the variances first can overflow or underflow even when
    // both variances and the correlation are representable.
    return std::clamp(c_xy / std::sqrt(m2_x) / std::sqrt(m2_y), -1.0, 1.0);
}

std::optional<double> ScatterStats::slope() const
{
    if (n < 2 || !(m2_x > 0))
        return std::nullopt;
    return c_xy / m2_x;
}

std::optional<double> ScatterStats::intercept() const
{
    const auto b = slope();
    if (!b)
        return std::nullopt;
    return mean_y - *b * mean_x;
}

void ScatterRange::add(const CellValue &x, const CellValue &y)
{
    x_.add(x);
    y_.add(y);
}

BoxStats boxStats(std::vector<double> values)
{
    BoxStats box;
    std::erase_if(values, [](double value)
                  { return std::isnan(value); });
    if (values.empty())
        return box;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    const auto quantile = [&](double p)
    {
        const double h = static_cast<double>(n - 1) * p;
        const auto low = static_cast<size_t>(std::floor(h));
        const size_t high = std::min(low + 1, n - 1);
        return values[low] + (h - static_cast<double>(low)) * (values[high] - values[low]);
    };
    box.count = n;
    box.min = values.front();
    box.max = values.back();
    box.q1 = quantile(0.25);
    box.median = quantile(0.5);
    box.q3 = quantile(0.75);
    double sum = 0;
    for (const double value : values)
        sum += value;
    box.mean = sum / static_cast<double>(n);
    const double reach = 1.5 * (box.q3 - box.q1);
    const auto first = std::lower_bound(values.begin(), values.end(), box.q1 - reach);
    const auto last = std::upper_bound(values.begin(), values.end(), box.q3 + reach);
    // The whiskers end at values, within the box at least.
    box.low_whisker = first != values.end() ? std::min(*first, box.q1) : box.q1;
    box.high_whisker = last != values.begin() ? std::max(*(last - 1), box.q3) : box.q3;
    const auto below = static_cast<size_t>(first - values.begin());
    const auto above = static_cast<size_t>(values.end() - last);
    box.outlier_count = below + above;
    box.outliers.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(std::min(below, BoxStats::MAX_OUTLIERS)));
    box.outliers.insert(box.outliers.end(), values.end() - static_cast<std::ptrdiff_t>(std::min(above, BoxStats::MAX_OUTLIERS)),
                        values.end());
    return box;
}

ScatterPlot ScatterRange::layout(const ScatterOptions &options) const
{
    ScatterPlot plot;
    plot.x_axis = x_.layout(AXIS_TICKS);
    plot.y_axis = y_.layout(AXIS_TICKS);
    if (plot.y_axis.kind == HistogramKind::None)
        return plot;
    if (plot.x_axis.kind == HistogramKind::None)
    {
        // Text on X: its most frequent values are the categories.
        const auto &top = plot.x_axis.top;
        for (size_t i = 0; i < top.size() && i < options.max_categories; ++i)
            plot.categories.push_back(top[i].value);
        plot.other_category = top.size() > options.max_categories;
        if (plot.categories.empty())
            return plot;
    }
    else
        plot.x_bins = x_.layout(options.density_bins_x);
    plot.y_bins = y_.layout(options.density_bins_y);
    plot.counts.assign(plot.xBinCount() * plot.y_bins.binCount(), 0);
    return plot;
}

ScatterCounter::ScatterCounter(ScatterPlot layout, size_t max_points)
    : plot_(std::move(layout)), x_(plot_.x_bins), y_(plot_.y_bins), max_points_(max_points), random_(SAMPLE_SEED)
{
    std::fill(plot_.counts.begin(), plot_.counts.end(), 0);
    plot_.pairs = 0;
    plot_.others = 0;
    plot_.rows = 0;
    plot_.stats = {};
    plot_.points.reset();
    plot_.boxes.clear();
    category_values_.assign(plot_.categorical() ? plot_.xBinCount() : 0, {});
    category_seen_.assign(category_values_.size(), 0);
}

std::optional<size_t> ScatterCounter::categoryOf(const CellValue &x) const
{
    if (isEmptyCell(x))
        return std::nullopt;
    // Few categories: a linear search is quick enough.
    const auto it = std::find(plot_.categories.begin(), plot_.categories.end(), x);
    if (it != plot_.categories.end())
        return static_cast<size_t>(it - plot_.categories.begin());
    if (plot_.other_category)
        return plot_.categories.size();
    return std::nullopt;
}

void ScatterCounter::add(const CellValue &x, const CellValue &y, size_t view_row)
{
    ++plot_.rows;
    const bool categorical = plot_.categorical();
    const auto x_bin = !plot_.plottable() ? std::nullopt : categorical ? categoryOf(x) : x_.binOf(x);
    const auto y_bin = x_bin ? y_.binOf(y) : std::nullopt;
    if (!y_bin)
    {
        ++plot_.others;
        return;
    }
    ++plot_.counts[plot_.densityIndex(*x_bin, *y_bin)];
    ++plot_.pairs;
    const double y_coordinate = histogramCoordinate(plot_.y_bins.kind, y).value_or(0);
    double x_coordinate = 0;
    if (categorical)
    {
        // A correlation with category indices would mean nothing.
        x_coordinate = static_cast<double>(*x_bin);
        // Reservoir sampling beyond the cap: the box plot is exact up to
        // MAX_CATEGORY_VALUES values per category, and keeps memory bounded.
        auto &values = category_values_[*x_bin];
        const size_t seen = ++category_seen_[*x_bin];
        if (values.size() < MAX_CATEGORY_VALUES)
            values.push_back(y_coordinate);
        else if (const size_t slot = static_cast<size_t>(random_() % seen); slot < MAX_CATEGORY_VALUES)
            values[slot] = y_coordinate;
    }
    else
    {
        x_coordinate = histogramCoordinate(plot_.x_bins.kind, x).value_or(0);
        plot_.stats.add(x_coordinate, y_coordinate);
    }
    if (max_points_ == 0)
        return;
    // Reservoir sampling: every row counted so far is kept with the same
    // probability.
    size_t slot = points_.size();
    if (points_.size() >= max_points_)
    {
        slot = static_cast<size_t>(random_() % plot_.pairs);
        if (slot >= max_points_)
            return;
    }
    const ScatterPoint point{x_coordinate, y_coordinate, view_row};
    if (slot == points_.size())
        points_.push_back(point);
    else
        points_[slot] = point;
}

bool ScatterCounter::fits(const CellValue &x, const CellValue &y) const
{
    if (!plot_.plottable() || !y_.fits(y))
        return false;
    if (!plot_.categorical())
        return x_.fits(x);
    return isEmptyCell(x) || categoryOf(x).has_value();
}

ScatterPlot ScatterCounter::result() const
{
    ScatterPlot plot = plot_;
    if (max_points_ > 0)
        plot.points = std::make_shared<const std::vector<ScatterPoint>>(points_);
    for (const std::vector<double> &values : category_values_)
        plot.boxes.push_back(boxStats(values));
    return plot;
}
