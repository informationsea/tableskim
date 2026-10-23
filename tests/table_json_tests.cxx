#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

#include "table.hxx"
#include "table_json.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    std::shared_ptr<TableModel> firstSheet(TableBook &book)
    {
        auto model = book.sheet(book.sheetNames().front());
        REQUIRE(model);
        REQUIRE(waitFor([&]
                        { return model->state() != ModelState::Opening; }));
        INFO(model->errorMessage());
        REQUIRE(model->state() == ModelState::Ready);
        return model;
    }

    std::vector<std::vector<std::string>> allTexts(TableModel &model)
    {
        model.requestRange(0, 1000);
        REQUIRE(waitFor([&]
                        { return model.rowCount().isExact(); }));
        std::vector<std::vector<std::string>> texts;
        for (size_t r = 0; r < model.rowCount().value; ++r)
        {
            std::vector<std::string> row;
            for (size_t c = 0; c < model.columnCount(); ++c)
                row.push_back(cellText(model, r, c));
            texts.push_back(std::move(row));
        }
        return texts;
    }

    std::filesystem::path tempFile(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / ("tableskim-test-" + name);
    }
} // namespace

TEST_CASE("JSON: arrays, NDJSON and gzip read like the Parquet file")
{
    const char *file = GENERATE("normal.json", "normal.ndjson", "normal.jsonl.gz");
    INFO(file);
    auto book = TableBook::open(kDemoData / file);
    CHECK(book->sheetNames() == std::vector<std::string>{file});
    auto model = firstSheet(*book);
    REQUIRE(model->columnCount() == 9);
    CHECK(model->columnName(0) == "ID");
    CHECK(model->columnName(8) == "Active");
    CHECK(model->viewCapabilities().sort);

    // Typed: numbers, dates (ISO text) and booleans.
    CHECK(std::holds_alternative<int64_t>(*waitForCell(*model, 0, 0).value));
    CHECK(std::holds_alternative<double>(*waitForCell(*model, 0, 6).value));
    CHECK(std::holds_alternative<Date>(*waitForCell(*model, 0, 7).value));
    CHECK(std::holds_alternative<bool>(*waitForCell(*model, 0, 8).value));
    CHECK(std::holds_alternative<std::monostate>(*waitForCell(*model, 52, 3).value)); // null

    auto parquet_book = TableBook::open(kDemoData / "normal.parquet");
    auto parquet = firstSheet(*parquet_book);
    CHECK(allTexts(*model) == allTexts(*parquet));

    // Sorted and filtered in DuckDB, like the Parquet file.
    ViewSpec spec;
    spec.sort = {{3, false}};
    spec.filter = Filter{Filter::Combine::And, {{4, FilterOp::Equals, "Osaka"}}};
    model->setView(spec);
    parquet->setView(spec);
    REQUIRE(waitFor([&]
                    { return model->view() == spec && parquet->view() == spec; }));
    CHECK(allTexts(*model) == allTexts(*parquet));
}

TEST_CASE("JSON: nested values, missing keys and changing types")
{
    auto book = TableBook::open(kDemoData / "nested.json");
    auto model = firstSheet(*book);
    REQUIRE(model->columnCount() == 5);
    CHECK(model->columnName(1) == "user");
    CHECK(model->columnName(4) == "note");
    // Nested objects and arrays are text.
    CHECK(cellText(*model, 0, 1).find("user1") != std::string::npos);
    CHECK(cellText(*model, 0, 2) == "[1, 2, 3]");
    // "code" turns to text after the first rows; every row still reads.
    CHECK(cellText(*model, 0, 3) == "1");
    CHECK(cellText(*model, 29, 3) == "C-30");
    CHECK(std::holds_alternative<std::monostate>(*waitForCell(*model, 0, 4).value));
    CHECK(cellText(*model, 3, 4) == "note 4");
}

TEST_CASE("JSON: mixed-type columns filter and sort by displayed values", "[regression]")
{
    const auto path = tempFile("mixed-values.json");
    {
        std::ofstream out(path, std::ios::binary);
        out << R"([{"id":0,"v":1},{"id":1,"v":"hello"},{"id":2,"v":""},)"
               R"({"id":3,"v":true},{"id":4,"v":null},{"id":5,"v":"10"},)"
               R"({"id":6,"v":2},{"id":7,"v":"2024-01-01"},{"id":8,"v":"hello"}])";
    }
    {
        auto data = openJsonDataSource(path, {});
        auto sheet = data->openSheet(data->sheetNames().front());
        const auto original = sheet->read(0, 100, {});
        REQUIRE(original->rows.size() == 9);
        CHECK(formatCell(original->rows[1][1]) == "hello");
        CHECK(isEmptyCell(original->rows[2][1]));
        for (const auto op : {FilterOp::Equals, FilterOp::NotEquals, FilterOp::Less, FilterOp::LessEqual,
                              FilterOp::Greater, FilterOp::GreaterEqual, FilterOp::Contains, FilterOp::NotContains,
                              FilterOp::Matches, FilterOp::NotMatches, FilterOp::IsEmpty, FilterOp::IsNotEmpty,
                              FilterOp::IsNumber, FilterOp::IsDate})
        {
            for (const char *value : {"hello", "9", "", "true", "2024-01-01"})
            {
                INFO("op=" << static_cast<int>(op) << " value=" << value);
                const FilterCondition condition{1, op, value};
                std::vector<TableRow> expected;
                for (const auto &row : original->rows)
                {
                    if (cellMatchesCondition(row[1], condition))
                        expected.push_back(row);
                }
                sheet->applyView({.filter = Filter{.conditions = {condition}}});
                CHECK(sheet->read(0, 100, {})->rows == expected);
            }
        }
        for (const bool ascending : {true, false})
        {
            auto expected = original->rows;
            std::stable_sort(expected.begin(), expected.end(), [ascending](const TableRow &a, const TableRow &b)
                             {
                if (isEmptyCell(a[1]) || isEmptyCell(b[1]))
                    return !isEmptyCell(a[1]) && isEmptyCell(b[1]);
                const int cmp = compareCells(a[1], b[1]);
                return ascending ? cmp < 0 : cmp > 0; });
            sheet->applyView({.sort = {{.column = 1, .ascending = ascending}}});
            CHECK(sheet->read(0, 100, {})->rows == expected);
        }
    }
    std::filesystem::remove(path);
}

TEST_CASE("JSON: SQL names the file without its extensions")
{
    auto book = TableBook::open(kDemoData / "normal.jsonl.gz");
    REQUIRE(book->supportsSql());
    auto promise = std::make_shared<std::promise<TableBook::SqlResult>>();
    auto future = promise->get_future();
    book->executeSql("SELECT count(*) AS n FROM normal WHERE Active", [promise](const TableBook::SqlResult &result)
                     { promise->set_value(result); });
    REQUIRE(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    const auto result = future.get();
    INFO(result.message);
    REQUIRE(result.ok);
    auto model = book->sheet(result.sheet);
    REQUIRE(model);
    CHECK(cellText(*model, 0, 0) != "0");
}

TEST_CASE("JSON: recognized by content, and unsupported compression")
{
    // No extension: text starting with "[" or "{" is JSON, not CSV.
    const auto plain = tempFile("json-without-extension");
    {
        std::ofstream out(plain, std::ios::binary);
        out << "\xEF\xBB\xBF  [{\"a\": 1, \"b\": \"x\"}, {\"a\": 2, \"b\": \"y\"}]\n";
    }
    {
        auto book = TableBook::open(plain);
        auto model = firstSheet(*book);
        CHECK(model->columnCount() == 2);
        CHECK(cellText(*model, 1, 1) == "y");
    }
    std::filesystem::remove(plain);

    // DuckDB reads gzip and zstd only.
    const auto bz2 = tempFile("data.json.bz2");
    std::filesystem::copy_file(kDemoData / "normal.csv.bz2", bz2, std::filesystem::copy_options::overwrite_existing);
    CHECK_THROWS(TableBook::open(bz2));
    std::filesystem::remove(bz2);

    CHECK_THROWS(TableBook::open(kDemoData / "normal.csv.gz", {}, [] {
        BackendRegistry registry;
        registerJsonBackend(registry);
        return registry;
    }()));
}
