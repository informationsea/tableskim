#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>

#include <sqlite3.h>

#include "table.hxx"
#include "table_sqlite.hxx"
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

    std::filesystem::path createDatabase(const std::string &file, const std::string &sql)
    {
        const auto path = std::filesystem::temp_directory_path() / file;
        std::filesystem::remove(path);
        sqlite3 *db = nullptr;
        REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
        char *error = nullptr;
        const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
        INFO((error ? error : ""));
        sqlite3_free(error);
        sqlite3_close(db);
        REQUIRE(rc == SQLITE_OK);
        return path;
    }

    std::vector<std::string> columnTexts(SheetSource &sheet, size_t column, size_t max_rows = 100)
    {
        std::vector<std::string> texts;
        const auto block = sheet.read(0, max_rows, {}); // keep the block alive while iterating
        for (const auto &row : block->rows)
            texts.push_back(formatCell(row[column]));
        return texts;
    }
} // namespace

TEST_CASE("demo.sqlite3: tables and views become sheets")
{
    auto book = TableBook::open(kDemoData / "demo.sqlite3");
    CHECK(book->sheetNames() ==
          std::vector<std::string>{"normal", "ragged", "large", "dates", "多言語サンプル", "large_summary"});
}

TEST_CASE("demo.sqlite3: typed values")
{
    auto book = TableBook::open(kDemoData / "demo.sqlite3");
    auto model = openSheet(*book, "normal");

    CHECK(model->columnName(0) == "ID");
    CHECK(model->columnName(7) == "Birthday");
    CHECK(model->rowCount() == RowCount{54, RowCount::Kind::Exact});
    CHECK(model->viewCapabilities().sort);

    const CellRef id = waitForCell(*model, 0, 0);
    REQUIRE(id.state == CellState::Ready);
    CHECK(std::holds_alternative<int64_t>(*id.value));
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK(cellText(*model, 0, 6) == "941751.65");
    CHECK(cellText(*model, 0, 7) == "1986-08-02");
    CHECK(cellText(*model, 52, 1) == "");
    const CellRef salary = waitForCell(*model, 52, 6);
    REQUIRE(salary.state == CellState::Ready);
    CHECK(std::holds_alternative<std::monostate>(*salary.value));
    CHECK(waitForCell(*model, 54, 0).state == CellState::OutOfRange);

    auto multilingual = openSheet(*book, "多言語サンプル");
    CHECK(cellText(*multilingual, 0, 5) == "こんにちは");

    auto summary = openSheet(*book, "large_summary");
    CHECK(summary->columnName(1) == "n");
    CHECK(exactRowCount(*summary) == 5);
}

TEST_CASE("demo.sqlite3: paging through a large table")
{
    auto book = TableBook::open(kDemoData / "demo.sqlite3", {}, BackendRegistry::builtin(), {.block_rows = 100});
    auto model = openSheet(*book, "large");

    CHECK(model->rowCount() == RowCount{10000, RowCount::Kind::Exact});
    model->requestRange(9990, 9999);
    CHECK(cellText(*model, 9999, 0) == "10000");
    CHECK(cellText(*model, 5000, 0) == "5001");
    CHECK(cellText(*model, 0, 0) == "1");
}

TEST_CASE("demo.sqlite3: sort and filter are pushed down")
{
    auto book = TableBook::open(kDemoData / "demo.sqlite3");
    auto model = openSheet(*book, "normal");

    SECTION("numeric filter with stable descending sort")
    {
        Filter filter;
        filter.conditions.push_back({.column = 3, .op = FilterOp::Greater, .value = "60"});
        applyView(*model, {.sort = {{.column = 3, .ascending = false}}, .filter = filter});
        CHECK(exactRowCount(*model) == 4);
        CHECK(cellText(*model, 0, 0) == "21");
        CHECK(cellText(*model, 1, 0) == "43");
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

    SECTION("clearing the view restores the table")
    {
        Filter filter;
        filter.combine = Filter::Combine::Or;
        filter.conditions.push_back({.column = 1, .op = FilterOp::IsEmpty});
        filter.conditions.push_back({.column = 0, .op = FilterOp::Equals, .value = "1"});
        applyView(*model, {.filter = filter});
        CHECK(exactRowCount(*model) == 2);
        CHECK(cellText(*model, 1, 0) == "53");
        applyView(*model, {});
        CHECK(exactRowCount(*model) == 54);
        CHECK(cellText(*model, 1, 0) == "2");
    }

    SECTION("sorting a view")
    {
        auto summary = openSheet(*book, "large_summary");
        applyView(*summary, {.sort = {{.column = 0, .ascending = true}}});
        CHECK(exactRowCount(*summary) == 5);
        CHECK(cellText(*summary, 0, 0) == "alpha");
        CHECK(cellText(*summary, 4, 0) == "gamma");
    }
}

TEST_CASE("SQLite: mixed types sort and filter like the in-memory model")
{
    const auto path = createDatabase("tableskim-mixed.sqlite3", R"(
        CREATE TABLE mixed (x, id INTEGER);
        INSERT INTO mixed VALUES ('10', 1), (9, 2), ('apple', 3), ('', 4), (NULL, 5), (' 2 ', 6),
                                 (9007199254740993, 7), (9007199254740992.0, 8), ('1.0', 9), (1, 10),
                                 ('Banana', 11), (x'0102', 12);
    )");
    auto data = openSqliteDataSource(path);
    auto sheet = data->openSheet("mixed");

    sheet->applyView({.sort = {{.column = 0, .ascending = true}}});
    CHECK(columnTexts(*sheet, 1) ==
          std::vector<std::string>{"9", "10", "6", "2", "1", "8", "7", "12", "11", "3", "4", "5"});
    sheet->applyView({.sort = {{.column = 0, .ascending = false}}});
    CHECK(columnTexts(*sheet, 1) ==
          std::vector<std::string>{"3", "11", "12", "7", "8", "1", "2", "6", "9", "10", "4", "5"});

    Filter filter;
    filter.conditions.push_back({.column = 0, .op = FilterOp::Less, .value = "9.5"});
    sheet->applyView({.filter = filter});
    // numbers below 9.5, plus empty values (compared as "" < "9.5")
    CHECK(columnTexts(*sheet, 1) == std::vector<std::string>{"2", "4", "5", "6", "9", "10"});

    filter.conditions[0] = {.column = 0, .op = FilterOp::Contains, .value = "AN"};
    sheet->applyView({.filter = filter});
    CHECK(columnTexts(*sheet, 1) == std::vector<std::string>{"11"});

    // Regular expressions and dates use the in-memory semantics as well.
    filter.conditions[0] = {.column = 0, .op = FilterOp::Matches, .value = "^\\d+$"};
    sheet->applyView({.filter = filter});
    // 9007199254740992.0 is shown as 9007199254740992.
    CHECK(columnTexts(*sheet, 1) == std::vector<std::string>{"1", "2", "7", "8", "10"});

    sheet->applyView({});
    CHECK(columnTexts(*sheet, 0).size() == 12);
    data.reset();
    sheet.reset();
    std::filesystem::remove(path);
}

TEST_CASE("SQLite: WITHOUT ROWID tables, shadowed rowid and views")
{
    const auto path = createDatabase("tableskim-rowid.sqlite3", R"(
        CREATE TABLE keyed (k TEXT PRIMARY KEY, v INTEGER) WITHOUT ROWID;
        INSERT INTO keyed VALUES ('b', 1), ('a', 1), ('c', 0);
        CREATE TABLE shadow (rowid TEXT, v INTEGER);
        INSERT INTO shadow VALUES ('x', 2), ('y', 1), ('z', 2);
        CREATE VIEW dup AS SELECT v, v FROM shadow;
    )");
    auto data = openSqliteDataSource(path);
    CHECK(data->sheetNames() == std::vector<std::string>{"keyed", "shadow", "dup"});

    auto keyed = data->openSheet("keyed");
    CHECK(keyed->rowCount() == RowCount{3, RowCount::Kind::Exact});
    CHECK(columnTexts(*keyed, 0) == std::vector<std::string>{"a", "b", "c"});
    keyed->applyView({.sort = {{.column = 1, .ascending = false}}});
    CHECK(columnTexts(*keyed, 0) == std::vector<std::string>{"a", "b", "c"}); // ties keep table order
    CHECK(keyed->countRowsSlow({}) == 3);

    auto shadow = data->openSheet("shadow");
    CHECK(shadow->columns()[0].name == "rowid");
    shadow->applyView({.sort = {{.column = 1, .ascending = true}}});
    CHECK(columnTexts(*shadow, 0) == std::vector<std::string>{"y", "x", "z"});

    auto dup = data->openSheet("dup");
    CHECK(dup->columns().size() == 2);
    CHECK(dup->countRowsSlow({}) == 3);
    Filter filter;
    filter.conditions.push_back({.column = 1, .op = FilterOp::Equals, .value = "2"});
    dup->applyView({.filter = filter});
    CHECK(dup->countRowsSlow({}) == 2);
    CHECK(columnTexts(*dup, 0) == std::vector<std::string>{"2", "2"});

    keyed.reset();
    shadow.reset();
    dup.reset();
    data.reset();
    std::filesystem::remove(path);
}

TEST_CASE("SQLite: keyset paging matches offset paging")
{
    const auto path = createDatabase("tableskim-gaps.sqlite3", R"(
        CREATE TABLE gaps (v INTEGER);
        WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 1000)
        INSERT INTO gaps (rowid, v) SELECT i * 3, i FROM n;
        DELETE FROM gaps WHERE v % 7 = 0;
    )");
    auto data = openSqliteDataSource(path);
    auto sheet = data->openSheet("gaps");
    REQUIRE(sheet->rowCount() == RowCount{858, RowCount::Kind::Exact});

    std::vector<std::string> sequential;
    for (size_t first = 0; first < 858; first += 100)
    {
        const auto block = sheet->read(first, 100, {});
        for (const auto &row : block->rows)
            sequential.push_back(formatCell(row[0]));
    }
    REQUIRE(sequential.size() == 858);
    // jumping backwards falls back to OFFSET
    const auto jumped = sheet->read(500, 1, {});
    REQUIRE(jumped->rows.size() == 1);
    CHECK(formatCell(jumped->rows[0][0]) == sequential[500]);
    CHECK(sequential[6] == "8");

    sheet.reset();
    data.reset();
    std::filesystem::remove(path);
}

TEST_CASE("SQLite: volatile SQL results and views keep the same rows across pages and views", "[regression]")
{
    const auto path = createDatabase("tableskim-volatile.sqlite3", R"(
        CREATE TABLE data (id INTEGER);
        WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i + 1 FROM n WHERE i < 2000)
        INSERT INTO data SELECT i FROM n;
        CREATE VIEW shuffled AS SELECT id, random() AS value FROM data ORDER BY random();
    )");
    {
        auto data = openSqliteDataSource(path);
        std::string name;
        SECTION("stored view") { name = "shuffled"; }
        SECTION("SQL result")
        {
            name = data->executeSql("SELECT id, random() AS value FROM data ORDER BY random()").sheet;
        }
        auto sheet = data->openSheet(name);
        std::map<int64_t, CellValue> original;
        std::vector<TableRow> rows;
        for (size_t first = 0; first < 2000; first += 512)
        {
            const auto block = sheet->read(first, 512, {});
            for (const auto &row : block->rows)
            {
                original.emplace(std::get<int64_t>(row[0]), row[1]);
                rows.push_back(row);
            }
        }
        REQUIRE(rows.size() == 2000);
        CHECK(original.size() == 2000);
        REQUIRE(sheet->countRowsSlow({}) == 2000);
        CHECK(sheet->read(0, 512, {})->rows == std::vector<TableRow>(rows.begin(), rows.begin() + 512));

        sheet->applyView({.sort = {{0, true}}, .filter = Filter{.conditions = {{0, FilterOp::LessEqual, "100"}}}});
        CHECK(sheet->countRowsSlow({}) == 100);
        const auto filtered = sheet->read(0, 101, {});
        REQUIRE(filtered->rows.size() == 100);
        for (size_t i = 0; i < filtered->rows.size(); ++i)
        {
            const auto id = static_cast<int64_t>(i + 1);
            CHECK(filtered->rows[i][0] == CellValue{id});
            CHECK(filtered->rows[i][1] == original.at(id));
        }
        sheet->applyView({});
        CHECK(sheet->countRowsSlow({}) == 2000);
        CHECK(sheet->read(0, 512, {})->rows == std::vector<TableRow>(rows.begin(), rows.begin() + 512));
    }
    std::filesystem::remove(path);
}

TEST_CASE("SQLite: snapshot columns retain their value types", "[regression]")
{
    const auto path = createDatabase("tableskim-snapshot-types.sqlite3", "CREATE TABLE data (id INTEGER)");
    {
        auto data = openSqliteDataSource(path);
        const auto outcome = data->executeSql("SELECT 1 AS value UNION ALL SELECT 1.0 UNION ALL SELECT '1' UNION ALL SELECT NULL");
        auto sheet = data->openSheet(outcome.sheet);
        const auto block = sheet->read(0, 10, {});
        REQUIRE(block->rows.size() == 4);
        CHECK(std::holds_alternative<int64_t>(block->rows[0][0]));
        CHECK(std::holds_alternative<double>(block->rows[1][0]));
        CHECK(std::holds_alternative<std::string>(block->rows[2][0]));
        CHECK(std::holds_alternative<std::monostate>(block->rows[3][0]));
    }
    std::filesystem::remove(path);
}

TEST_CASE("SQLite: probing and errors")
{
    const auto path = std::filesystem::temp_directory_path() / "tableskim-broken.sqlite3";
    {
        std::ofstream out(path, std::ios::binary);
        out.write("SQLite format 3\0", 16);
        out << std::string(200, 'x');
    }
    CHECK_THROWS_AS(TableBook::open(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST_CASE("SQLite: source names do not collide with internal CTEs")
{
    std::filesystem::path path;
    SECTION("WITHOUT ROWID table named src")
    {
        path = createDatabase("tableskim-src-name.sqlite3", R"(
            CREATE TABLE src (k TEXT PRIMARY KEY) WITHOUT ROWID;
            INSERT INTO src VALUES ('b'), ('a');
        )");
    }
    SECTION("view named src")
    {
        path = createDatabase("tableskim-src-name.sqlite3", R"(
            CREATE TABLE base (k TEXT);
            INSERT INTO base VALUES ('b'), ('a');
            CREATE VIEW src AS SELECT k FROM base;
        )");
    }
    {
        auto data = openSqliteDataSource(path);
        auto sheet = data->openSheet("src");
        CHECK(sheet->columns().size() == 1);
        CHECK(sheet->countRowsSlow({}) == 2);
        CHECK(columnTexts(*sheet, 0).size() == 2);
        sheet->applyView({.sort = {{.column = 0, .ascending = false}}});
        CHECK(columnTexts(*sheet, 0) == std::vector<std::string>{"b", "a"});
        sheet->applyView({});
        CHECK(sheet->countRowsSlow({}) == 2);
    }
    std::filesystem::remove(path);
}

TEST_CASE("SQLite: temp views do not shadow main tables")
{
    const auto path = createDatabase("tableskim-temp-name.sqlite3", R"(
        CREATE TABLE ordinary (n INTEGER);
        INSERT INTO ordinary VALUES (2), (1);
        CREATE TABLE tableskim_view_0 (v TEXT);
        INSERT INTO tableskim_view_0 VALUES ('original-b'), ('original-a');
    )");
    {
        auto data = openSqliteDataSource(path);
        auto ordinary = data->openSheet("ordinary");
        ordinary->applyView({.sort = {{.column = 0, .ascending = true}}});
        CHECK(columnTexts(*ordinary, 0) == std::vector<std::string>{"1", "2"});

        auto sheet = data->openSheet("tableskim_view_0");
        REQUIRE(sheet->columns().size() == 1);
        CHECK(sheet->columns()[0].name == "v");
        CHECK(sheet->rowCount() == RowCount{2, RowCount::Kind::Exact});
        CHECK(columnTexts(*sheet, 0) == std::vector<std::string>{"original-b", "original-a"});
        sheet->applyView({.sort = {{.column = 0, .ascending = true}}});
        CHECK(columnTexts(*sheet, 0) == std::vector<std::string>{"original-a", "original-b"});
        sheet->applyView({});
        CHECK(columnTexts(*sheet, 0) == std::vector<std::string>{"original-b", "original-a"});
    }
    std::filesystem::remove(path);
}

TEST_CASE("SQLite: NaN text sorts as a number and preserves ties")
{
    const auto path = createDatabase("tableskim-nan-sort.sqlite3", R"(
        CREATE TABLE numbers (x, id INTEGER);
        INSERT INTO numbers VALUES ('nan', 1), (2, 2), ('apple', 3), ('inf', 4), ('-inf', 5),
                                   ('NAN', 6), (NULL, 7), ('', 8), ('NaN', 9);
    )");
    {
        auto data = openSqliteDataSource(path);
        auto sheet = data->openSheet("numbers");
        sheet->applyView({.sort = {{.column = 0, .ascending = true}}});
        CHECK(columnTexts(*sheet, 1) == std::vector<std::string>{"5", "2", "4", "1", "6", "9", "3", "7", "8"});
        sheet->applyView({.sort = {{.column = 0, .ascending = false}}});
        CHECK(columnTexts(*sheet, 1) == std::vector<std::string>{"3", "1", "6", "9", "4", "2", "5", "7", "8"});
        sheet->applyView({.sort = {{.column = 0, .ascending = true}, {.column = 1, .ascending = false}}});
        CHECK(columnTexts(*sheet, 1) == std::vector<std::string>{"5", "2", "4", "9", "6", "1", "3", "8", "7"});
        Filter filter;
        filter.conditions.push_back({.column = 0, .op = FilterOp::Equals, .value = "nan"});
        sheet->applyView({.filter = filter});
        CHECK(columnTexts(*sheet, 1) == std::vector<std::string>{"1", "6", "9"});
    }
    std::filesystem::remove(path);
}
