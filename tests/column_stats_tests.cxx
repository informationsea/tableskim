#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "column_stats.hxx"

namespace
{
    ColumnStats statsOf(const std::vector<CellValue> &values, ColumnStatsLimits limits = {})
    {
        ColumnStatsAccumulator accumulator(limits);
        for (const auto &value : values)
            accumulator.add(value);
        return accumulator.result();
    }

    CellValue text(const char *s) { return CellValue{std::string(s)}; }
} // namespace

TEST_CASE("column statistics of nothing", "[stats]")
{
    const ColumnStats stats = statsOf({});
    CHECK(stats.rows == 0);
    CHECK(stats.distinct == 0);
    CHECK(stats.distinct_exact);
    CHECK_FALSE(stats.min);
    CHECK_FALSE(stats.max);
    CHECK_FALSE(stats.mean);
    CHECK(stats.top.empty());

    // Empty cells count as rows, but nothing else.
    const ColumnStats empty = statsOf({CellValue{}, text("")});
    CHECK(empty.rows == 2);
    CHECK(empty.empty == 2);
    CHECK(empty.distinct == 0);
    CHECK_FALSE(empty.min);
    CHECK(empty.top.empty());
}

TEST_CASE("column statistics of numeric text", "[stats]")
{
    const ColumnStats stats = statsOf({text("3"), text(" 1 "), text("2.5"), text(""), text("10"), text("3")});
    CHECK(stats.rows == 6);
    CHECK(stats.empty == 1);
    CHECK(stats.numbers == 5);
    CHECK(stats.distinct == 4);
    // Numeric order, not text order ("10" < "2.5").
    CHECK(stats.min == text(" 1 "));
    CHECK(stats.max == text("10"));
    CHECK(stats.sum == Catch::Approx(19.5));
    CHECK(stats.mean == Catch::Approx(3.9));
    // Sample standard deviation of 3, 1, 2.5, 10, 3.
    CHECK(stats.stddev == Catch::Approx(3.5071).epsilon(1e-4));
    REQUIRE(stats.top.size() == 4);
    CHECK(stats.top[0] == ValueCount{text("3"), 2});
    // Ties in sort order.
    CHECK(stats.top[1] == ValueCount{text(" 1 "), 1});
    CHECK(stats.top[2] == ValueCount{text("2.5"), 1});
    CHECK(stats.top[3] == ValueCount{text("10"), 1});
}

TEST_CASE("column statistics of mixed values follow the sort order", "[stats]")
{
    const ColumnStats stats = statsOf({text("banana"), text("42"), text("apple"), int64_t{7}, text("Cherry"), 1.5});
    CHECK(stats.numbers == 3);
    CHECK(stats.mean == Catch::Approx((42 + 7 + 1.5) / 3));
    // Numbers before text; text bytewise, so upper case first.
    CHECK(stats.min == CellValue{1.5});
    CHECK(stats.max == text("banana"));
    REQUIRE(stats.top.size() == 6);
    CHECK(stats.top[0].value == CellValue{1.5});
    CHECK(stats.top[1].value == CellValue{int64_t{7}});
    CHECK(stats.top[2].value == text("42"));
    CHECK(stats.top[3].value == text("Cherry"));
    CHECK(stats.top[5].value == text("banana"));

    // Only text: no numeric statistics.
    const ColumnStats words = statsOf({text("b"), text("a")});
    CHECK(words.numbers == 0);
    CHECK_FALSE(words.sum);
    CHECK(words.min == text("a"));
    CHECK(words.max == text("b"));
}

TEST_CASE("column statistics keep values of different types apart", "[stats]")
{
    const ColumnStats stats = statsOf({int64_t{1}, 1.0, true, int64_t{1}, Date{0}, Date{0}, DateTime{0}});
    CHECK(stats.distinct == 5);
    REQUIRE_FALSE(stats.top.empty());
    CHECK(stats.top[0].count == 2);
    // Booleans and dates are not numbers for the mean.
    CHECK(stats.numbers == 3);
    CHECK(stats.mean == Catch::Approx(1.0));
    CHECK(stats.min == CellValue{Date{0}});
}

TEST_CASE("column statistics stop counting new values at the limit", "[stats]")
{
    std::vector<CellValue> values;
    for (int i = 0; i < 10; ++i)
        values.push_back(text("same"));
    for (int i = 0; i < 100; ++i)
        values.push_back(CellValue{std::string("v") + std::to_string(i)});
    const ColumnStats stats = statsOf(values, {.max_distinct = 20, .top_values = 3});
    CHECK(stats.rows == 110);
    CHECK(stats.distinct == 20);
    CHECK_FALSE(stats.distinct_exact);
    REQUIRE(stats.top.size() == 3);
    CHECK(stats.top[0] == ValueCount{text("same"), 10});
    // Min and max still see every value.
    CHECK(stats.min == text("same"));
    CHECK(stats.max == text("v99"));
}

TEST_CASE("column statistics sum many numbers precisely", "[stats]")
{
    ColumnStatsAccumulator accumulator;
    accumulator.add(1e16);
    for (int i = 0; i < 1000; ++i)
        accumulator.add(1.0);
    accumulator.add(-1e16);
    const ColumnStats stats = accumulator.result();
    CHECK(stats.sum == 1000.0);
    // NaN is a number that sorts last.
    accumulator.add(text("nan"));
    CHECK(std::isnan(*accumulator.result().mean));
    CHECK(accumulator.result().max == text("nan"));
}

TEST_CASE("column statistics with infinities", "[stats]")
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    // inf - inf in the running sums used to make these NaN.
    ColumnStats stats = statsOf({inf});
    CHECK(stats.sum == inf);
    CHECK(stats.mean == inf);
    stats = statsOf({1.0, inf, 2.0});
    CHECK(stats.numbers == 3);
    CHECK(stats.sum == inf);
    CHECK(stats.mean == inf);
    REQUIRE(stats.stddev);
    CHECK(std::isnan(*stats.stddev));
    stats = statsOf({-inf, 5.0});
    CHECK(stats.mean == -inf);
    CHECK(std::isnan(*statsOf({inf, -inf}).mean));
    // Finite values alone are unaffected by what came before.
    stats = statsOf({1.0, 2.0, 3.0});
    CHECK(stats.mean == Catch::Approx(2.0));
    CHECK(stats.stddev == Catch::Approx(1.0));
}

TEST_CASE("column statistics can be read while values are added", "[stats]")
{
    ColumnStatsAccumulator accumulator;
    accumulator.add(text("a"));
    CHECK(accumulator.result().rows == 1);
    accumulator.add(text("a"));
    const ColumnStats stats = accumulator.result();
    CHECK(stats.rows == 2);
    REQUIRE(stats.top.size() == 1);
    CHECK(stats.top[0].count == 2);
}

TEST_CASE("column statistics of counted values match repeated values", "[stats]")
{
    const std::vector<std::pair<CellValue, size_t>> counted = {
        {text("3"), 4}, {CellValue{}, 2}, {text("x"), 3}, {2.5, 1}, {int64_t{-7}, 5}, {text(""), 1}};
    ColumnStatsAccumulator weighted;
    ColumnStatsAccumulator repeated;
    for (const auto &[value, count] : counted)
    {
        weighted.add(value, count);
        for (size_t i = 0; i < count; ++i)
            repeated.add(value);
    }
    const ColumnStats a = weighted.result();
    const ColumnStats b = repeated.result();
    CHECK(a.rows == b.rows);
    CHECK(a.empty == b.empty);
    CHECK(a.distinct == b.distinct);
    CHECK(a.numbers == b.numbers);
    CHECK(a.min == b.min);
    CHECK(a.max == b.max);
    CHECK(*a.sum == Catch::Approx(*b.sum));
    CHECK(*a.mean == Catch::Approx(*b.mean));
    CHECK(*a.stddev == Catch::Approx(*b.stddev));
    CHECK(a.top == b.top);
    weighted.add(text("y"), 0);
    CHECK(weighted.result().rows == a.rows);
}

TEST_CASE("column statistics of large finite values do not overflow intermediate squares", "[stats][regression]")
{
    for (const double value : {1e200, -1e200, 1e308, -1e308})
    {
        INFO(value);
        const auto stats = statsOf({value, value});
        CHECK(stats.mean == value);
        CHECK(stats.stddev == 0.0);
        ColumnStatsAccumulator counted;
        counted.add(value, 1000);
        CHECK(counted.result().mean == value);
        CHECK(counted.result().stddev == 0.0);
    }
    const auto wide = statsOf({-1e200, 1e200});
    CHECK(wide.mean == 0.0);
    REQUIRE(wide.stddev);
    CHECK(*wide.stddev / 1e200 == Catch::Approx(std::sqrt(2.0)));
    const auto tiny = statsOf({-1e-200, 1e-200});
    REQUIRE(tiny.stddev);
    CHECK(*tiny.stddev / 1e-200 == Catch::Approx(std::sqrt(2.0)));
}
