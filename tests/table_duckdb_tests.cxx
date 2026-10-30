#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include <duckdb.hpp>

#include "cell_format.hxx"
#include "sql_text.hxx"
#include "table.hxx"
#include "table_duckdb.hxx"
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

    void applyView(TableModel &model, ViewSpec spec)
    {
        const ViewSpec expected = spec;
        model.setView(std::move(spec));
        REQUIRE(waitFor([&]
                        { return model.view() == expected; }));
    }

    size_t exactRowCount(TableModel &model)
    {
        REQUIRE(waitFor([&]
                        { return model.rowCount().isExact(); }));
        return model.rowCount().value;
    }

    void checkFilterMatches(SheetSource &sheet, const RowBlock &original, size_t column,
                            FilterOp op, const std::string &value)
    {
        INFO("column=" << column << " op=" << static_cast<int>(op) << " value=" << value);
        const FilterCondition condition{column, op, value};
        std::vector<int64_t> expected;
        for (const auto &row : original.rows)
        {
            if (cellMatchesCondition(row[column], condition))
                expected.push_back(std::get<int64_t>(row[0]));
        }
        sheet.applyView({.filter = Filter{.conditions = {condition}}});
        const auto filtered = sheet.read(0, original.rows.size() + 1, {});
        std::vector<int64_t> actual;
        for (const auto &row : filtered->rows)
            actual.push_back(std::get<int64_t>(row[0]));
        CHECK(actual == expected);
    }
    ColumnStats columnStatsOf(TableModel &model, size_t column)
    {
        model.startColumnStats(column);
        REQUIRE(waitFor([&]
                        { return model.columnStatsState().done; }, std::chrono::seconds(30)));
        return model.columnStatsState().stats;
    }

    // Typed values match text that shows (or sorts as) the same thing, as
    // "500000.0" in a CSV file does a DOUBLE 500000.
    bool sameValue(const CellValue &a, const CellValue &b)
    {
        return formatCell(a) == formatCell(b) || compareCells(a, b) == 0;
    }

    void checkSameStats(const ColumnStats &expected, const ColumnStats &actual)
    {
        CHECK(actual.rows == expected.rows);
        CHECK(actual.empty == expected.empty);
        CHECK(actual.distinct == expected.distinct);
        CHECK(actual.distinct_exact == expected.distinct_exact);
        CHECK(actual.numbers == expected.numbers);
        REQUIRE(actual.min.has_value() == expected.min.has_value());
        if (expected.min)
        {
            CHECK(sameValue(*actual.min, *expected.min));
            CHECK(sameValue(*actual.max, *expected.max));
        }
        REQUIRE(actual.mean.has_value() == expected.mean.has_value());
        if (expected.mean)
        {
            CHECK(*actual.sum == Catch::Approx(*expected.sum));
            CHECK(*actual.mean == Catch::Approx(*expected.mean));
        }
        CHECK(actual.stddev.has_value() == expected.stddev.has_value());
        REQUIRE(actual.top.size() == expected.top.size());
        for (size_t i = 0; i < expected.top.size(); ++i)
        {
            INFO("top " << i << ": " << formatCell(expected.top[i].value) << " / " << formatCell(actual.top[i].value));
            CHECK(actual.top[i].count == expected.top[i].count);
            CHECK(sameValue(actual.top[i].value, expected.top[i].value));
        }
    }
} // namespace

TEST_CASE("demo.duckdb: tables and views become sheets")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb");
    CHECK(book->sheetNames() ==
          std::vector<std::string>{"dates", "large", "normal", "ragged", "多言語サンプル", "large_summary"});
}

TEST_CASE("DuckDB: a sheet name shared by two tables opens the first one")
{
    // As specified: tables of other schemas are "schema.name", so table b of
    // schema a and table "a.b" of main are both "a.b"; the main one (listed
    // first) is the sheet, the other is reached with SQL.
    const auto path = std::filesystem::temp_directory_path() / "tableskim-test-shared-name.duckdb";
    std::filesystem::remove(path);
    {
        duckdb::DuckDB db(path.string());
        duckdb::Connection conn(db);
        for (const char *sql : {"CREATE TABLE \"a.b\" AS SELECT 'main' AS x", "CREATE SCHEMA a",
                                "CREATE TABLE a.b AS SELECT 'schema a' AS x"})
            REQUIRE_FALSE(conn.Query(sql)->HasError());
    }
    {
        auto book = TableBook::open(path);
        const auto names = book->sheetNames();
        CHECK(std::count(names.begin(), names.end(), "a.b") == 2);
        auto model = book->sheet("a.b");
        REQUIRE(model);
        CHECK(cellText(*model, 0, 0) == "main");
    }
    std::filesystem::remove(path);
}

TEST_CASE("demo.duckdb: typed values")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb");
    auto model = openSheet(*book, "normal");

    CHECK(model->hasHeader());
    CHECK(model->columnCount() == 9);
    CHECK(model->columnName(0) == "ID");
    CHECK(model->columnName(7) == "Birthday");
    CHECK(model->rowCount() == RowCount{54, RowCount::Kind::Exact});
    CHECK(model->viewCapabilities().sort);
    CHECK(model->viewCapabilities().filter);

    const CellRef id = waitForCell(*model, 0, 0);
    REQUIRE(id.state == CellState::Ready);
    CHECK(std::holds_alternative<int64_t>(*id.value));
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK(cellText(*model, 0, 6) == "941751.65");
    const CellRef birthday = waitForCell(*model, 0, 7);
    REQUIRE(birthday.state == CellState::Ready);
    CHECK(std::holds_alternative<Date>(*birthday.value));
    CHECK(formatCell(*birthday.value) == "1986-08-02");
    CHECK(cellText(*model, 0, 8) == "TRUE");

    // row 53: empty string and NULLs
    CHECK(cellText(*model, 52, 1) == "");
    const CellRef salary = waitForCell(*model, 52, 6);
    REQUIRE(salary.state == CellState::Ready);
    CHECK(std::holds_alternative<std::monostate>(*salary.value));
    CHECK(waitForCell(*model, 54, 0).state == CellState::OutOfRange);
}

TEST_CASE("demo.duckdb: quick filters find the row they were made from")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb");
    auto model = openSheet(*book, "normal");
    // Row 53 has an empty string and NULLs.
    for (const size_t row : {size_t{0}, size_t{52}})
    {
        for (size_t column = 0; column < model->columnCount(); ++column)
        {
            applyView(*model, {});
            const CellRef id = waitForCell(*model, row, 0);
            const CellRef cell = waitForCell(*model, row, column);
            REQUIRE(id.state == CellState::Ready);
            REQUIRE(cell.state == CellState::Ready);
            const std::string id_text = formatCell(*id.value);
            const FilterCondition condition = cellFilterCondition(column, *cell.value, true);
            INFO("row=" << row << " column=" << column << " op=" << static_cast<int>(condition.op)
                        << " value=" << condition.value);
            applyView(*model, {.filter = Filter{.conditions = {condition}}});
            const size_t count = exactRowCount(*model);
            bool found = false;
            for (size_t r = 0; r < count && !found; ++r)
                found = cellText(*model, r, 0) == id_text;
            CHECK(found);
        }
    }
}

TEST_CASE("demo.duckdb: dates, times and intervals")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb");
    auto model = openSheet(*book, "dates");

    CHECK(cellText(*model, 1, 1) == "1900-03-01");
    CHECK(cellText(*model, 1, 2) == "1900-03-01 12:00:00");
    CHECK(cellText(*model, 1, 3) == "12:00:00");  // TIME as text
    CHECK(cellText(*model, 1, 4) == "148:36:00"); // INTERVAL as text
    CHECK(cellText(*model, 1, 5) == "1900/03/01");
}

TEST_CASE("demo.duckdb: views and non-ASCII names")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb");

    auto summary = openSheet(*book, "large_summary");
    CHECK(summary->columnName(1) == "n");
    CHECK(exactRowCount(*summary) == 5);

    auto multilingual = openSheet(*book, "多言語サンプル");
    CHECK(cellText(*multilingual, 0, 1) == "日本語");
    CHECK(cellText(*multilingual, 0, 5) == "こんにちは");
}

TEST_CASE("demo.duckdb: paging through a large table")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb", {}, BackendRegistry::builtin(), {.block_rows = 100});
    auto model = openSheet(*book, "large");

    CHECK(model->rowCount() == RowCount{10000, RowCount::Kind::Exact});
    model->requestRange(9990, 9999);
    CHECK(cellText(*model, 9999, 0) == "10000");
    CHECK(cellText(*model, 5000, 0) == "5001");
    CHECK(cellText(*model, 0, 0) == "1");
}

TEST_CASE("demo.duckdb: sort and filter are pushed down")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb");
    auto model = openSheet(*book, "normal");

    SECTION("numeric filter with stable descending sort")
    {
        Filter filter;
        filter.conditions.push_back({.column = 3, .op = FilterOp::Greater, .value = "60"});
        applyView(*model, {.sort = {{.column = 3, .ascending = false}}, .filter = filter});
        CHECK(exactRowCount(*model) == 4);
        CHECK(cellText(*model, 0, 0) == "21");
        CHECK(cellText(*model, 1, 0) == "43"); // tie on Age 62 keeps table order
        CHECK(cellText(*model, 2, 0) == "30");
        CHECK(cellText(*model, 3, 0) == "32");
        CHECK(waitForCell(*model, 4, 0).state == CellState::OutOfRange);
    }

    SECTION("case-insensitive contains")
    {
        Filter filter;
        filter.conditions.push_back({.column = 4, .op = FilterOp::Contains, .value = "TOKYO"});
        applyView(*model, {.filter = filter});
        CHECK(exactRowCount(*model) == 7);
    }

    SECTION("empty values sort last")
    {
        applyView(*model, {.sort = {{.column = 1, .ascending = true}}});
        CHECK(cellText(*model, 0, 1) == "Alice Brown");
        CHECK(cellText(*model, 53, 0) == "53");
        applyView(*model, {.sort = {{.column = 1, .ascending = false}}});
        CHECK(cellText(*model, 53, 0) == "53");
    }

    SECTION("is empty / boolean equality / OR")
    {
        Filter filter;
        filter.combine = Filter::Combine::Or;
        filter.conditions.push_back({.column = 1, .op = FilterOp::IsEmpty});
        filter.conditions.push_back({.column = 0, .op = FilterOp::Equals, .value = "1"});
        applyView(*model, {.filter = filter});
        CHECK(exactRowCount(*model) == 2);
        CHECK(cellText(*model, 0, 0) == "1");
        CHECK(cellText(*model, 1, 0) == "53");

        Filter active;
        active.conditions.push_back({.column = 8, .op = FilterOp::Equals, .value = "TRUE"});
        active.conditions.push_back({.column = 0, .op = FilterOp::LessEqual, .value = "1"});
        applyView(*model, {.filter = active});
        CHECK(exactRowCount(*model) == 1);
    }

    SECTION("clearing the view restores the table")
    {
        Filter filter;
        filter.conditions.push_back({.column = 0, .op = FilterOp::Less, .value = "3"});
        applyView(*model, {.filter = filter});
        CHECK(exactRowCount(*model) == 2);
        applyView(*model, {});
        CHECK(exactRowCount(*model) == 54);
    }
}

TEST_CASE("demo.duckdb: numeric-aware filter and sort on text columns")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "mixed",
        .from_sql = "(SELECT * FROM (VALUES ('10'), ('9'), ('apple'), (''), (NULL), (' 2 ')) AS v(x))",
    }});
    TableBook book(std::move(data));
    auto model = openSheet(book, "mixed");
    CHECK(exactRowCount(*model) == 6);

    applyView(*model, {.sort = {{.column = 0, .ascending = true}}});
    CHECK(cellText(*model, 0, 0) == " 2 ");
    CHECK(cellText(*model, 1, 0) == "9");
    CHECK(cellText(*model, 2, 0) == "10");
    CHECK(cellText(*model, 3, 0) == "apple");

    Filter filter;
    filter.conditions.push_back({.column = 0, .op = FilterOp::Less, .value = "9.5"});
    applyView(*model, {.filter = filter});
    // " 2 ", "9" numerically; "" and NULL compare as empty text, which is less
    CHECK(exactRowCount(*model) == 4);
}

TEST_CASE("DuckDB: ENUM values sort by displayed text", "[regression]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "enum",
        .from_sql = "(SELECT id, CAST(x AS ENUM('z', 'a', '10', '2', '')) AS x "
                    "FROM (VALUES (0, 'z'), (1, 'a'), (2, '10'), (3, '2'), (4, ''), (5, NULL), (6, 'a')) AS v(id, x))",
    }});
    auto sheet = data->openSheet("enum");
    const auto original = sheet->read(0, 100, {});
    REQUIRE(original->rows.size() == 7);
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
    for (const auto op : {FilterOp::Equals, FilterOp::Less, FilterOp::IsEmpty, FilterOp::IsNotEmpty})
    {
        for (const char *value : {"a", "9", ""})
            checkFilterMatches(*sheet, *original, 1, op, value);
    }
}

TEST_CASE("DuckDB: a column's collation does not change sorts, filters or value counts", "[regression]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "nocase",
        .from_sql = "(SELECT id, x COLLATE NOCASE AS x "
                    "FROM (VALUES (0, 'b'), (1, 'A'), (2, 'a'), (3, 'B'), (4, ''), (5, NULL)) AS v(id, x))",
    }});
    auto sheet = data->openSheet("nocase");
    const auto original = sheet->read(0, 100, {});
    REQUIRE(original->rows.size() == 6);
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
    for (const auto op : {FilterOp::Equals, FilterOp::NotEquals, FilterOp::Less, FilterOp::Contains})
    {
        for (const char *value : {"a", "B"})
            checkFilterMatches(*sheet, *original, 1, op, value);
    }
    sheet->applyView({});
    std::map<std::string, size_t> counts;
    const auto distinct = sheet->columnValueCounts(1, [&](const CellValue &value, size_t count)
                                                   { counts[formatCell(value)] += count; }, {});
    REQUIRE(distinct);
    CHECK(*distinct == 4);
    CHECK(counts == std::map<std::string, size_t>{{"", 2}, {"A", 1}, {"B", 1}, {"a", 1}, {"b", 1}});
}

TEST_CASE("DuckDB: probing and errors")
{
    CHECK_THROWS_AS(openDuckDbRelations({{.sheet_name = "x", .from_sql = "no_such_table"}}), std::runtime_error);

    // A .duckdb extension alone is not enough.
    const auto path = std::filesystem::temp_directory_path() / "tableskim-not-duckdb.duckdb";
    {
        std::ofstream out(path, std::ios::binary);
        out << "a,b\n1,2\n";
    }
    auto book = TableBook::open(path); // falls back to text
    CHECK(book->sheetNames() == std::vector<std::string>{"tableskim-not-duckdb.duckdb"});
    std::filesystem::remove(path);
}

TEST_CASE("DuckDB: unsigned 64-bit integers retain every digit", "[regression]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "unsigned",
        .from_sql = "(SELECT * FROM (VALUES (18446744073709551615::UBIGINT), (0), "
                    "(9223372036854775807), (9223372036854775808), (18446744073709551614), (NULL)) AS v(n))",
    }});
    auto sheet = data->openSheet("unsigned");
    auto block = sheet->read(0, 10, {});
    REQUIRE(block->rows.size() == 6);
    CHECK(formatCell(block->rows[0][0]) == "18446744073709551615");
    CHECK(formatCell(block->rows[1][0]) == "0");
    CHECK(std::holds_alternative<int64_t>(block->rows[1][0]));
    CHECK(formatCell(block->rows[2][0]) == "9223372036854775807");
    CHECK(std::holds_alternative<int64_t>(block->rows[2][0]));
    CHECK(formatCell(block->rows[3][0]) == "9223372036854775808");
    CHECK(formatCell(block->rows[4][0]) == "18446744073709551614");
    CHECK(std::holds_alternative<std::monostate>(block->rows[5][0]));

    const std::vector<std::string> sorted{
        "0", "9223372036854775807", "9223372036854775808", "18446744073709551614", "18446744073709551615", ""};
    sheet->applyView({.sort = {{.column = 0}}});
    block = sheet->read(0, 10, {});
    REQUIRE(block->rows.size() == sorted.size());
    for (size_t i = 0; i < sorted.size(); ++i)
        CHECK(formatCell(block->rows[i][0]) == sorted[i]);

    Filter filter{.conditions = {{.column = 0, .op = FilterOp::Equals, .value = "18446744073709551615"}}};
    sheet->applyView({.filter = filter});
    block = sheet->read(0, 10, {});
    REQUIRE(block->rows.size() == 1);
    CHECK(formatCell(block->rows[0][0]) == "18446744073709551615");
}

TEST_CASE("DuckDB: exact numeric text comparisons and secondary sort keys")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "numbers",
        .from_sql = "(SELECT * FROM (VALUES ('9007199254740992', 1), ('9007199254740993', 2), "
                    "('9007199254740992.0', 3), ('9223372036854775807', 4), "
                    "('9223372036854775808.0', 5)) AS v(x, id))",
    }});
    auto sheet = data->openSheet("numbers");
    Filter filter;
    filter.conditions.push_back({.column = 0, .op = FilterOp::Equals, .value = "9007199254740992"});
    sheet->applyView({.filter = filter});
    auto rows = sheet->read(0, 10, {});
    REQUIRE(rows->rows.size() == 2);
    CHECK(formatCell(rows->rows[0][1]) == "1");
    CHECK(formatCell(rows->rows[1][1]) == "3");

    filter.conditions[0].value = "9223372036854775807";
    sheet->applyView({.filter = filter});
    rows = sheet->read(0, 10, {});
    REQUIRE(rows->rows.size() == 1);
    CHECK(formatCell(rows->rows[0][1]) == "4");

    sheet->applyView({.sort = {{.column = 0, .ascending = true}}});
    rows = sheet->read(0, 10, {});
    REQUIRE(rows->rows.size() == 5);
    CHECK(formatCell(rows->rows[0][1]) == "1");
    CHECK(formatCell(rows->rows[1][1]) == "3");
    CHECK(formatCell(rows->rows[2][1]) == "2");
    CHECK(formatCell(rows->rows[3][1]) == "4");
    CHECK(formatCell(rows->rows[4][1]) == "5");

    auto ties_data = openDuckDbRelations({{
        .sheet_name = "ties",
        .from_sql = "(SELECT * FROM (VALUES ('1.0', 1), ('1', 2), ('01', 3)) AS v(x, id))",
    }});
    auto ties = ties_data->openSheet("ties");
    for (const bool ascending : {true, false})
    {
        ties->applyView({.sort = {{.column = 0, .ascending = ascending}}});
        rows = ties->read(0, 10, {});
        REQUIRE(rows->rows.size() == 3);
        for (size_t i = 0; i < 3; ++i)
            CHECK(formatCell(rows->rows[i][1]) == std::to_string(i + 1));

        ties->applyView({.sort = {{.column = 0, .ascending = ascending}, {.column = 1, .ascending = false}}});
        rows = ties->read(0, 10, {});
        REQUIRE(rows->rows.size() == 3);
        for (size_t i = 0; i < 3; ++i)
            CHECK(formatCell(rows->rows[i][1]) == std::to_string(3 - i));
    }
}

TEST_CASE("DuckDB: numeric string filters use the displayed text", "[regression]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "formatted",
        .from_sql = "(SELECT id, CAST(f AS FLOAT) AS f, CAST(d AS DOUBLE) AS d, CAST(n AS DECIMAL(38,4)) AS n "
                    "FROM (VALUES (0, '1', '1', '1.0000'), (1, '0.1', '0.1', '9007199254740993.0000'), "
                    "(2, '-0', '-0', '-0.0000'), (3, '1e20', '1e20', '12345678901234567890.1234'), "
                    "(4, 'NaN', 'NaN', NULL), (5, 'Infinity', 'Infinity', NULL), "
                    "(6, '-Infinity', '-Infinity', NULL), (7, NULL, NULL, NULL)) AS v(id, f, d, n))",
    }});
    auto sheet = data->openSheet("formatted");
    const auto original = sheet->read(0, 20, {});
    REQUIRE(original->rows.size() == 8);
    CHECK(formatCell(original->rows[0][2]) == "1");
    CHECK(formatCell(original->rows[1][1]) == "0.10000000149011612");
    CHECK(formatCell(original->rows[1][3]) == "9007199254740992");
    CHECK(formatCell(original->rows[2][2]) == "-0");

    auto check_filter = [&](size_t column, FilterOp op, const std::string &value)
    {
        INFO("column=" << column << " op=" << static_cast<int>(op) << " value=" << value);
        const FilterCondition condition{.column = column, .op = op, .value = value};
        std::vector<int64_t> expected;
        for (const auto &row : original->rows)
        {
            if (cellMatchesCondition(row[column], condition))
                expected.push_back(std::get<int64_t>(row[0]));
        }
        sheet->applyView({.filter = Filter{.conditions = {condition}}});
        const auto filtered = sheet->read(0, 20, {});
        std::vector<int64_t> actual;
        for (const auto &row : filtered->rows)
            actual.push_back(std::get<int64_t>(row[0]));
        CHECK(actual == expected);
    };

    for (size_t column = 1; column <= 3; ++column)
    {
        for (const auto op : {FilterOp::Contains, FilterOp::NotContains})
        {
            for (const char *value : {".0", "e+20", "-0", "0.10000000149011612", "NAN", "INF", "9007199254740992", ""})
                check_filter(column, op, value);
        }
        for (const auto op : {FilterOp::Equals, FilterOp::NotEquals, FilterOp::Less, FilterOp::LessEqual,
                              FilterOp::Greater, FilterOp::GreaterEqual})
        {
            for (const char *value : {"1x", ""})
                check_filter(column, op, value);
        }
    }
}

TEST_CASE("DuckDB: temporal filters use the displayed text")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "temporal",
        .from_sql = "(SELECT DATE '2020-01-01' AS d, TIMESTAMPTZ '2020-01-01 12:00:00+00' AS tz, "
                    "TIMESTAMP '2020-01-01 12:00:00.123400' AS ts, "
                    "CAST('2020-01-01 12:00:00.123' AS TIMESTAMP_MS) AS ms, "
                    "CAST('2020-01-01 12:00:00.123456789' AS TIMESTAMP_NS) AS ns "
                    "UNION ALL SELECT NULL, NULL, NULL, NULL, NULL)",
    }});
    auto sheet = data->openSheet("temporal");
    const auto original = sheet->read(0, 10, {});
    REQUIRE(original->rows.size() == 2);
    CHECK(formatCell(original->rows[0][1]) == "2020-01-01 12:00:00");
    CHECK(formatCell(original->rows[0][2]) == "2020-01-01 12:00:00.123400");
    for (size_t column = 0; column < 5; ++column)
    {
        for (const auto op : {FilterOp::Equals, FilterOp::Contains})
        {
            Filter filter;
            filter.conditions.push_back({.column = column, .op = op, .value = formatCell(original->rows[0][column])});
            sheet->applyView({.filter = filter});
            CHECK(sheet->read(0, 10, {})->rows.size() == 1);
        }
        Filter empty;
        empty.conditions.push_back({.column = column, .op = FilterOp::Equals, .value = ""});
        sheet->applyView({.filter = empty});
        CHECK(sheet->read(0, 10, {})->rows.size() == 1);
    }
}

TEST_CASE("DuckDB: relations without row ids keep stable pages")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "tied",
        .from_sql = "(SELECT i % 2 AS key, i AS id FROM range(2000000) v(i))",
    }});
    auto sheet = data->openSheet("tied");
    CHECK(sheet->columns().size() == 2); // the snapshot must not expose its row id
    sheet->applyView({.sort = {{.column = 0, .ascending = true}}});
    for (size_t page = 0; page < 10; ++page)
    {
        const auto rows = sheet->read(page * 100, 100, {});
        REQUIRE(rows->rows.size() == 100);
        for (size_t i = 0; i < rows->rows.size(); ++i)
        {
            CHECK(formatCell(rows->rows[i][0]) == "0");
            CHECK(formatCell(rows->rows[i][1]) == std::to_string(2 * (page * 100 + i)));
        }
    }
    sheet->applyView({.sort = {{.column = 0, .ascending = false}}});
    CHECK(formatCell(sheet->read(0, 1, {})->rows[0][1]) == "1");
    sheet->applyView({});
    CHECK(formatCell(sheet->read(0, 1, {})->rows[0][1]) == "0");
    CHECK(sheet->countRowsSlow({}) == 2000000);

    auto other = data->openSheet("tied");
    CHECK(formatCell(other->read(0, 1, {})->rows[0][1]) == "0");
}

TEST_CASE("DuckDB: numeric text keys preserve bounds and parser semantics")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "bounds",
        .from_sql = "(SELECT * FROM (VALUES ('-9223372036854775808', 1), ('-9007199254740993', 2), "
                    "('-9007199254740992', 3), ('0', 4), ('-0.0', 5), ('\t+1\t', 6), "
                    "('1.0', 7), ('++1', 8), ('0x10', 9), (NULL, 10), ('', 11)) AS v(x, id))",
    }});
    auto sheet = data->openSheet("bounds");
    sheet->applyView({.sort = {{.column = 0, .ascending = true}}});
    auto rows = sheet->read(0, 20, {});
    REQUIRE(rows->rows.size() == 11);
    for (size_t i = 0; i < 9; ++i)
        CHECK(formatCell(rows->rows[i][1]) == std::to_string(i + 1));
    // NULL and empty strings are equal for sorting and preserve source order.
    CHECK(formatCell(rows->rows[9][1]) == "10");
    CHECK(formatCell(rows->rows[10][1]) == "11");

    Filter filter;
    filter.conditions.push_back({.column = 0, .op = FilterOp::Equals, .value = "1"});
    sheet->applyView({.filter = filter});
    rows = sheet->read(0, 20, {});
    REQUIRE(rows->rows.size() == 2);
    CHECK(formatCell(rows->rows[0][1]) == "6");
    CHECK(formatCell(rows->rows[1][1]) == "7");

    filter.conditions[0].op = FilterOp::Less;
    filter.conditions[0].value = "-9007199254740992";
    sheet->applyView({.filter = filter});
    rows = sheet->read(0, 20, {});
    REQUIRE(rows->rows.size() == 5); // two numbers, "++1", NULL and empty text
    CHECK(formatCell(rows->rows[0][1]) == "1");
    CHECK(formatCell(rows->rows[1][1]) == "2");
}

TEST_CASE("DuckDB: spill files go to a private temporary directory")
{
    auto currentTempDirectory = [](DataSource &data)
    {
        auto sheet = data.openSheet("setting");
        const auto rows = sheet->read(0, 1, {});
        REQUIRE(rows->rows.size() == 1);
        const std::string text = formatCell(rows->rows[0][0]);
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    };
    const DuckDbRelation setting{.sheet_name = "setting",
                                 .from_sql = "(SELECT current_setting('temp_directory') AS d)"};

    auto first = openDuckDbRelations({setting});
    auto second = openDuckDbRelations({setting});
    const auto first_dir = currentTempDirectory(*first);
    const auto second_dir = currentTempDirectory(*second);
    CHECK(first_dir.parent_path() == (std::filesystem::temp_directory_path() / "x").parent_path());
    CHECK(first_dir.filename().string().starts_with("tableskim-duckdb-"));
    CHECK(first_dir != second_dir);

    // Whatever is left there is removed together with the database.
    std::filesystem::create_directories(first_dir);
    std::ofstream(first_dir / "leftover.tmp") << "x";
    first.reset();
    CHECK_FALSE(std::filesystem::exists(first_dir));
    CHECK(currentTempDirectory(*second) == second_dir);
}

TEST_CASE("DuckDB: filesystem protection also applies to internal queries", "[regression]")
{
    const auto directory = std::filesystem::temp_directory_path() / "tableskim-test-duckdb-write-protection";
    std::filesystem::create_directories(directory);
    const auto protected_file = directory / "protected.parquet";
    std::ofstream(protected_file, std::ios::binary) << "keep this file";
    const auto pathLiteral = [](const std::filesystem::path &path)
    {
        const auto text = path.generic_u8string();
        return quoteSqlString(std::string(text.begin(), text.end()));
    };
    const auto rejectCopy = [&](const std::string &target, const std::string &options)
    {
        // Bypass executeSql's statement policy to test the independent filesystem
        // boundary, through the trusted SQL used to initialize a relation.
        const DuckDbRelation relation{
            .sheet_name = "attempt",
            .from_sql = "range(1); COPY (SELECT 99 AS x, 100 AS y) TO " + target + " " + options +
                        "; SELECT * FROM range(1)"};
        CHECK_THROWS_WITH(openDuckDbRelations({relation}),
                          Catch::Matchers::ContainsSubstring("SQL cannot write external files"));
    };
    rejectCopy(pathLiteral(protected_file), "(FORMAT parquet)");
    if (const auto relative = std::filesystem::relative(protected_file); !relative.empty())
        rejectCopy(pathLiteral(relative), "(FORMAT parquet)");
    const auto link = directory / "link.parquet";
    std::error_code error;
    std::filesystem::create_symlink(protected_file, link, error);
    if (!error)
        rejectCopy(pathLiteral(link), "(FORMAT parquet)");
    const auto hard_link = directory / "hard-link.parquet";
    std::filesystem::create_hard_link(protected_file, hard_link, error);
    if (!error)
        rejectCopy(pathLiteral(hard_link), "(FORMAT parquet)");
    const auto fresh = directory / "new.parquet";
    rejectCopy(pathLiteral(fresh), "(FORMAT parquet)");
    CHECK_FALSE(std::filesystem::exists(fresh));
    const auto partitions = directory / "partitions";
    rejectCopy(pathLiteral(partitions), "(FORMAT parquet, PARTITION_BY (x))");
    CHECK_FALSE(std::filesystem::exists(partitions));
    rejectCopy("(current_setting('temp_directory') || '/../tableskim-spill-escape.parquet')", "(FORMAT parquet)");
    {
        std::ifstream input(protected_file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        CHECK(text == "keep this file");
    }
    std::filesystem::remove_all(directory);

    // Internal spill writes must still work in the private directory.
    auto data = openDuckDbRelations({
        {.sheet_name = "setup",
         .from_sql = "range(1); COPY (SELECT 42 AS x) TO "
                     "(current_setting('temp_directory') || '/internal.parquet') (FORMAT parquet); "
                     "SELECT * FROM range(1)"},
        {.sheet_name = "internal",
         .from_sql = "read_parquet(current_setting('temp_directory') || '/internal.parquet')"}});
    const auto rows = data->openSheet("internal")->read(0, 1, {});
    REQUIRE(rows->rows.size() == 1);
    CHECK(formatCell(rows->rows[0][0]) == "42");
}


TEST_CASE("DuckDB: typed numeric filters preserve integer and floating point comparisons", "[regression]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "numbers",
        .from_sql = "(SELECT id, CAST(i AS BIGINT) AS i, CAST(u AS UBIGINT) AS u, "
                    "CAST(f AS FLOAT) AS f, CAST(d AS DOUBLE) AS d, CAST(n AS DECIMAL(38,4)) AS n, "
                    "CAST(b AS BOOLEAN) AS b FROM (VALUES "
                    "(0, '9007199254740993', '18446744073709551615', '0.1', '9007199254740992', '9007199254740993', TRUE), "
                    "(1, '9007199254740992', '18446744073709551614', '-0', '-0', '0.1', FALSE), "
                    "(2, '-9007199254740993', '9007199254740993', 'NaN', 'NaN', '-9007199254740993', NULL), "
                    "(3, '9223372036854775807', '0', 'Infinity', 'Infinity', '9223372036854775807', TRUE), "
                    "(4, '-9223372036854775808', '1', '-Infinity', '-Infinity', '-9223372036854775808', FALSE), "
                    "(5, NULL, NULL, NULL, NULL, NULL, NULL), "
                    "(6, '0', '0', '1e18', '1.0000000000000001e18', '1000000000000000128', FALSE)) AS v(id, i, u, f, d, n, b))",
    }});
    auto sheet = data->openSheet("numbers");
    const auto original = sheet->read(0, 10, {});
    REQUIRE(original->rows.size() == 7);
    for (size_t column = 1; column <= 6; ++column)
    {
        for (const auto op : {FilterOp::Equals, FilterOp::NotEquals, FilterOp::Less, FilterOp::LessEqual,
                              FilterOp::Greater, FilterOp::GreaterEqual})
        {
            for (const char *value : {"9007199254740992.0", "9.007199254740992e15", "9007199254740993",
                                      "-9007199254740992.0", "9223372036854775807", "9223372036854775808.0",
                                      "-9223372036854775808", "0.1", "0", "1",
                                      "1000000000000000100", "1000000000000000128", "NaN", "inf", "-inf", ""})
                checkFilterMatches(*sheet, *original, column, op, value);
        }
    }
    // Column 6 is the boolean one.
    for (size_t column = 1; column <= 6; ++column)
    {
        for (const auto op : {FilterOp::IsNumber, FilterOp::IsDate})
            checkFilterMatches(*sheet, *original, column, op, "");
    }
}

TEST_CASE("DuckDB: contains filters use the common ASCII case folding", "[regression]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "text",
        .from_sql = "(SELECT * FROM (VALUES (0, 'ÄBC'), (1, 'äbc'), (2, 'ABC'), (3, '日本語'), "
                    "(4, ''), (5, NULL)) AS v(id, x))",
    }});
    auto sheet = data->openSheet("text");
    const auto original = sheet->read(0, 10, {});
    REQUIRE(original->rows.size() == 6);
    for (const auto op : {FilterOp::Contains, FilterOp::NotContains})
    {
        for (const char *value : {"ä", "Ä", "bc", "BC", "日本", "", "missing"})
            checkFilterMatches(*sheet, *original, 1, op, value);
    }
}

TEST_CASE("DuckDB: temporal numeric filters use epoch days and microseconds", "[regression]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "temporal",
        .from_sql = "(SELECT id, CAST(d AS DATE) AS d, CAST(t AS TIMESTAMP) AS t, "
                    "CAST(t AS TIMESTAMPTZ) AS tz, CAST(t AS TIMESTAMP_MS) AS ms, "
                    "CAST(t AS TIMESTAMP_NS) AS ns FROM (VALUES "
                    "(0, '1970-01-02', '1970-01-01 00:00:00.000001'), "
                    "(1, '1969-12-31', '1969-12-31 23:59:59.999999'), "
                    "(2, '1970-01-01', '1970-01-01 00:00:00'), "
                    "(3, 'infinity', 'infinity'), (4, '-infinity', '-infinity'), "
                    "(5, NULL, NULL)) AS v(id, d, t))",
    }});
    auto sheet = data->openSheet("temporal");
    const auto original = sheet->read(0, 10, {});
    REQUIRE(original->rows.size() == 6);
    for (size_t column = 1; column <= 5; ++column)
    {
        for (const auto op : {FilterOp::Equals, FilterOp::NotEquals, FilterOp::Less, FilterOp::LessEqual,
                              FilterOp::Greater, FilterOp::GreaterEqual})
        {
            for (const char *value : {"1", "1.0", "-1", "0", "1e0", "inf", "-inf", "NaN", ""})
                checkFilterMatches(*sheet, *original, column, op, value);
        }
        for (const auto op : {FilterOp::IsNumber, FilterOp::IsDate})
            checkFilterMatches(*sheet, *original, column, op, "");
    }
}

TEST_CASE("DuckDB: is a number / is a date read text as in memory", "[filter]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "kinds",
        .from_sql = "(SELECT * FROM (VALUES (0, '15 kg'), (1, '12'), (2, ' 3.5 '), (3, 'nan'), (4, 'inf'), "
                    "(5, '1e400'), (6, '2024'), (7, '2024-02-30'), (8, '2024-03-01'), (9, '2024-03-01T10:00:00Z'), "
                    "(10, '+7'), (11, ''), (12, NULL), (13, 'TRUE'), (14, '2024-03-01 10:00')) AS v(id, x))",
    }});
    auto sheet = data->openSheet("kinds");
    const auto original = sheet->read(0, 20, {});
    REQUIRE(original->rows.size() == 15);
    for (const auto op : {FilterOp::IsNumber, FilterOp::IsDate})
        checkFilterMatches(*sheet, *original, 1, op, "");
    CHECK(cellMatchesCondition(CellValue{std::string("15 kg")}, {0, FilterOp::IsNumber, ""}) == false);
    CHECK(cellMatchesCondition(CellValue{std::string(" 3.5 ")}, {0, FilterOp::IsNumber, ""}));
    CHECK(cellMatchesCondition(CellValue{std::string("nan")}, {0, FilterOp::IsNumber, ""}) == false);
    CHECK(cellMatchesCondition(CellValue{true}, {0, FilterOp::IsNumber, ""}) == false);
    CHECK(cellMatchesCondition(CellValue{Date{0}}, {0, FilterOp::IsNumber, ""}) == false);
    CHECK(cellMatchesCondition(CellValue{Date{0}}, {0, FilterOp::IsDate, ""}));
    CHECK(cellMatchesCondition(CellValue{std::string("2024-03-01")}, {0, FilterOp::IsDate, ""}));
    CHECK(cellMatchesCondition(CellValue{std::string("2024-02-30")}, {0, FilterOp::IsDate, ""}) == false);
    // Numeric text is a number, even "2024".
    CHECK(cellMatchesCondition(CellValue{std::string("2024")}, {0, FilterOp::IsDate, ""}) == false);
}

TEST_CASE("DuckDB: regular expression, date and boolean filters match the in-memory model", "[filter]")
{
    auto data = openDuckDbRelations({{
        .sheet_name = "typed",
        .from_sql = "(SELECT id, CAST(d AS DATE) AS d, CAST(t AS TIMESTAMP) AS t, CAST(t AS TIMESTAMPTZ) AS tz, "
                    "CAST(t AS TIMESTAMP_NS) AS ns, x, CAST(b AS BOOLEAN) AS b, n FROM (VALUES "
                    "(0, '2024-01-01', '2024-01-01 00:00:00', '2024-01-01', 'TRUE', 10), "
                    "(1, '2024-01-01', '2024-01-01 12:00:00.5', '2024-01-01 12:00', 'FALSE', -3), "
                    "(2, '2023-12-31', '2023-12-31 23:59:59.999999', '2023-12-31T23:59:59.999999', NULL, 2024), "
                    "(3, '2024-01-02', '2024-01-02 00:00:00', 'Apple pie', 'TRUE', NULL), "
                    "(4, 'infinity', 'infinity', '2024-1-1', 'FALSE', 0), "
                    "(5, NULL, NULL, '', NULL, 7), "
                    "(6, '2024-02-29', '2024-02-29 10:00:00', NULL, 'TRUE', 1)) AS v(id, d, t, x, b, n))",
    }});
    auto sheet = data->openSheet("typed");
    const auto original = sheet->read(0, 10, {});
    REQUIRE(original->rows.size() == 7);
    for (size_t column = 1; column <= 7; ++column)
    {
        for (const auto op : {FilterOp::Equals, FilterOp::NotEquals, FilterOp::Less, FilterOp::LessEqual,
                              FilterOp::Greater, FilterOp::GreaterEqual})
        {
            for (const char *value : {"2024-01-01", "2023-12-31", "2024-01-01 12:00", "2024-01-01T12:00:00.5",
                                      "2024-01-01 00:00:00", "2024-02-30", "true", "FALSE", "yes", "no", "1", "0",
                                      "maybe", ""})
                checkFilterMatches(*sheet, *original, column, op, value);
        }
        for (const auto op : {FilterOp::IsNumber, FilterOp::IsDate})
            checkFilterMatches(*sheet, *original, column, op, "");
        for (const auto op : {FilterOp::Matches, FilterOp::NotMatches})
        {
            for (const char *value : {"^2024", "01-01$", "apple", "(?-i)apple", "^$", "^\\d+$", "12:00:00\\.5",
                                      "true", "", "日"})
                checkFilterMatches(*sheet, *original, column, op, value);
        }
    }
}

TEST_CASE("column statistics computed by DuckDB match a scan of the rows", "[stats]")
{
    // The CSV file is scanned in memory; Parquet and DuckDB count with GROUP BY.
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    auto csv_book = TableBook::open(kDemoData / "normal.csv", options);
    auto parquet_book = TableBook::open(kDemoData / "normal.parquet");
    auto duckdb_book = TableBook::open(kDemoData / "demo.duckdb");
    auto csv = openSheet(*csv_book, "normal.csv");
    REQUIRE(waitFor([&] { return csv->viewCapabilities().filter; }));
    auto parquet = openSheet(*parquet_book, "normal.parquet");
    auto duckdb = openSheet(*duckdb_book, "normal");
    REQUIRE(csv->columnCount() == 9);

    auto compareAll = [&]
    {
        for (size_t column = 0; column < 9; ++column)
        {
            INFO("column " << csv->columnName(column));
            const ColumnStats expected = columnStatsOf(*csv, column);
            {
                INFO("parquet");
                checkSameStats(expected, columnStatsOf(*parquet, column));
            }
            {
                INFO("duckdb");
                checkSameStats(expected, columnStatsOf(*duckdb, column));
            }
        }
    };
    compareAll();

    // Over the filtered rows only.
    const ViewSpec spec{.filter = Filter{.conditions = {{3, FilterOp::GreaterEqual, "40"}}}};
    for (auto *model : {csv.get(), parquet.get(), duckdb.get()})
        applyView(*model, spec);
    compareAll();
    CHECK(columnStatsOf(*parquet, 3).rows < 54);
}
