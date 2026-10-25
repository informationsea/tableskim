#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <numeric>

#include "cell_format.hxx"
#include "histogram.hxx"
#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    int64_t micros(int y, unsigned m, unsigned d, int hours = 0)
    {
        using namespace std::chrono;
        const sys_days day{year{y} / month{m} / std::chrono::day{d}};
        return (static_cast<int64_t>(day.time_since_epoch().count()) * 24 + hours) * 3600 * 1000000;
    }

    HistogramState waitForHistogram(TableModel &model, size_t column)
    {
        model.startHistogram(column);
        HistogramState state;
        REQUIRE(waitFor([&]
                        { state = model.histogramState(); return state.done; }));
        return state;
    }

    // `books` keeps the book open for the model. Not a static: books left
    // for exit would close DuckDB after its thread-local state is gone.
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
} // namespace

TEST_CASE("numberHistogramEdges: nice widths covering the range")
{
    int decimals = -1;
    CHECK(numberHistogramEdges(0, 100, false, 10, &decimals) ==
          std::vector<double>{0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100});
    CHECK(decimals == 0);

    const auto tenths = numberHistogramEdges(0.13, 0.95, false, 10, &decimals);
    CHECK(decimals == 1);
    REQUIRE(tenths.size() == 10);
    CHECK(tenths.front() == 0.1);
    CHECK(tenths.back() >= 0.95);

    // Few integers: a bin each.
    CHECK(numberHistogramEdges(3, 7, true, 30) == std::vector<double>{3, 4, 5, 6, 7, 8});
    // Many integers: integral widths.
    const auto wide = numberHistogramEdges(0, 1000, true, 20);
    CHECK(wide.size() == 21);
    CHECK(wide[1] == 50);
    CHECK(numberHistogramEdges(0, 7, true, 8) == std::vector<double>{0, 1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(numberHistogramEdges(0, 7, true, 2) == std::vector<double>{0, 5, 10});
    CHECK(numberHistogramEdges(0, 70, true, 2) == std::vector<double>{0, 50, 100});

    // A single value; negative ranges.
    const auto single = numberHistogramEdges(2.5, 2.5, false, 30);
    REQUIRE(single.size() == 2);
    CHECK(single.front() <= 2.5);
    CHECK(single.back() >= 2.5);
    const auto negative = numberHistogramEdges(-5.5, 4.5, false, 10);
    CHECK(negative.front() <= -5.5);
    CHECK(negative.back() >= 4.5);
}

TEST_CASE("numberHistogramEdges: edges are the decimals the bin filters read", "[regression]")
{
    // 3 * 0.1 > 0.3: an edge computed so put 0.3 in the bin labelled 0.2 – 0.3.
    Histogram histogram;
    histogram.kind = HistogramKind::Number;
    histogram.edges = numberHistogramEdges(0, 1, false, 10, &histogram.decimals);
    for (int i = 0; i <= 100; ++i)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%.2f", i / 100.0);
        const CellValue cell{std::string(text)};
        for (size_t bin = 0; bin + 1 < histogram.edges.size(); ++bin)
        {
            INFO(text << " in bin " << bin);
            const auto conditions = histogramBinConditions(histogram, 0, bin);
            const bool kept = std::all_of(conditions.begin(), conditions.end(), [&](const FilterCondition &condition)
                                          { return cellMatchesCondition(cell, condition); });
            CHECK((histogramBin(histogram.edges, *parseNumber(text)) == bin) == kept);
        }
    }

    // Nor does the first edge lie above the minimum (1.7 / 0.1 rounds up).
    for (int k = 0; k < 3000; ++k)
    {
        const double min = k / 1000.0;
        INFO("min=" << min);
        CHECK(histogramBin(numberHistogramEdges(min, min + 0.37, false, 10), min).has_value());
    }
}

TEST_CASE("a bin's filter leaves out values of another kind", "[regression]")
{
    // Others used to pass the comparisons: "15 kg" as text, booleans and
    // dates as numbers, non-ISO date text as text.
    const std::vector<CellValue> numbers{int64_t{12}, 15.5, std::string("18"), std::string("15 kg"), true, false,
                                         Date{15}, std::string("nan"), std::numeric_limits<double>::infinity(),
                                         std::string(""), CellValue{}, int64_t{10}, std::string("19.9")};
    const std::vector<CellValue> times{Date{19723}, DateTime{19723LL * 86400 * 1000000 + 1}, std::string("2024-01-15"),
                                       std::string("2024-01-15T10:00:00Z"), std::string("2024"), int64_t{19723},
                                       std::string("2024-02-30"), Date{19760}, std::string("text"), true};
    for (const auto *values : {&numbers, &times})
    {
        HistogramRange range;
        for (const auto &value : *values)
            range.add(value);
        HistogramCounter counter(range.layout(5));
        for (const auto &value : *values)
            counter.add(value);
        const Histogram &histogram = counter.result();
        REQUIRE(histogram.binCount() > 0);
        CHECK(histogram.others > 0);
        for (size_t bin = 0; bin < histogram.binCount(); ++bin)
        {
            INFO("bin " << bin);
            const auto conditions = histogramBinConditions(histogram, 0, bin);
            size_t kept = 0;
            for (const auto &value : *values)
            {
                kept += std::all_of(conditions.begin(), conditions.end(), [&](const FilterCondition &condition)
                                    { return cellMatchesCondition(value, condition); });
            }
            CHECK(kept == histogram.counts[bin]);
        }
    }
}

TEST_CASE("numberHistogramEdges: large integers terminate and remain countable", "[regression]")
{
    const double min = GENERATE(9007199254740991.0, 9007199254740992.0, 10000000000000000.0,
                                -10000000000000000.0, 9223372036854775808.0, -9223372036854775808.0);
    const double span = GENERATE(0.0, 2.0, 16.0);
    const double max = min + span;
    INFO("min=" << min << " max=" << max);
    HistogramRange range;
    range.add(min);
    range.add(max);
    const Histogram layout = range.layout(30);
    REQUIRE(layout.edges.size() >= 2);
    CHECK(layout.edges.size() <= 31);
    CHECK(layout.edges.front() <= min);
    CHECK(layout.edges.back() > max);
    for (size_t i = 1; i < layout.edges.size(); ++i)
        CHECK(layout.edges[i] > layout.edges[i - 1]);

    HistogramCounter counter(layout);
    counter.add(min);
    counter.add(max);
    CHECK(counter.result().values == 2);
    CHECK(counter.result().others == 0);
    CHECK(std::accumulate(counter.result().counts.begin(), counter.result().counts.end(), size_t{0}) == 2);
}

TEST_CASE("numberHistogramEdges: a narrow range far from zero is covered", "[regression]")
{
    const std::vector<std::pair<double, double>> cases = {
        {1e15 + 0.125, 1e15 + 0.25},
        {9223372036854775807.0 - 5000, 9223372036854775807.0},
        {-1e17 - 64, -1e17},
        {0.1 + 0.2, 0.3},
    };
    for (const auto &[min, max] : cases)
    {
        INFO("min=" << min << " max=" << max);
        HistogramRange range;
        range.add(min);
        range.add(max);
        const Histogram layout = range.layout(30);
        REQUIRE(layout.edges.size() >= 2);
        CHECK(layout.edges.front() <= min);
        CHECK(layout.edges.back() >= max);
        for (size_t i = 1; i < layout.edges.size(); ++i)
            CHECK(layout.edges[i] > layout.edges[i - 1]);
        HistogramCounter counter(layout);
        counter.add(min);
        counter.add(max);
        CHECK(counter.result().values == 2);
        CHECK(counter.result().others == 0);
    }
}

TEST_CASE("histogram bins keep exactly the large integers they count", "[regression]")
{
    const std::vector<std::vector<int64_t>> cases = {
        {9007199254740992LL, 9007199254740995LL, 9007199254740996LL},
        {-9007199254740996LL, -9007199254740993LL, -9007199254740992LL},
        {9007199254740995LL},
        {std::numeric_limits<int64_t>::max() - 3, std::numeric_limits<int64_t>::max()},
        {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min() + 3},
    };
    for (const auto &integers : cases)
    {
        for (const bool text : {false, true})
        {
            std::vector<CellValue> values;
            HistogramRange range;
            for (const auto integer : integers)
            {
                values.push_back(text ? CellValue{std::to_string(integer)} : CellValue{integer});
                range.add(values.back());
            }
            HistogramCounter counter(range.layout(30));
            for (const auto &value : values)
                counter.add(value);
            const auto &histogram = counter.result();
            REQUIRE(histogram.binCount() > 0);
            CHECK(histogram.values == integers.size());
            for (size_t bin = 0; bin < histogram.binCount(); ++bin)
            {
                INFO("bin " << bin << ", text=" << text);
                const auto conditions = histogramBinConditions(histogram, 0, bin);
                size_t kept = 0;
                for (const auto &value : values)
                    kept += std::all_of(conditions.begin(), conditions.end(), [&](const FilterCondition &condition)
                                        { return cellMatchesCondition(value, condition); });
                CHECK(histogram.counts[bin] == kept);
            }
        }
    }
}

TEST_CASE("histogram edge text and filters retain very large and small numbers", "[regression]")
{
    for (const auto &values : {std::vector<CellValue>{1e100, 2e100}, std::vector<CellValue>{1e-100, 2e-100}})
    {
        HistogramRange range;
        for (const auto &value : values)
            range.add(value);
        HistogramCounter counter(range.layout(30));
        for (const auto &value : values)
            counter.add(value);
        const auto &histogram = counter.result();
        REQUIRE(histogram.binCount() > 0);
        CHECK(histogram.values == values.size());
        for (const double edge : histogram.edges)
            CHECK(parseNumber(histogramEdgeText(histogram, edge)) == edge);
        for (size_t bin = 0; bin < histogram.binCount(); ++bin)
        {
            const auto conditions = histogramBinConditions(histogram, 0, bin);
            size_t kept = 0;
            for (const auto &value : values)
                kept += std::all_of(conditions.begin(), conditions.end(), [&](const FilterCondition &condition)
                                    { return cellMatchesCondition(value, condition); });
            CHECK(histogram.counts[bin] == kept);
        }
    }
}

TEST_CASE("timeHistogramEdges: whole units from seconds to years")
{
    TimeUnit unit{};
    // Two months of dates: two-day bins.
    auto edges = timeHistogramEdges(micros(2024, 1, 1), micros(2024, 3, 1), true, 30, &unit);
    CHECK(unit == TimeUnit::Day);
    CHECK(edges[1] - edges[0] == 2.0 * 86400 * 1000000);

    // A year: months, from the first of the month.
    edges = timeHistogramEdges(micros(2024, 1, 15), micros(2024, 12, 20), true, 30, &unit);
    CHECK(unit == TimeUnit::Month);
    REQUIRE(edges.size() == 13);
    CHECK(edges.front() == static_cast<double>(micros(2024, 1, 1)));
    CHECK(edges[1] == static_cast<double>(micros(2024, 2, 1)));
    CHECK(edges.back() == static_cast<double>(micros(2025, 1, 1)));

    // Decades: years, from a multiple of the width.
    edges = timeHistogramEdges(micros(1961, 3, 1), micros(2004, 7, 1), true, 30, &unit);
    CHECK(unit == TimeUnit::Year);
    CHECK(edges.front() == static_cast<double>(micros(1960, 1, 1)));
    CHECK(edges[1] == static_cast<double>(micros(1962, 1, 1)));

    // Weeks start on Monday.
    edges = timeHistogramEdges(micros(2024, 1, 3), micros(2024, 4, 10), true, 30, &unit);
    CHECK(unit == TimeUnit::Day);
    CHECK(edges.front() == static_cast<double>(micros(2024, 1, 1))); // a Monday
    CHECK(edges[1] - edges[0] == 7.0 * 86400 * 1000000);

    // Times of day: 30-minute bins over 12 hours; dates never get bins
    // smaller than a day.
    edges = timeHistogramEdges(micros(2024, 1, 1, 8), micros(2024, 1, 1, 20), false, 30, &unit);
    CHECK(unit == TimeUnit::Minute);
    CHECK(edges.size() == 26);
    edges = timeHistogramEdges(micros(2024, 1, 1), micros(2024, 1, 1), true, 30, &unit);
    CHECK(unit == TimeUnit::Day);
    CHECK(edges.size() == 2);
}

TEST_CASE("histogramBin: half-open bins, the last one closed")
{
    const std::vector<double> edges = {0, 10, 20};
    CHECK(histogramBin(edges, 0) == 0);
    CHECK(histogramBin(edges, 9.99) == 0);
    CHECK(histogramBin(edges, 10) == 1);
    CHECK(histogramBin(edges, 20) == 1);
    CHECK_FALSE(histogramBin(edges, 20.01));
    CHECK_FALSE(histogramBin(edges, -1));
    CHECK_FALSE(histogramBin({5}, 5));
}

TEST_CASE("HistogramRange and HistogramCounter")
{
    SECTION("numbers, with numeric text; other values counted apart")
    {
        HistogramRange range;
        const std::vector<CellValue> cells = {int64_t{1},   std::string("2"), 3.5,   std::string("x"),
                                              CellValue{},  std::string(""),  int64_t{10}, std::string("2024-01-01")};
        for (const auto &cell : cells)
            range.add(cell);
        const Histogram layout = range.layout(30);
        CHECK(layout.kind == HistogramKind::Number);
        CHECK_FALSE(layout.integers);
        CHECK(layout.rows == 8);
        CHECK(layout.empty == 2);
        CHECK(layout.values == 4);
        CHECK(layout.others == 2);

        HistogramCounter counter(layout);
        for (const auto &cell : cells)
            counter.add(cell);
        const Histogram &result = counter.result();
        CHECK(std::accumulate(result.counts.begin(), result.counts.end(), size_t{0}) == 4);
        CHECK(result.values == 4);
        CHECK(result.others == 2);
        CHECK(result.empty == 2);
        CHECK(result.counts[*histogramBin(result.edges, 10)] == 1);
    }
    SECTION("dates and date text")
    {
        HistogramRange range;
        range.add(Date{19723}); // 2024-01-01
        range.add(std::string("2024-03-31"), 3);
        const Histogram layout = range.layout(30);
        CHECK(layout.kind == HistogramKind::Time);
        CHECK(layout.dates);
        CHECK(layout.values == 4);
        HistogramCounter counter(layout);
        counter.add(Date{19723});
        counter.add(std::string("2024-03-31"), 3);
        CHECK(counter.result().counts.front() == 1);
        CHECK(counter.result().counts.back() == 3);
    }
    SECTION("neither: the most frequent values")
    {
        HistogramRange range;
        range.add(std::string("b"), 2);
        range.add(std::string("a"), 5);
        const Histogram layout = range.layout(30);
        CHECK(layout.kind == HistogramKind::None);
        CHECK(layout.binCount() == 0);
        REQUIRE(layout.top.size() == 2);
        CHECK(layout.top[0].value == CellValue{std::string("a")});
        CHECK(layout.others == 7);
    }
}

TEST_CASE("TableModel::startHistogram: scans and GROUP BY agree")
{
    const size_t column = GENERATE(3, 6, 7, 4); // Age, Salary, Birthday, City
    INFO("column " << column);
    std::vector<std::shared_ptr<TableBook>> books;
    auto csv = openFirst(books, kDemoData / "normal.csv");
    auto parquet = openFirst(books, kDemoData / "normal.parquet");
    const HistogramState scanned = waitForHistogram(*csv, column);
    const HistogramState grouped = waitForHistogram(*parquet, column);
    const Histogram &a = scanned.histogram;
    const Histogram &b = grouped.histogram;
    CHECK(scanned.column == column);
    CHECK(a.kind == b.kind);
    CHECK(a.edges == b.edges);
    CHECK(a.counts == b.counts);
    CHECK(a.values == b.values);
    CHECK(a.empty == b.empty);
    CHECK(a.rows == 54);
    CHECK(std::accumulate(a.counts.begin(), a.counts.end(), size_t{0}) == a.values);
    if (column == 3)
    {
        CHECK(a.kind == HistogramKind::Number);
        CHECK(a.integers);
        CHECK(a.values == 53);
        CHECK(a.empty == 1);
    }
    if (column == 7)
    {
        CHECK(a.kind == HistogramKind::Time);
        CHECK(a.dates);
        CHECK(a.unit == TimeUnit::Year);
    }
    if (column == 4)
    {
        CHECK(a.kind == HistogramKind::None);
        REQUIRE_FALSE(a.top.empty());
        CHECK(a.top == b.top);
    }
}

TEST_CASE("TableModel::startHistogram: over the filtered rows, again after a view change")
{
    std::vector<std::shared_ptr<TableBook>> books;
    auto model = openFirst(books, kDemoData / "normal.parquet");
    const HistogramState all = waitForHistogram(*model, 3);
    const ViewSpec osaka{.filter = Filter{Filter::Combine::And, {{4, FilterOp::Equals, "Osaka"}}}};
    model->setView(osaka);
    REQUIRE(waitFor([&]
                    { return model->view() == osaka; }));
    HistogramState filtered;
    REQUIRE(waitFor([&]
                    { filtered = model->histogramState(); return filtered.done; }));
    CHECK(filtered.generation != all.generation);
    CHECK(filtered.histogram.values < all.histogram.values);
    CHECK(filtered.histogram.values == model->rowCount().value);

    model->cancelHistogram();
    CHECK_FALSE(model->histogramState().active);
}

TEST_CASE("histogram texts")
{
    Histogram numbers;
    numbers.kind = HistogramKind::Number;
    numbers.edges = numberHistogramEdges(0.13, 0.95, false, 10, &numbers.decimals);
    numbers.counts.assign(numbers.edges.size() - 1, 0);
    CHECK(histogramBinLabel(numbers, 0) == "0.1 – 0.2");
    CHECK(histogramBinRange(numbers, 0) == "[0.1, 0.2)");
    CHECK(histogramBinRange(numbers, numbers.binCount() - 1) == "[0.9, 1.0]");
    const auto conditions = histogramBinConditions(numbers, 2, 1);
    REQUIRE(conditions.size() == 3);
    CHECK(conditions[0] == FilterCondition{2, FilterOp::IsNumber, ""});
    CHECK(conditions[1] == FilterCondition{2, FilterOp::GreaterEqual, "0.2"});
    CHECK(conditions[2] == FilterCondition{2, FilterOp::Less, "0.3"});
    CHECK(histogramBinConditions(numbers, 2, numbers.binCount() - 1)[2].op == FilterOp::LessEqual);

    Histogram integers;
    integers.kind = HistogramKind::Number;
    integers.integers = true;
    integers.edges = numberHistogramEdges(3, 7, true, 30);
    CHECK(histogramBinLabel(integers, 0) == "3");

    Histogram months;
    months.kind = HistogramKind::Time;
    months.dates = true;
    months.edges = timeHistogramEdges(micros(2024, 1, 15), micros(2024, 12, 20), true, 30, &months.unit);
    months.counts.assign(months.edges.size() - 1, 0);
    months.values = 3;
    months.empty = 1;
    CHECK(histogramBinLabel(months, 1) == "2024-02");
    CHECK(histogramBinRange(months, 11) == "[2024-12, 2025-01)");
    CHECK(histogramBinConditions(months, 0, 1)[0].op == FilterOp::IsDate);
    CHECK(histogramBinConditions(months, 0, 1)[1].value == "2024-02-01");
    CHECK(histogramBinConditions(months, 0, 11)[2] == FilterCondition{0, FilterOp::Less, "2025-01-01"});
    CHECK(histogramSummary(months) == "3 dates in 12 bins, 1 empty");

    Histogram hours;
    hours.kind = HistogramKind::Time;
    hours.edges = timeHistogramEdges(micros(2024, 1, 1, 8), micros(2024, 1, 1, 20), false, 30, &hours.unit);
    CHECK(histogramBinLabel(hours, 1) == "2024-01-01 08:30");
    CHECK(histogramBinConditions(hours, 0, 1)[1].value == "2024-01-01 08:30:00");
}

TEST_CASE("time bins ending past the year 9999 keep their rows", "[regression]")
{
    // 9999-12-31 as a "no end" date: the last edge is in the year 10000.
    const std::vector<std::vector<CellValue>> cases = {
        {Date{2932896}},
        {Date{18262}, Date{2932896}},
        {CellValue{std::string("9999-12-31")}, CellValue{std::string("2020-01-01")}},
        {DateTime{micros(9999, 12, 31, 23)}, DateTime{micros(9999, 12, 30)}},
    };
    for (const auto &values : cases)
    {
        HistogramRange range;
        for (const auto &value : values)
            range.add(value);
        HistogramCounter counter(range.layout(30));
        for (const auto &value : values)
            counter.add(value);
        const auto &histogram = counter.result();
        REQUIRE(histogram.binCount() > 0);
        CHECK(histogram.values == values.size());
        CHECK(histogramEdgeText(histogram, histogram.edges.back()).starts_with("10000"));
        for (size_t bin = 0; bin < histogram.binCount(); ++bin)
        {
            INFO(histogramBinRange(histogram, bin));
            const auto conditions = histogramBinConditions(histogram, 0, bin);
            size_t kept = 0;
            for (const auto &value : values)
                kept += std::all_of(conditions.begin(), conditions.end(), [&](const FilterCondition &condition)
                                    { return cellMatchesCondition(value, condition); });
            CHECK(histogram.counts[bin] == kept);
        }
    }
}

TEST_CASE("a bin's filter keeps the rows counted in it")
{
    const size_t column = GENERATE(3, 6, 7); // Age, Salary, Birthday
    INFO("column " << column);
    std::vector<std::shared_ptr<TableBook>> books;
    auto model = openFirst(books, kDemoData / "normal.csv");
    const Histogram histogram = waitForHistogram(*model, column).histogram;
    for (size_t bin = 0; bin < histogram.binCount(); ++bin)
    {
        if (histogram.counts[bin] == 0)
            continue;
        INFO(histogramBinRange(histogram, bin));
        const ViewSpec spec{.filter = Filter{Filter::Combine::And, histogramBinConditions(histogram, column, bin)}};
        model->setView(spec);
        REQUIRE(waitFor([&]
                        { return model->view() == spec; }));
        CHECK(model->rowCount().value == histogram.counts[bin]);
    }
}
