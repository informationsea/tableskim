#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>

#include "cell_format.hxx"
#include "scatter.hxx"
#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    ScatterState waitForScatter(TableModel &model, size_t x_column, size_t y_column, ScatterOptions options = {})
    {
        model.startScatter(x_column, y_column, options);
        ScatterState state;
        REQUIRE(waitFor([&]
                        { state = model.scatterState(); return state.done; }));
        return state;
    }

    // `books` keeps the book open for the model (see histogram_tests.cxx).
    std::shared_ptr<TableModel> openFirst(std::vector<std::shared_ptr<TableBook>> &books,
                                          const std::filesystem::path &path)
    {
        OpenOptions options;
        options.header = OpenOptions::Header::Yes;
        books.push_back(TableBook::open(path, options));
        auto model = books.back()->sheet(books.back()->sheetNames().front());
        REQUIRE(waitFor([&]
                        { return model->state() == ModelState::Ready; }));
        return model;
    }

    struct Row
    {
        CellValue x;
        CellValue y;
    };

    ScatterPlot plotRows(const std::vector<Row> &rows, ScatterOptions options = {})
    {
        ScatterRange range;
        for (const Row &row : rows)
            range.add(row.x, row.y);
        ScatterCounter counter(range.layout(options), options.max_points);
        for (size_t i = 0; i < rows.size(); ++i)
            counter.add(rows[i].x, rows[i].y, i);
        return counter.result();
    }

    bool keeps(const std::vector<FilterCondition> &conditions, const Row &row)
    {
        return std::all_of(conditions.begin(), conditions.end(), [&](const FilterCondition &condition)
                           { return cellMatchesCondition(condition.column == 0 ? row.x : row.y, condition); });
    }
} // namespace

TEST_CASE("ScatterRange and ScatterCounter", "[scatter]")
{
    SECTION("numbers and numeric text; empty and other values skipped")
    {
        const std::vector<Row> rows = {
            {int64_t{1}, 10.5}, {int64_t{2}, std::string("20")}, {std::string("3"), 30.0},
            {int64_t{4}, CellValue{}}, {std::string("n/a"), 40.0}, {int64_t{5}, 50.0},
        };
        const ScatterPlot plot = plotRows(rows);
        REQUIRE(plot.plottable());
        CHECK(plot.x_bins.kind == HistogramKind::Number);
        CHECK(plot.y_bins.kind == HistogramKind::Number);
        CHECK(plot.rows == 6);
        CHECK(plot.pairs == 4);
        CHECK(plot.others == 2);
        CHECK(std::accumulate(plot.counts.begin(), plot.counts.end(), size_t{0}) == 4);
        REQUIRE(plot.points);
        CHECK(plot.points->size() == 4);
        CHECK_FALSE(plot.sampled());
        CHECK(plot.points->at(1).x == 2);
        CHECK(plot.points->at(1).y == 20);
        CHECK(plot.points->at(3).view_row == 5);
        CHECK(scatterSummary(plot, true) == "4 rows plotted, 2 skipped (empty or not numbers or dates)");
    }
    SECTION("numbers against dates")
    {
        const std::vector<Row> rows = {
            {1.5, Date{19000}}, {2.5, std::string("2022-01-01")}, {3.5, DateTime{int64_t{19100} * 86400 * 1000000}},
        };
        const ScatterPlot plot = plotRows(rows);
        REQUIRE(plot.plottable());
        CHECK(plot.y_bins.kind == HistogramKind::Time);
        CHECK(plot.pairs == 3);
        CHECK(plot.points->at(0).y == 19000.0 * 86400 * 1000000);
        CHECK(scatterValueText(plot.y_axis, plot.points->at(0).y) == "2022-01-08 00:00:00");
    }
    SECTION("a Y column of neither numbers nor dates")
    {
        const ScatterPlot plot = plotRows({{1.0, std::string("a")}, {2.0, std::string("b")}});
        CHECK_FALSE(plot.plottable());
        CHECK(plot.pairs == 0);
        CHECK(scatterSummary(plot, true) == "The Y column holds no numbers or dates");
    }
}

TEST_CASE("boxStats: quartiles, whiskers and outliers", "[scatter]")
{
    CHECK(boxStats({}).count == 0);
    const BoxStats one = boxStats({7});
    CHECK(one.count == 1);
    CHECK(one.q1 == 7);
    CHECK(one.median == 7);
    CHECK(one.low_whisker == 7);
    CHECK(one.high_whisker == 7);

    // As QUARTILE.INC: 1 2 3 4 5 6 7 8 gives 2.75, 4.5, 6.25.
    const BoxStats box = boxStats({8, 3, 1, 2, 7, 4, 6, 5});
    CHECK(box.count == 8);
    CHECK(box.min == 1);
    CHECK(box.max == 8);
    CHECK(box.q1 == Catch::Approx(2.75));
    CHECK(box.median == Catch::Approx(4.5));
    CHECK(box.q3 == Catch::Approx(6.25));
    CHECK(box.mean == Catch::Approx(4.5));
    CHECK(box.low_whisker == 1);
    CHECK(box.high_whisker == 8);
    CHECK(box.outlier_count == 0);

    // Q1 10, Q3 14.25: whiskers reach up to 6.375 beyond, so to 7 and 16.
    const BoxStats out = boxStats({-50, 7, 10, 10, 11, 12, 12, 15, 16, 100, std::nan("")});
    CHECK(out.count == 10);
    CHECK(out.q1 == Catch::Approx(10));
    CHECK(out.q3 == Catch::Approx(14.25));
    CHECK(out.low_whisker == 7);
    CHECK(out.high_whisker == 16);
    CHECK(out.outlier_count == 2);
    CHECK(out.outliers == std::vector<double>{-50, 100});
}

TEST_CASE("Categorical X: categories, density and box plots", "[scatter]")
{
    using S = std::string;
    std::vector<Row> rows = {
        {S("b"), 1.0}, {S("a"), 2.0}, {S("b"), 3.0}, {S("c"), 4.0}, {S("b"), S("5")},
        {S("a"), 6.0}, {S(""), 7.0}, {S("d"), S("n/a")}, {S("b"), 9.0},
    };
    SECTION("the most frequent values first")
    {
        const ScatterPlot plot = plotRows(rows);
        REQUIRE(plot.plottable());
        REQUIRE(plot.categorical());
        CHECK(plot.categories == std::vector<CellValue>{S("b"), S("a"), S("c"), S("d")});
        CHECK_FALSE(plot.other_category);
        CHECK(plot.xBinCount() == 4);
        CHECK(plot.pairs == 7);
        CHECK(plot.others == 2);
        CHECK(plot.stats.n == 0);
        CHECK(scatterStatsText(plot).empty());
        REQUIRE(plot.boxes.size() == 4);
        CHECK(plot.boxes[0].count == 4);
        CHECK(plot.boxes[0].median == Catch::Approx(4));
        CHECK(plot.boxes[3].count == 0);
        CHECK(plot.points->at(0).x == 0);
        CHECK(plot.points->at(1).x == 1);
        CHECK(scatterSummary(plot, false) ==
              "7 rows plotted in 4 values of X, 2 skipped (empty or not numbers or dates)");
        CHECK(boxPlotText(plot, 1) == "a: 2 rows; min 2, Q1 3, median 4, Q3 5, max 6; mean 4");
        CHECK(scatterCategoryText(plot, 2) == "c");

        // A density cell's filter keeps exactly its rows; a box's, its category's.
        for (size_t xi = 0; xi < plot.xBinCount(); ++xi)
        {
            for (size_t yi = 0; yi < plot.y_bins.binCount(); ++yi)
            {
                const auto conditions = densityCellConditions(plot, 0, 1, xi, yi);
                const size_t kept = static_cast<size_t>(std::count_if(rows.begin(), rows.end(), [&](const Row &row)
                                                                      { return keeps(conditions, row); }));
                CHECK(kept == plot.count(xi, yi));
            }
            const auto conditions = scatterCategoryConditions(plot, 0, 1, xi);
            const size_t kept = static_cast<size_t>(std::count_if(rows.begin(), rows.end(), [&](const Row &row)
                                                                  { return keeps(conditions, row); }));
            CHECK(kept == plot.boxes[xi].count);
        }
        CHECK(scatterRangeConditions(plot, 0, 1, 0, 1, 0, 10).empty());
    }
    SECTION("the rest is (other)")
    {
        ScatterOptions options;
        options.max_categories = 2;
        const ScatterPlot plot = plotRows(rows, options);
        CHECK(plot.categories.size() == 2);
        CHECK(plot.other_category);
        CHECK(plot.xBinCount() == 3);
        REQUIRE(plot.boxes.size() == 3);
        CHECK(plot.boxes[2].count == 1); // c; d has no number
        CHECK(scatterCategoryText(plot, 2) == "(other)");
        CHECK(scatterCategoryConditions(plot, 0, 1, 2).empty());
        CHECK(densityCellConditions(plot, 0, 1, 2, 0).empty());
        CHECK(scatterSummary(plot, false).starts_with("7 rows plotted in the 2 most frequent values of X and (other)"));
    }
}

TEST_CASE("Categorical X from a sheet: scans and GROUP BY agree", "[scatter]")
{
    std::vector<std::shared_ptr<TableBook>> books;
    auto csv = openFirst(books, kDemoData / "normal.csv");
    auto parquet = openFirst(books, kDemoData / "normal.parquet");
    const ScatterState a = waitForScatter(*csv, 4, 6); // City, Salary
    const ScatterState b = waitForScatter(*parquet, 4, 6);
    REQUIRE(a.plot.categorical());
    CHECK(a.plot.categories == b.plot.categories);
    CHECK(a.plot.counts == b.plot.counts);
    CHECK(a.plot.boxes == b.plot.boxes);
    CHECK(a.plot.categories.front() == CellValue{std::string("Fukuoka")});
    CHECK(a.plot.boxes.front().count == 9);
}

TEST_CASE("ScatterCounter samples at most max_points, the same each time", "[scatter]")
{
    std::vector<Row> rows;
    for (int64_t i = 0; i < 5000; ++i)
        rows.push_back({i, static_cast<double>(i % 97)});
    ScatterOptions options;
    options.max_points = 300;
    const ScatterPlot a = plotRows(rows, options);
    const ScatterPlot b = plotRows(rows, options);
    CHECK(a.pairs == 5000);
    REQUIRE(a.points->size() == 300);
    CHECK(a.sampled());
    CHECK(scatterSummary(a, true) == "5000 rows plotted, a sample of 300 points");
    CHECK(scatterSummary(a, false) == "5000 rows plotted");
    for (size_t i = 0; i < a.points->size(); ++i)
    {
        CHECK(a.points->at(i).view_row == b.points->at(i).view_row);
        CHECK(a.points->at(i).x == static_cast<double>(a.points->at(i).view_row));
    }
    // A sample of the whole range, not its first rows.
    const auto last = std::max_element(a.points->begin(), a.points->end(), [](const auto &p, const auto &q)
                                       { return p.view_row < q.view_row; });
    CHECK(last->view_row > 4000);

    options.max_points = 0;
    CHECK_FALSE(plotRows(rows, options).points);
}

TEST_CASE("a density cell's filter keeps exactly the rows counted in it", "[scatter]")
{
    std::vector<Row> rows;
    for (int64_t i = 0; i < 400; ++i)
        rows.push_back({CellValue{static_cast<double>(i) * 0.37 - 20}, CellValue{(i * 7919) % 1000}});
    // Large integers, as in the histogram's regression test.
    for (int64_t i = 0; i < 4; ++i)
        rows.push_back({CellValue{1.0}, CellValue{9007199254740992LL + i}});
    ScatterOptions options;
    options.density_bins_x = 12;
    options.density_bins_y = 9;
    const ScatterPlot plot = plotRows(rows, options);
    REQUIRE(plot.plottable());
    for (size_t x = 0; x < plot.x_bins.binCount(); ++x)
    {
        for (size_t y = 0; y < plot.y_bins.binCount(); ++y)
        {
            INFO(densityCellText(plot, x, y));
            const auto conditions = densityCellConditions(plot, 0, 1, x, y);
            const auto kept = static_cast<size_t>(
                std::count_if(rows.begin(), rows.end(), [&](const Row &row) { return keeps(conditions, row); }));
            CHECK(plot.count(x, y) == kept);
        }
    }
}

TEST_CASE("scatterRangeConditions keep the rows in a rectangle", "[scatter]")
{
    std::vector<Row> rows;
    for (int64_t i = 0; i < 100; ++i)
        rows.push_back({CellValue{i}, CellValue{static_cast<double>(i) / 10}});
    const ScatterPlot plot = plotRows(rows);
    const auto conditions = scatterRangeConditions(plot, 0, 1, 10.4, 60.2, 2.55, 9.0);
    REQUIRE(conditions.size() == 6);
    CHECK(conditions[1] == FilterCondition{0, FilterOp::GreaterEqual, "11"});
    CHECK(conditions[2] == FilterCondition{0, FilterOp::LessEqual, "60"});
    const auto kept = std::count_if(rows.begin(), rows.end(), [&](const Row &row) { return keeps(conditions, row); });
    // x in [11, 60] and y = x / 10 in [2.55, 9]: x from 26 to 60.
    CHECK(kept == 35);
}

TEST_CASE("ScatterStats: correlation and least-squares line", "[scatter]")
{
    SECTION("correlation with large and small variances")
    {
        const double scale = GENERATE(1e100, 1e-100);
        ScatterStats line, inverse, uncorrelated;
        for (const double x : {-1.0, 0.0, 1.0})
        {
            line.add(x * scale, x * scale);
            inverse.add(x * scale, -x * scale);
            uncorrelated.add(x * scale, x * x * scale);
        }
        REQUIRE(line.correlation());
        CHECK(*line.correlation() == Catch::Approx(1.0));
        REQUIRE(inverse.correlation());
        CHECK(*inverse.correlation() == Catch::Approx(-1.0));
        REQUIRE(uncorrelated.correlation());
        CHECK(*uncorrelated.correlation() == Catch::Approx(0.0).margin(1e-15));
    }
    SECTION("a line")
    {
        std::vector<Row> rows;
        for (int64_t i = 0; i < 10; ++i)
            rows.push_back({CellValue{i}, CellValue{3.0 - 2.5 * static_cast<double>(i)}});
        const ScatterPlot plot = plotRows(rows);
        CHECK(plot.stats.n == 10);
        CHECK(plot.stats.correlation().value() == Catch::Approx(-1.0));
        CHECK(plot.stats.slope().value() == Catch::Approx(-2.5));
        CHECK(plot.stats.intercept().value() == Catch::Approx(3.0));
        CHECK(scatterStatsText(plot) == "r = -1.0000, R² = 1.0000, y = -2.5 x + 3");
    }
    SECTION("against a direct computation, with values far from zero")
    {
        std::vector<Row> rows;
        std::vector<double> xs, ys;
        for (int i = 0; i < 1000; ++i)
        {
            const double x = 1e9 + (i * 7919) % 1000;
            const double y = 0.5 * x + ((i * 104729) % 300) - 150;
            xs.push_back(x);
            ys.push_back(y);
            rows.push_back({CellValue{x}, CellValue{y}});
        }
        double mx = 0, my = 0;
        for (size_t i = 0; i < xs.size(); ++i)
        {
            mx += xs[i] - 1e9;
            my += ys[i] - 5e8;
        }
        mx = mx / static_cast<double>(xs.size()) + 1e9;
        my = my / static_cast<double>(ys.size()) + 5e8;
        double sxx = 0, syy = 0, sxy = 0;
        for (size_t i = 0; i < xs.size(); ++i)
        {
            sxx += (xs[i] - mx) * (xs[i] - mx);
            syy += (ys[i] - my) * (ys[i] - my);
            sxy += (xs[i] - mx) * (ys[i] - my);
        }
        const ScatterPlot plot = plotRows(rows);
        CHECK(plot.stats.correlation().value() == Catch::Approx(sxy / std::sqrt(sxx * syy)).epsilon(1e-9));
        CHECK(plot.stats.slope().value() == Catch::Approx(sxy / sxx).epsilon(1e-9));
    }
    SECTION("a constant column, too few rows, times")
    {
        const ScatterPlot constant = plotRows({{1.0, 5.0}, {2.0, 5.0}, {3.0, 5.0}});
        CHECK_FALSE(constant.stats.correlation());
        CHECK(constant.stats.slope().value() == Catch::Approx(0.0));
        CHECK(scatterStatsText(constant) == "r undefined (a column is constant)");
        CHECK(scatterStatsText(plotRows({{1.0, 5.0}})).empty());
        const ScatterPlot times = plotRows({{1.0, Date{19000}}, {2.0, Date{19001}}, {3.0, Date{19003}}});
        CHECK(times.stats.correlation().value() > 0.9);
        // No line in microseconds.
        CHECK(scatterStatsText(times).find("y =") == std::string::npos);
    }
}

TEST_CASE("TableModel::startScatter: scans and GROUP BY agree", "[scatter]")
{
    // Age against Salary, and Salary against Birthday.
    const auto [x, y] = GENERATE(std::pair<size_t, size_t>{3, 6}, std::pair<size_t, size_t>{6, 7});
    INFO("columns " << x << ", " << y);
    std::vector<std::shared_ptr<TableBook>> books;
    auto csv = openFirst(books, kDemoData / "normal.csv");
    auto parquet = openFirst(books, kDemoData / "normal.parquet");
    const ScatterState scanned = waitForScatter(*csv, x, y);
    const ScatterState grouped = waitForScatter(*parquet, x, y);
    const ScatterPlot &a = scanned.plot;
    const ScatterPlot &b = grouped.plot;
    REQUIRE(a.plottable());
    CHECK(scanned.x_column == x);
    CHECK(scanned.y_column == y);
    CHECK(a.x_bins.edges == b.x_bins.edges);
    CHECK(a.y_bins.edges == b.y_bins.edges);
    CHECK(a.counts == b.counts);
    CHECK(a.rows == 54);
    CHECK(a.pairs + a.others == 54);
    CHECK(a.pairs == b.pairs);
    CHECK(a.stats.n == a.pairs);
    REQUIRE(a.stats.correlation());
    CHECK(a.stats.correlation().value() == Catch::Approx(b.stats.correlation().value()));
    REQUIRE(a.points);
    CHECK(a.points->size() == a.pairs);
}

TEST_CASE("TableModel::startScatter: over the filtered rows, again after a view change", "[scatter]")
{
    std::vector<std::shared_ptr<TableBook>> books;
    auto model = openFirst(books, kDemoData / "normal.csv");
    const ScatterState all = waitForScatter(*model, 3, 6);
    REQUIRE(all.plot.plottable());

    // Filter to one cell: the plot starts over on its rows.
    size_t x_bin = 0, y_bin = 0;
    for (size_t i = 0; i < all.plot.counts.size(); ++i)
    {
        if (all.plot.counts[i] > 0)
        {
            x_bin = i / all.plot.y_bins.binCount();
            y_bin = i % all.plot.y_bins.binCount();
            break;
        }
    }
    const ViewSpec spec{.filter = Filter{Filter::Combine::And, densityCellConditions(all.plot, 3, 6, x_bin, y_bin)}};
    model->setView(spec);
    REQUIRE(waitFor([&]
                    { return model->view() == spec; }));
    CHECK(model->rowCount().value == all.plot.count(x_bin, y_bin));
    ScatterState filtered;
    REQUIRE(waitFor([&]
                    { filtered = model->scatterState(); return filtered.done && filtered.generation != all.generation; }));
    CHECK(filtered.plot.pairs == all.plot.count(x_bin, y_bin));
    for (const ScatterPoint &point : *filtered.plot.points)
        CHECK(point.view_row < model->rowCount().value);

    model->cancelScatter();
    CHECK_FALSE(model->scatterState().active);
}

TEST_CASE("defaultScatterColumns: columns of numbers or dates near the cursor", "[scatter]")
{
    std::vector<std::shared_ptr<TableBook>> books;
    // ID, Name, Email, Age, City, Department, Salary, Birthday, Active
    auto model = openFirst(books, kDemoData / "normal.csv");
    REQUIRE(waitFor([&]
                    { return model->peek(0, 0).state == CellState::Ready; }));
    CHECK(columnLooksPlottable(*model, 3, 0, 50));
    CHECK(columnLooksPlottable(*model, 7, 0, 50));
    CHECK_FALSE(columnLooksPlottable(*model, 2, 0, 50));
    CHECK_FALSE(columnLooksPlottable(*model, 8, 0, 50)); // TRUE / FALSE
    auto columns = [&](size_t column, HiddenColumns hidden = {})
    {
        const ScatterColumns c = defaultScatterColumns(*model, column, hidden, 0, 50);
        return std::pair{c.x, c.y};
    };
    CHECK(columns(6) == std::pair<size_t, size_t>{3, 6});
    // From a text column, the nearest number column, then the one left of it.
    CHECK(columns(2) == std::pair<size_t, size_t>{0, 3});
    CHECK(columns(4) == std::pair<size_t, size_t>{0, 3});
    // Nothing to the left: the nearest one to the right.
    CHECK(columns(0) == std::pair<size_t, size_t>{3, 0});
    CHECK(columns(8) == std::pair<size_t, size_t>{6, 7});
    HiddenColumns hidden;
    hidden.hide(3, 3, model->columnCount());
    CHECK(columns(6, hidden) == std::pair<size_t, size_t>{0, 6});
}

TEST_CASE("defaultScatterColumns: without numbers or dates, the column and its neighbour", "[scatter]")
{
    const auto path = std::filesystem::temp_directory_path() / "tableskim-scatter-text.csv";
    {
        std::ofstream out(path, std::ios::binary);
        out << "a,b,c\nx,y,z\nu,v,w\n";
    }
    std::vector<std::shared_ptr<TableBook>> books;
    auto model = openFirst(books, path);
    REQUIRE(waitFor([&]
                    { return model->peek(0, 0).state == CellState::Ready; }));
    const ScatterColumns middle = defaultScatterColumns(*model, 1, {}, 0, 50);
    CHECK(middle.x == 0);
    CHECK(middle.y == 1);
    const ScatterColumns first = defaultScatterColumns(*model, 0, {}, 0, 50);
    CHECK(first.x == 0);
    CHECK(first.y == 1);
    books.clear();
    model.reset();
    std::filesystem::remove(path);
}

TEST_CASE("scatterValueText: whole numbers in full", "[scatter]")
{
    Histogram axis;
    axis.kind = HistogramKind::Number;
    CHECK(scatterValueText(axis, 500000.0) == "500000");
    CHECK(scatterValueText(axis, 1279101.65) == "1279101.65");
    CHECK(scatterValueText(axis, -3.0) == "-3");
}
