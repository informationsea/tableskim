#ifndef _SCATTER_HXX_
#define _SCATTER_HXX_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "histogram.hxx"
#include "table_types.hxx"

// A row whose X and Y values both lie on their axes, at their coordinates
// (see histogramCoordinate()).
struct ScatterPoint
{
    double x = 0;
    double y = 0;
    size_t view_row = 0;
};

// Pearson's correlation and the least-squares line of points, on their
// coordinates. Welford's updates keep them accurate for large values such
// as times in microseconds.
struct ScatterStats
{
    size_t n = 0;
    double mean_x = 0;
    double mean_y = 0;
    double m2_x = 0; // sums of squared deviations from the mean
    double m2_y = 0;
    double c_xy = 0; // sum of the products of the deviations

    void add(double x, double y);
    // nullopt with fewer than two points or a constant column.
    std::optional<double> correlation() const;
    // y = intercept + slope * x; nullopt with fewer than two points or a
    // constant X.
    std::optional<double> slope() const;
    std::optional<double> intercept() const;
};

// Two columns plotted against each other: points for a scatter plot and
// counts on a grid for a 2D density.
struct ScatterPlot
{
    // Coarse layouts of the columns, for the range and the ticks of the axes
    // (their counts are not used). An axis of kind None has no bins: its
    // column holds neither numbers nor times, and nothing is plotted.
    Histogram x_axis;
    Histogram y_axis;
    // The density grid: bins of each column (layouts, counts not used) and
    // the rows counted in each cell, at densityIndex().
    Histogram x_bins;
    Histogram y_bins;
    std::vector<size_t> counts;
    size_t pairs = 0;  // rows counted in a cell
    size_t others = 0; // rows with an empty value or a value of another kind
    size_t rows = 0;
    // Of every row counted, not only the sample.
    ScatterStats stats;
    // A uniform sample of the rows counted, at most ScatterOptions::max_points.
    std::shared_ptr<const std::vector<ScatterPoint>> points;

    bool plottable() const { return x_bins.binCount() > 0 && y_bins.binCount() > 0; }
    size_t densityIndex(size_t x_bin, size_t y_bin) const { return x_bin * y_bins.binCount() + y_bin; }
    size_t count(size_t x_bin, size_t y_bin) const { return counts[densityIndex(x_bin, y_bin)]; }
    // Whether the points are a sample of the rows counted.
    bool sampled() const { return points && points->size() < pairs; }
};

struct ScatterOptions
{
    size_t density_bins_x = 50; // about this many
    size_t density_bins_y = 50;
    size_t max_points = 200000; // 0: no points, the density only
};

// The first pass: the range of each column. Not thread-safe.
class ScatterRange
{
public:
    explicit ScatterRange(ColumnStatsLimits limits = {}) : x_(limits), y_(limits) {}
    // Values of the columns may come together (a row) or separately (the
    // distinct values of one column with their counts).
    void add(const CellValue &x, const CellValue &y);
    HistogramRange &x() { return x_; }
    HistogramRange &y() { return y_; }
    // Axes and empty density bins.
    ScatterPlot layout(const ScatterOptions &options) const;

private:
    HistogramRange x_;
    HistogramRange y_;
};

// The second pass: counts the rows into the density grid and samples points.
// The same view gives the same sample. Not thread-safe.
class ScatterCounter
{
public:
    ScatterCounter(ScatterPlot layout, size_t max_points);
    void add(const CellValue &x, const CellValue &y, size_t view_row);
    // A copy, with the points so far.
    ScatterPlot result() const;

private:
    ScatterPlot plot_;
    HistogramCounter x_;
    HistogramCounter y_;
    size_t max_points_;
    std::vector<ScatterPoint> points_;
    std::mt19937_64 random_;
};

#endif // _SCATTER_HXX_
