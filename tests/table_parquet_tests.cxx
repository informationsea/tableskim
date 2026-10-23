#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "table.hxx"
#include "table_parquet.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    std::shared_ptr<TableModel> openSheet(TableBook &book, const std::string &name)
    {
        auto model = book.sheet(name);
        REQUIRE(model);
        REQUIRE(waitFor([&]
                        { return model->state() != ModelState::Opening; }));
        INFO(model->errorMessage());
        REQUIRE(model->state() == ModelState::Ready);
        return model;
    }
} // namespace

TEST_CASE("normal.parquet: one sheet with typed values")
{
    auto book = TableBook::open(kDemoData / "normal.parquet");
    REQUIRE(book->sheetNames() == std::vector<std::string>{"normal.parquet"});
    auto model = openSheet(*book, "normal.parquet");

    CHECK(model->columnName(1) == "Name");
    CHECK(model->rowCount() == RowCount{54, RowCount::Kind::Exact});
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK(cellText(*model, 0, 7) == "1986-08-02");
    CHECK(cellText(*model, 0, 8) == "TRUE");
}

TEST_CASE("parquet: sort and filter")
{
    auto book = TableBook::open(kDemoData / "normal.parquet");
    auto model = openSheet(*book, "normal.parquet");

    Filter filter;
    filter.conditions.push_back({.column = 3, .op = FilterOp::Greater, .value = "60"});
    ViewSpec spec{.sort = {{.column = 3, .ascending = false}}, .filter = filter};
    model->setView(spec);
    REQUIRE(waitFor([&]
                    { return model->view() == spec && model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 4);
    CHECK(cellText(*model, 0, 0) == "21");
    CHECK(cellText(*model, 1, 0) == "43");
}

TEST_CASE("parquet: dates, large files and non-ASCII names")
{
    {
        auto book = TableBook::open(kDemoData / "dates.parquet");
        auto model = openSheet(*book, "dates.parquet");
        CHECK(cellText(*model, 1, 2) == "1900-03-01 12:00:00");
        CHECK(cellText(*model, 1, 3) == "12:00:00");
    }
    {
        auto book = TableBook::open(kDemoData / "large.parquet");
        auto model = openSheet(*book, "large.parquet");
        CHECK(model->rowCount() == RowCount{10000, RowCount::Kind::Exact});
        model->requestRange(9990, 9999);
        CHECK(cellText(*model, 9999, 0) == "10000");
    }
    {
        const std::filesystem::path path = kDemoData / std::filesystem::path(u8"多言語サンプル.parquet");
        auto book = TableBook::open(path);
        auto model = openSheet(*book, "多言語サンプル.parquet");
        CHECK(cellText(*model, 0, 5) == "こんにちは");
    }
}

TEST_CASE("parquet: invalid file")
{
    const auto path = std::filesystem::temp_directory_path() / "tableskim-broken.parquet";
    {
        std::ofstream out(path, std::ios::binary);
        out << "PAR1 not really parquet";
    }
    CHECK_THROWS_AS(openParquetDataSource(path), std::runtime_error);
    std::filesystem::remove(path);
}

// Hidden (run with "[.super-large]"): one million rows, zstd-compressed.
TEST_CASE("super-large.parquet: paging, sort and filter", "[.super-large]")
{
    auto book = TableBook::open(kDemoData / "super-large.parquet");
    auto model = openSheet(*book, "super-large.parquet");
    CHECK(model->rowCount() == RowCount{1000000, RowCount::Kind::Exact});
    model->requestRange(999990, 999999);
    CHECK(cellText(*model, 999999, 0) == "1000000");
    CHECK(cellText(*model, 999999, 5) == "2026-12-03 00:47:24");

    Filter filter;
    filter.conditions.push_back({.column = 2, .op = FilterOp::Equals, .value = "epsilon"});
    ViewSpec spec{.sort = {{.column = 3, .ascending = true}}, .filter = filter};
    model->setView(spec);
    REQUIRE(waitFor([&]
                    { return model->view() == spec && model->rowCount().isExact(); }, std::chrono::minutes(2)));
    CHECK(model->rowCount().value > 0);
    CHECK(cellText(*model, 0, 2) == "epsilon");
}

// Hidden (run with "[.super-large]"): counted by DuckDB, not read page by page.
TEST_CASE("super-large.parquet: column statistics", "[.super-large]")
{
    auto book = TableBook::open(kDemoData / "super-large.parquet");
    auto model = openSheet(*book, "super-large.parquet");
    model->startColumnStats(2);
    REQUIRE(waitFor([&]
                    { return model->columnStatsState().done; }, std::chrono::seconds(10)));
    const ColumnStats stats = model->columnStatsState().stats;
    CHECK(stats.rows == 1000000);
    CHECK(stats.distinct_exact);
    REQUIRE_FALSE(stats.top.empty());
}

TEST_CASE("parquet: the path is not a glob pattern or hive partition", "[regression]")
{
    const auto dir = std::filesystem::temp_directory_path() / "tableskim-parquet-glob" / "year=2024";
    std::filesystem::remove_all(dir.parent_path());
    std::filesystem::create_directories(dir);
    // "x[1].parquet" would match "x1.parquet" as a glob pattern.
    std::filesystem::copy_file(kDemoData / "normal.parquet", dir / "x[1].parquet");
    std::filesystem::copy_file(kDemoData / "ragged.parquet", dir / "x1.parquet");

    {
        auto normal = TableBook::open(kDemoData / "normal.parquet");
        auto expected = openSheet(*normal, "normal.parquet");
        auto book = TableBook::open(dir / "x[1].parquet");
        auto model = openSheet(*book, "x[1].parquet");
        CHECK(model->rowCount() == expected->rowCount());
        // No "year" column from the directory name.
        CHECK(model->columnCount() == expected->columnCount());
        CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    }
    std::filesystem::remove_all(dir.parent_path());
}
