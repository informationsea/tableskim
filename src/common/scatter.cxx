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
    return std::clamp(c_xy / std::sqrt(m2_x * m2_y), -1.0, 1.0);
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

ScatterPlot ScatterRange::layout(const ScatterOptions &options) const
{
    ScatterPlot plot;
    plot.x_axis = x_.layout(AXIS_TICKS);
    plot.y_axis = y_.layout(AXIS_TICKS);
    if (plot.x_axis.kind == HistogramKind::None || plot.y_axis.kind == HistogramKind::None)
        return plot;
    plot.x_bins = x_.layout(options.density_bins_x);
    plot.y_bins = y_.layout(options.density_bins_y);
    plot.counts.assign(plot.x_bins.binCount() * plot.y_bins.binCount(), 0);
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
}

void ScatterCounter::add(const CellValue &x, const CellValue &y, size_t view_row)
{
    ++plot_.rows;
    const auto x_bin = plot_.plottable() ? x_.binOf(x) : std::nullopt;
    const auto y_bin = x_bin ? y_.binOf(y) : std::nullopt;
    if (!y_bin)
    {
        ++plot_.others;
        return;
    }
    ++plot_.counts[plot_.densityIndex(*x_bin, *y_bin)];
    ++plot_.pairs;
    const double x_coordinate = histogramCoordinate(plot_.x_bins.kind, x).value_or(0);
    const double y_coordinate = histogramCoordinate(plot_.y_bins.kind, y).value_or(0);
    plot_.stats.add(x_coordinate, y_coordinate);
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

ScatterPlot ScatterCounter::result() const
{
    ScatterPlot plot = plot_;
    if (max_points_ > 0)
        plot.points = std::make_shared<const std::vector<ScatterPoint>>(points_);
    return plot;
}
