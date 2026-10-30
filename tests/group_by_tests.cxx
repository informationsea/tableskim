#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <limits>
#include <stdexcept>

#include "group_by.hxx"
#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    // `books` keeps the book open for the model.
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

    std::string letters(size_t column)
    {
        return columnLetter(column);
    }

    std::vector<std::string> columnNames(const ResultSheet &sheet)
    {
        std::vector<std::string> names;
        for (const auto &column : sheet.columns)
            names.push_back(column.name);
        return names;
    }

    std::vector<std::string> rowText(const TableRow &row)
    {
        std::vector<std::string> texts;
        for (const auto &value : row)
            texts.push_back(formatCell(value));
        return texts;
    }

    // The rows of a sheet as text, once it is read.
    std::vector<std::vector<std::string>> sheetText(TableModel &model)
    {
        REQUIRE(waitFor([&]
                        { return model.state() == ModelState::Ready && model.rowCount().isExact(); }));
        std::vector<std::vector<std::string>> rows;
        for (size_t r = 0; r < model.rowCount().value; ++r)
        {
            std::vector<std::string> row;
            for (size_t c = 0; c < model.columnCount(); ++c)
                row.push_back(cellText(model, r, c));
            rows.push_back(std::move(row));
        }
        return rows;
    }
} // namespace

TEST_CASE("GroupByAccumulator: groups in sort order with their aggregates")
{
    using S = std::string;
    GroupByAccumulator accumulator({.keys = {0},
                                    .values = {1},
                                    .aggregates = {Aggregate::Count, Aggregate::Distinct, Aggregate::Sum,
                                                   Aggregate::Mean, Aggregate::Min, Aggregate::Max,
                                                   Aggregate::StdDev}});
    accumulator.add({S("b"), S("10")});
    accumulator.add({S("a"), int64_t{4}});
    accumulator.add({S("b"), S("x")});
    accumulator.add({S("b"), S("10")});
    accumulator.add({S(""), S("1")});
    accumulator.add({std::monostate{}, S("")});
    accumulator.add({}); // a ragged row: empty key and value
    accumulator.add({S("10"), 2.5});
    accumulator.add({S("9"), int64_t{1}});
    CHECK(accumulator.groups() == 5);

    const ResultSheet sheet = accumulator.result(letters);
    CHECK(columnNames(sheet) == std::vector<std::string>{"A", "count", "B count", "B distinct", "B sum", "B mean",
                                                         "B min", "B max", "B std dev"});
    REQUIRE(sheet.rows.size() == 5);
    // Numbers first (numerically), then text, then the empty key.
    CHECK(rowText(sheet.rows[0]) == std::vector<std::string>{"9", "1", "1", "1", "1", "1", "1", "1", ""});
    CHECK(rowText(sheet.rows[1]) == std::vector<std::string>{"10", "1", "1", "1", "2.5", "2.5", "2.5", "2.5", ""});
    CHECK(rowText(sheet.rows[2]) == std::vector<std::string>{"a", "1", "1", "1", "4", "4", "4", "4", ""});
    // Numbers count for the sum and mean; text sorts after them for min / max.
    CHECK(rowText(sheet.rows[3]) == std::vector<std::string>{"b", "3", "3", "2", "20", "10", "10", "x", "0"});
    CHECK(rowText(sheet.rows[4]) == std::vector<std::string>{"", "3", "1", "1", "1", "1", "1", "1", ""});
    CHECK(std::holds_alternative<std::monostate>(sheet.rows[4][0]));
    CHECK(std::holds_alternative<int64_t>(sheet.rows[0][1]));
}

TEST_CASE("GroupByAccumulator: several keys, values that sort alike stay apart")
{
    using S = std::string;
    GroupByAccumulator accumulator({.keys = {0, 1}, .values = {}, .aggregates = {Aggregate::Sum}});
    accumulator.add({S("1"), S("x")});
    accumulator.add({S("01"), S("x")});
    accumulator.add({int64_t{1}, S("x")});
    accumulator.add({S("1"), S("y")});
    accumulator.add({S("1"), S("x")});
    const ResultSheet sheet = accumulator.result(letters);
    CHECK(columnNames(sheet) == std::vector<std::string>{"A", "B", "count"});
    REQUIRE(sheet.rows.size() == 4);
    size_t total = 0;
    for (const auto &row : sheet.rows)
        total += static_cast<size_t>(std::get<int64_t>(row[2]));
    CHECK(total == 5);
}

TEST_CASE("GroupByAccumulator: a NaN key leaves the later keys apart", "[regression]")
{
    using S = std::string;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    GroupByAccumulator accumulator({.keys = {0, 1}});
    accumulator.add({nan, S("x")});
    accumulator.add({nan, S("y")});
    accumulator.add({nan, S("x")});
    const ResultSheet sheet = accumulator.result(letters);
    REQUIRE(sheet.rows.size() == 2);
    CHECK(rowText(sheet.rows[0])[1] == "x");
    CHECK(rowText(sheet.rows[0])[2] == "2");
    CHECK(rowText(sheet.rows[1])[1] == "y");
    CHECK(rowText(sheet.rows[1])[2] == "1");
}

TEST_CASE("GroupByAccumulator: no keys make one group; too many groups fail")
{
    GroupByAccumulator all({.keys = {}, .values = {0}, .aggregates = {Aggregate::Sum}});
    for (int64_t i = 1; i <= 4; ++i)
        all.add({i});
    const ResultSheet sheet = all.result(letters);
    REQUIRE(sheet.rows.size() == 1);
    CHECK(rowText(sheet.rows[0]) == std::vector<std::string>{"4", "10"});

    GroupByAccumulator limited({.keys = {0}}, {.max_groups = 3});
    for (int64_t i = 0; i < 3; ++i)
        limited.add({i});
    limited.add({int64_t{0}});
    CHECK_THROWS_AS(limited.add({int64_t{3}}), std::runtime_error);
}

TEST_CASE("groupByView: the rows of the view, alike for scans and databases")
{
    const auto file = GENERATE(as<std::string>{}, "normal.csv", "normal.parquet", "normal.xlsx");
    INFO(file);
    std::vector<std::shared_ptr<TableBook>> books;
    auto model = openFirst(books, kDemoData / file);
    const GroupBySpec spec{.keys = {4}, // City
                           .values = {3, 5}, // Age, Department
                           .aggregates = {Aggregate::Sum, Aggregate::Min, Aggregate::Max, Aggregate::Distinct}};
    size_t progress = 0;
    const auto sheet = groupByView(model, spec, {}, [&](size_t rows)
                                   { progress = rows; });
    REQUIRE(sheet);
    CHECK(progress == 54);
    CHECK(columnNames(*sheet) == std::vector<std::string>{"City", "count", "Age sum", "Age min", "Age max",
                                                          "Age distinct", "Department sum", "Department min",
                                                          "Department max", "Department distinct"});
    REQUIRE(sheet->rows.size() == 12);
    CHECK(rowText(sheet->rows[0]).at(0) == " Osaka ");
    CHECK(rowText(sheet->rows[2]) == std::vector<std::string>{"Fukuoka", "9", "389", "20", "57", "8", "", "Marketing",
                                                              "人事部", "4"});
    CHECK(rowText(sheet->rows[10]).at(0) == "仙台");
    CHECK(rowText(sheet->rows[11]).at(0).empty());

    SECTION("over the filtered rows")
    {
        const ViewSpec tokyo{.filter = Filter{Filter::Combine::And, {{4, FilterOp::Equals, "Tokyo"}}}};
        model->setView(tokyo);
        REQUIRE(waitFor([&]
                        { return model->view() == tokyo; }));
        const auto filtered = groupByView(model, spec);
        REQUIRE(filtered);
        REQUIRE(filtered->rows.size() == 1);
        CHECK(rowText(filtered->rows[0]).at(2) == "322");
    }
    SECTION("stopped")
    {
        std::stop_source stop;
        stop.request_stop();
        CHECK_FALSE(groupByView(model, spec, stop.get_token()));
    }
}

TEST_CASE("TableBook::addResultSheet: a sheet of its own, replaced by the next result")
{
    auto book = TableBook::open(kDemoData / "multisheet.xlsx");
    const auto file_sheets = book->sheetNames();
    ResultSheet first_sheet;
    first_sheet.columns = {{"key"}, {"count"}};
    first_sheet.rows = {{std::string("a"), int64_t{2}}, {std::string("b"), int64_t{1}}};
    const auto first = ResultRows::make(std::move(first_sheet));
    const std::string name = book->addResultSheet("Group By", first);
    CHECK(name == "Group By");
    auto expected = file_sheets;
    expected.push_back("Group By");
    CHECK(book->sheetNames() == expected);

    auto model = book->sheet(name);
    REQUIRE(model);
    CHECK(sheetText(*model) == std::vector<std::vector<std::string>>{{"a", "2"}, {"b", "1"}});
    CHECK(model->columnName(1) == "count");
    CHECK(model->hasHeader());
    CHECK(model->editKind() == CellEditKind::None);

    SECTION("sorts and filters in memory")
    {
        const ViewSpec spec{.sort = {{1, true}}};
        REQUIRE(waitFor([&]
                        { return model->viewCapabilities().sort; }));
        model->setView(spec);
        REQUIRE(waitFor([&]
                        { return model->view() == spec; }));
        CHECK(cellText(*model, 0, 0) == "b");
    }
    SECTION("the next result replaces the rows")
    {
        ResultSheet second_sheet;
        second_sheet.columns = {{"total"}};
        second_sheet.rows = {{int64_t{3}}};
        const auto second = ResultRows::make(std::move(second_sheet));
        CHECK(book->addResultSheet("Group By", second) == "Group By");
        CHECK(book->sheetNames() == expected);
        REQUIRE(waitFor([&]
                        { return model->state() == ModelState::Error; }));
        auto replaced = book->sheet(name);
        REQUIRE(replaced != model);
        CHECK(sheetText(*replaced) == std::vector<std::vector<std::string>>{{"3"}});
    }
    SECTION("a file's sheet of the same name is kept")
    {
        CHECK(book->addResultSheet(file_sheets.front(), first) == file_sheets.front() + " (2)");
        CHECK(book->sheetNames().size() == file_sheets.size() + 2);
    }
}

TEST_CASE("TableBook::addResultSheet: kept after an SQL query")
{
    auto book = TableBook::open(kDemoData / "normal.parquet");
    book->addResultSheet("Group By", ResultRows::make({.columns = {{"x"}}}));
    std::optional<TableBook::SqlResult> result;
    book->executeSql("SELECT 1 AS one", [&](const TableBook::SqlResult &r)
                     { result = r; });
    REQUIRE(waitFor([&]
                    { return result.has_value(); }));
    REQUIRE(result->ok);
    const auto names = book->sheetNames();
    CHECK(std::find(names.begin(), names.end(), "Group By") != names.end());
    CHECK(std::find(names.begin(), names.end(), result->sheet) != names.end());
}

TEST_CASE("TableBook::addResultSheet: a result is read past the memory checkpoints", "[regression]")
{
    // Pauses a text file's sheets after their first block.
    TableModel::Options options;
    options.block_rows = 2;
    options.memory_checkpoints = {1};
    auto book = TableBook::open(kDemoData / "normal.csv", {}, BackendRegistry::builtin(), options);
    ResultSheet sheet;
    sheet.columns = {{"n"}};
    for (int64_t i = 0; i < 10; ++i)
        sheet.rows.push_back({i});
    const std::string name = book->addResultSheet("Group By", ResultRows::make(std::move(sheet), 2));

    auto result = book->sheet(name);
    REQUIRE(result);
    const auto rows = sheetText(*result);
    REQUIRE(rows.size() == 10);
    CHECK(rows.back() == std::vector<std::string>{"9"});
    CHECK_FALSE(result->memoryCheckpoint().has_value());

    auto file = book->sheet(book->sheetNames().front());
    file->setView({.sort = {{0, true}}});
    CHECK(waitFor([&]
                  { return file->memoryCheckpoint().has_value(); }));
}

TEST_CASE("GroupByAccumulator: sums and means without the noise of binary arithmetic")
{
    GroupByAccumulator accumulator({.keys = {}, .values = {0}, .aggregates = {Aggregate::Sum, Aggregate::Mean}});
    for (const double value : {0.1, 0.2, 918499.45 * 3 - 0.3})
        accumulator.add({value});
    const ResultSheet sheet = accumulator.result(letters);
    REQUIRE(sheet.rows.size() == 1);
    CHECK(rowText(sheet.rows[0]) == std::vector<std::string>{"3", "2755498.35", "918499.45"});
}

TEST_CASE("GroupByAccumulator: whole numbers keep their 16th digit", "[regression]")
{
    GroupByAccumulator accumulator({.keys = {}, .values = {0}, .aggregates = {Aggregate::Sum, Aggregate::Mean}});
    accumulator.add({int64_t{1234567890123456}});
    const ResultSheet sheet = accumulator.result(letters);
    REQUIRE(sheet.rows.size() == 1);
    CHECK(rowText(sheet.rows[0]) == std::vector<std::string>{"1", "1234567890123456", "1234567890123456"});
}

TEST_CASE("GroupByAccumulator: finite means survive an overflowing sum", "[regression]")
{
    const double value = GENERATE(1e308, -1e308);
    GroupByAccumulator accumulator({.values = {0}, .aggregates = {Aggregate::Mean}});
    accumulator.add({value});
    accumulator.add({value});
    accumulator.add({std::monostate{}});
    accumulator.add({std::string("text")});
    const ResultSheet sheet = accumulator.result(letters);
    REQUIRE(sheet.rows.size() == 1);
    CHECK(std::get<double>(sheet.rows[0][1]) == value);
}

TEST_CASE("GroupByAccumulator: means retain compensated cancellation", "[regression]")
{
    GroupByAccumulator accumulator({.values = {0}, .aggregates = {Aggregate::Mean}});
    for (const double value : {1e16, 1.0, -1e16})
        accumulator.add({value});
    const ResultSheet sheet = accumulator.result(letters);
    REQUIRE(sheet.rows.size() == 1);
    CHECK(rowText(sheet.rows[0]) == std::vector<std::string>{"3", "0.333333333333333"});
}

TEST_CASE("ResultRows: blocks shared with the model, other reads copied")
{
    ResultSheet sheet;
    sheet.columns = {{"n"}};
    for (int64_t i = 0; i < 1100; ++i)
        sheet.rows.push_back({i});
    const auto rows = ResultRows::make(std::move(sheet), 512);
    CHECK(rows->rowCount() == 1100);

    // The model's blocks are the stored ones.
    const RowBlockPtr a = rows->read(512, 512);
    CHECK(a == rows->read(512, 512));
    CHECK(a->rows.size() == 512);
    // The last one is shorter, which ends the table.
    CHECK(rows->read(1024, 512)->rows.size() == 76);
    // Other sizes are copies of the rows asked for, never fewer before the end.
    const RowBlockPtr wide = rows->read(0, 1024);
    CHECK(wide->rows.size() == 1024);
    CHECK(wide->rows[1023][0] == CellValue{int64_t{1023}});
    const RowBlockPtr odd = rows->read(500, 30);
    CHECK(odd->first_row == 500);
    CHECK(odd->rows.size() == 30);
    CHECK(odd->rows[29][0] == CellValue{int64_t{529}});
    CHECK(rows->read(1100, 512)->rows.empty());

    // A book of another block size reads them too.
    TableModel::Options options;
    options.block_rows = 100;
    auto book = TableBook::open(kDemoData / "normal.csv", {}, BackendRegistry::builtin(), options);
    auto model = book->sheet(book->addResultSheet("Group By", rows));
    REQUIRE(waitFor([&]
                    { return model->state() == ModelState::Ready; }));
    model->requestRange(1090, 1099);
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 1100);
    CHECK(cellText(*model, 1099, 0) == "1099");
    CHECK(cellText(*model, 513, 0) == "513");
}
