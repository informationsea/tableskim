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

// A box plot of values: quartiles by linear interpolation between the
// closest ranks (as spreadsheets' QUARTILE.INC), whiskers to the furthest
// values within 1.5 interquartile ranges of the box, and the values beyond
// them as outliers.
struct BoxStats
{
    static constexpr size_t MAX_OUTLIERS = 100; // kept on each side, the most extreme

    size_t count = 0;        // all values represented by the box
    size_t sample_count = 0; // values used for the statistics (count until sampled)
    double min = 0;
    double q1 = 0;
    double median = 0;
    double q3 = 0;
    double max = 0;
    double low_whisker = 0;
    double high_whisker = 0;
    double mean = 0;
    size_t outlier_count = 0;
    std::vector<double> outliers; // at most MAX_OUTLIERS below and above
    bool sampled() const { return sample_count < count; }
    bool operator==(const BoxStats &) const = default;
};
BoxStats boxStats(std::vector<double> values);

// Two columns plotted against each other: points for a scatter plot and
// counts on a grid for a 2D density.
//
// When X holds text (its axis has kind None) and Y numbers or times, X is
// categorical: its most frequent values are the categories, and the rest,
// if any, one more called "(other)". The density grid then has a column
// per category, the points have the category's index as X, and each
// category gets a box plot of its Y values.
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
    // Categorical X: the categories, most frequent first, whether "(other)"
    // follows them, and a box plot of each (of "(other)" last).
    std::vector<CellValue> categories;
    bool other_category = false;
    std::vector<BoxStats> boxes;

    bool categorical() const { return !categories.empty(); }
    // Columns of the density grid: X bins, or categories.
    size_t xBinCount() const { return categorical() ? categories.size() + (other_category ? 1 : 0) : x_bins.binCount(); }
    bool plottable() const { return xBinCount() > 0 && y_bins.binCount() > 0; }
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
    size_t max_categories = 30; // categorical X: the most frequent values shown apart
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
    // False if the row has a value outside the grid that a layout taken
    // with it would have covered: a number or time beyond an axis, or a
    // new value of a categorical X without "(other)" (rows added later).
    bool fits(const CellValue &x, const CellValue &y) const;

private:
    ScatterPlot plot_;
    HistogramCounter x_;
    HistogramCounter y_;
    size_t max_points_;
    std::vector<ScatterPoint> points_;
    std::mt19937_64 point_random_;
    std::mt19937_64 category_random_; // box samples do not depend on max_points_
    std::vector<std::vector<double>> category_values_; // Y of each category, for the box plots
    std::vector<size_t> category_seen_;                // values counted per category (a sample is kept past the cap)
    static constexpr size_t MAX_CATEGORY_VALUES = size_t{1} << 20;

    std::optional<size_t> categoryOf(const CellValue &x) const;
};

#endif // _SCATTER_HXX_
