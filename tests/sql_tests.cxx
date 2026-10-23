#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "sql_history.hxx"
#include "sql_text.hxx"
#include "table.hxx"
#include "table_duckdb.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    TableBook::SqlResult runSql(TableBook &book, const std::string &sql)
    {
        auto promise = std::make_shared<std::promise<TableBook::SqlResult>>();
        auto future = promise->get_future();
        book.executeSql(sql, [promise](const TableBook::SqlResult &result)
                        { promise->set_value(result); });
        REQUIRE(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
        return future.get();
    }

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

    std::string fileBytes(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    std::string sqlPath(const std::filesystem::path &path)
    {
        const auto text = path.generic_u8string();
        return duckDbStringLiteral(std::string(text.begin(), text.end()));
    }
} // namespace

TEST_CASE("SQL text helpers")
{
    CHECK(trimSqlStatement("  SELECT 1 ;;\n ") == "SELECT 1");
    CHECK(firstSqlKeyword("-- note\n /* block */ With x AS (SELECT 1) SELECT * FROM x") == "with");
    CHECK(firstSqlKeyword("  ") == "");
    CHECK(isBlankSql(" ; -- c\n /* d */ ;"));
    CHECK_FALSE(isBlankSql("; SELECT 2"));
    CHECK(uniqueSheetName("SQL Result", {"a"}) == "SQL Result");
    CHECK(uniqueSheetName("SQL Result", {"SQL Result", "SQL Result (2)"}) == "SQL Result (3)");
}

TEST_CASE("SQL tokenizer for highlighting")
{
    using K = SqlTokenKind;
    const std::string sql = "SELECT a1, 'it''s' AS \"select\" -- note\nFROM t WHERE x > 1.5e3 /* c */";
    const auto tokens = tokenizeSql(sql);
    auto textOf = [&](K kind)
    {
        std::vector<std::string> texts;
        for (const auto &token : tokens)
            if (token.kind == kind)
                texts.push_back(sql.substr(token.begin, token.end - token.begin));
        return texts;
    };
    CHECK(textOf(K::Keyword) == std::vector<std::string>{"SELECT", "AS", "FROM", "WHERE"});
    CHECK(textOf(K::String) == std::vector<std::string>{"'it''s'"});
    CHECK(textOf(K::Number) == std::vector<std::string>{"1.5e3"});
    CHECK(textOf(K::Comment) == std::vector<std::string>{"-- note", "/* c */"});
    // Whole text covered, in order.
    REQUIRE_FALSE(tokens.empty());
    CHECK(tokens.front().begin == 0);
    CHECK(tokens.back().end == sql.size());
    for (size_t i = 1; i < tokens.size(); ++i)
        CHECK(tokens[i].begin == tokens[i - 1].end);

    // Still being typed: the open string runs to the end.
    CHECK(tokenizeSql("select 'abc") == std::vector<SqlToken>{{0, 6, K::Keyword}, {6, 7, K::Plain}, {7, 11, K::String}});
    // Non-ASCII identifiers are not split into keywords.
    CHECK(tokenizeSql("多言語select") == std::vector<SqlToken>{{0, 15, K::Plain}});
}

TEST_CASE("SQL: a query becomes a sheet and is replaced by the next one")
{
    const char *file = GENERATE("demo.duckdb", "demo.sqlite3");
    INFO(file);
    auto book = TableBook::open(kDemoData / file);
    REQUIRE(book->supportsSql());
    const auto original_sheets = book->sheetNames();

    // A leading comment and a trailing semicolon are fine.
    auto result = runSql(*book, "-- adults\nSELECT Name, Age FROM normal WHERE Age >= 60 ORDER BY Age DESC, ID;");
    INFO(result.message);
    REQUIRE(result.ok);
    CHECK(result.sheet == SQL_RESULT_SHEET);
    auto names = book->sheetNames();
    CHECK(names.size() == original_sheets.size() + 1);
    CHECK(names.back() == SQL_RESULT_SHEET);

    auto first = openSheet(*book, SQL_RESULT_SHEET);
    REQUIRE(first->columnCount() == 2);
    CHECK(first->columnName(0) == "Name");
    CHECK(cellText(*first, 0, 1) == "62");
    REQUIRE(waitFor([&]
                    { return first->rowCount().isExact(); }));
    const size_t first_rows = first->rowCount().value;
    CHECK(first_rows > 0);

    // The same sheet, new rows; the old model is retired.
    result = runSql(*book, "WITH c AS (SELECT City, count(*) AS n FROM normal GROUP BY City) SELECT * FROM c ORDER BY n DESC, City");
    INFO(result.message);
    REQUIRE(result.ok);
    CHECK(result.sheet == SQL_RESULT_SHEET);
    CHECK(book->sheetNames().size() == original_sheets.size() + 1);
    CHECK(first->state() == ModelState::Error);
    auto second = openSheet(*book, SQL_RESULT_SHEET);
    CHECK(second != first);
    CHECK(second->columnName(0) == "City");
    CHECK(second->columnName(1) == "n");

    // A failing query keeps the last result.
    result = runSql(*book, "SELECT no_such_column FROM normal");
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.message.empty());
    CHECK(book->sheet(SQL_RESULT_SHEET) == second);
    CHECK(second->state() == ModelState::Ready);
}

TEST_CASE("SQL: replacing DuckDB results releases their snapshots")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb");
    std::vector<std::shared_ptr<TableModel>> old_models;
    for (int i = 0; i < 3; ++i)
    {
        auto result = runSql(*book, "SELECT * FROM range(1000) AS t(x)");
        INFO(result.message);
        REQUIRE(result.ok);
        auto model = openSheet(*book, result.sheet);
        CHECK(cellText(*model, 0, 0) == "0");
        // Even a model retained by the UI must release its retired source.
        old_models.push_back(std::move(model));
    }

    // Counted into a table first: a sheet's snapshot is created with CREATE
    // TABLE AS, and DuckDB's duckdb_tables() sees a table being created that
    // way only sometimes, so a query counting snapshots would count itself
    // now and then.
    auto counted = runSql(*book,
                          "CREATE TEMP TABLE snapshot_count AS SELECT count(*) AS n FROM duckdb_tables() "
                          "WHERE temporary AND starts_with(table_name, 'tableskim_snapshot_')");
    INFO(counted.message);
    REQUIRE(counted.ok);
    CHECK(counted.sheet.empty());
    // Only the snapshot of the last result, still shown, remains.
    CHECK(old_models[0]->state() == ModelState::Error);
    CHECK(old_models[1]->state() == ModelState::Error);
    CHECK(old_models[2]->state() == ModelState::Ready);

    auto result = runSql(*book, "SELECT n FROM snapshot_count");
    INFO(result.message);
    REQUIRE(result.ok);
    auto model = openSheet(*book, result.sheet);
    CHECK(cellText(*model, 0, 0) == "1");
    for (const auto &old_model : old_models)
        CHECK(old_model->state() == ModelState::Error);
}

TEST_CASE("SQL: statements other than queries")
{
    const char *file = GENERATE("demo.duckdb", "demo.sqlite3");
    INFO(file);
    auto book = TableBook::open(kDemoData / file);
    const auto sheets = book->sheetNames();

    auto result = runSql(*book, "CREATE TEMP TABLE picked AS SELECT ID FROM normal WHERE ID <= 3");
    INFO(result.message);
    REQUIRE(result.ok);
    CHECK(result.sheet.empty());
    CHECK(book->sheetNames() == sheets);

    // Several statements run in order; temporary objects can be used later.
    result = runSql(*book, "INSERT INTO picked VALUES (100); DELETE FROM picked WHERE ID = 1;");
    INFO(result.message);
    REQUIRE(result.ok);
    result = runSql(*book, "SELECT ID FROM picked ORDER BY ID");
    REQUIRE(result.ok);
    auto model = openSheet(*book, result.sheet);
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 3);
    CHECK(cellText(*model, 2, 0) == "100");

    // The file itself is read-only.
    result = runSql(*book, "DELETE FROM normal");
    CHECK_FALSE(result.ok);

    result = runSql(*book, "SELEC 1");
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.message.empty());
}

TEST_CASE("SQL: not available for other formats")
{
    auto book = TableBook::open(kDemoData / "normal.csv");
    CHECK_FALSE(book->supportsSql());
    CHECK_FALSE(runSql(*book, "SELECT 1").ok);
}

TEST_CASE("SQL: external writes cannot replace source files or other files", "[regression]")
{
    const char *file = GENERATE("demo.duckdb", "demo.sqlite3", "normal.parquet", "normal.json");
    INFO(file);
    const auto directory = std::filesystem::temp_directory_path() / "tableskim-test-sql-protection";
    std::filesystem::create_directories(directory);
    const auto source = directory / file;
    std::filesystem::copy_file(kDemoData / file, source, std::filesystem::copy_options::overwrite_existing);
    const auto original = fileBytes(source);
    const auto output = directory / "output.parquet";
    std::ofstream(output, std::ios::binary) << "keep this file";
    const auto untouched = fileBytes(output);
    {
        auto book = TableBook::open(source);
        const bool sqlite = std::string_view(file) == "demo.sqlite3";
        auto reject = [&](const std::string &sql)
        {
            INFO(sql);
            const auto result = runSql(*book, sql);
            CHECK_FALSE(result.ok);
            CHECK_FALSE(result.message.empty());
            CHECK(fileBytes(source) == original);
            CHECK(fileBytes(output) == untouched);
        };
        std::vector<std::filesystem::path> targets{source, output, directory / "." / file};
        if (const auto relative = std::filesystem::relative(source); !relative.empty())
            targets.push_back(relative);
        const auto link = directory / "source-link";
        std::error_code error;
        std::filesystem::create_symlink(source, link, error);
        if (!error)
            targets.push_back(link);
        const auto hard_link = directory / "source-hard-link";
        std::filesystem::create_hard_link(source, hard_link, error);
        if (!error)
            targets.push_back(hard_link);
        for (const auto &target : targets)
        {
            if (sqlite)
            {
                reject("ATTACH DATABASE " + sqlPath(target) + " AS writable");
                reject("VACUUM INTO " + sqlPath(target));
                reject("SELECT 1; ATTACH DATABASE " + sqlPath(target) + " AS writable");
            }
            else
            {
                const auto copy = "COPY (SELECT 99 AS x) TO " + sqlPath(target) + " (FORMAT parquet)";
                reject(copy);
                reject("SELECT 1; " + copy);
                reject("EXPLAIN ANALYZE " + copy);
                reject("PREPARE overwrite_source AS " + copy);
                reject("SELECT * FROM query(" + duckDbStringLiteral(copy) + ")");
            }
        }
        const auto fresh = directory / "new-file";
        CHECK_FALSE(std::filesystem::exists(fresh));
        if (sqlite)
        {
            reject("ATTACH DATABASE " + sqlPath(fresh) + " AS writable; CREATE TABLE writable.data(x)");
            reject("VACUUM INTO " + sqlPath(fresh));
            reject("EXPLAIN ATTACH DATABASE " + sqlPath(fresh) + " AS writable");
            reject("PRAGMA writable_schema = ON");
            reject("PRAGMA temp_store_directory = " + sqlPath(directory));
            reject("SELECT load_extension('fileio')");
        }
        else
        {
            reject("ATTACH " + sqlPath(fresh) + " AS writable");
            reject("EXPORT DATABASE " + sqlPath(fresh));
            reject("CREATE TABLE persistent AS SELECT 1 AS x");
            reject("CREATE SECRET saved_secret (TYPE s3, KEY_ID 'x', SECRET 'x')");
            reject("INSTALL httpfs");
            reject("LOAD httpfs");
            reject("SET lock_configuration = false");
            reject("PRAGMA temp_directory = " + sqlPath(directory));
            reject("SELECT * FROM enable_logging(storage='file', storage_path=" + sqlPath(source) + ")");
            reject("SELECT * FROM query(" + duckDbStringLiteral(
                "SELECT * FROM enable_logging(storage='file', storage_path=" + sqlPath(output) + ")") + ")");
        }
        CHECK_FALSE(std::filesystem::exists(fresh));
        // Rejected statements leave both normal queries and lazy source reads usable.
        const auto result = runSql(*book, "WITH n AS (SELECT 41 AS x) SELECT x + 1 AS answer FROM n");
        INFO(result.message);
        REQUIRE(result.ok);
        CHECK(cellText(*openSheet(*book, result.sheet), 0, 0) == "42");
        const auto name = book->sheetNames().front();
        CHECK(waitForCell(*openSheet(*book, name), 0, 0).state == CellState::Ready);
    }
    CHECK(fileBytes(source) == original);
    CHECK(fileBytes(output) == untouched);
    std::filesystem::remove_all(directory);
}

TEST_CASE("SQL: temporary tables and views remain writable", "[regression]")
{
    const char *file = GENERATE("demo.duckdb", "demo.sqlite3", "normal.parquet", "normal.json");
    INFO(file);
    auto book = TableBook::open(kDemoData / file);
    for (const auto &sql : {
             "CREATE TEMP TABLE scratch (x INTEGER)",
             "INSERT INTO scratch VALUES (1), (2)",
             "UPDATE scratch SET x = x + 10 WHERE x = 1",
             "DELETE FROM scratch WHERE x = 2",
             "CREATE TEMP VIEW picked AS SELECT x FROM scratch"})
    {
        INFO(sql);
        const auto result = runSql(*book, sql);
        INFO(result.message);
        REQUIRE(result.ok);
    }
    const auto result = runSql(*book, "SELECT x FROM picked");
    INFO(result.message);
    REQUIRE(result.ok);
    CHECK(cellText(*openSheet(*book, result.sheet), 0, 0) == "11");
    // A new query retires the old view before its dependencies are dropped.
    REQUIRE(runSql(*book, "SELECT 42 AS x").ok);
    REQUIRE(runSql(*book, "DROP VIEW picked; DROP TABLE scratch").ok);
}

TEST_CASE("SQL: a Parquet file is a view named after the file")
{
    auto book = TableBook::open(kDemoData / "normal.parquet");
    REQUIRE(book->supportsSql());
    auto result = runSql(*book, "SELECT Department, count(*) AS n FROM normal GROUP BY Department ORDER BY n DESC, Department");
    INFO(result.message);
    REQUIRE(result.ok);
    CHECK(book->sheetNames() == std::vector<std::string>{"normal.parquet", SQL_RESULT_SHEET});
    auto model = openSheet(*book, result.sheet);
    CHECK(model->columnName(0) == "Department");
    // The file's own sheet still works.
    auto file_sheet = openSheet(*book, "normal.parquet");
    CHECK(cellText(*file_sheet, 0, 1) == "Sakura Brown");
}

namespace
{
    std::vector<SqlTable> loadSchema(TableBook &book)
    {
        auto promise = std::make_shared<std::promise<std::vector<SqlTable>>>();
        auto future = promise->get_future();
        book.loadSqlSchema([promise](std::vector<SqlTable> schema)
                           { promise->set_value(std::move(schema)); });
        REQUIRE(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
        return future.get();
    }

    const SqlTable *tableNamed(const std::vector<SqlTable> &schema, const std::string &name)
    {
        for (const auto &table : schema)
        {
            if (table.name == name)
                return &table;
        }
        return nullptr;
    }

    // The candidates at the end of `sql` (or at "|", which is removed).
    std::vector<std::string> candidatesAt(std::string sql, const std::vector<SqlTable> &schema)
    {
        size_t cursor = sql.find('|');
        if (cursor == std::string::npos)
            cursor = sql.size();
        else
            sql.erase(cursor, 1);
        return completeSql(sql, cursor, schema).candidates;
    }

    bool contains(const std::vector<std::string> &list, const std::string &item)
    {
        return std::find(list.begin(), list.end(), item) != list.end();
    }

    const std::vector<SqlTable> kSchema = {
        {"orders", {"id", "customer_id", "Order Date", "total"}},
        {"customers", {"id", "name", "city"}},
        {"sales.targets", {"region", "amount"}},
        {"select", {"from"}},
    };
} // namespace

TEST_CASE("SQL completion")
{
    SECTION("columns of the tables read come first, then keywords and tables")
    {
        const auto candidates = candidatesAt("SELECT c| FROM customers", kSchema);
        REQUIRE(candidates.size() >= 3);
        CHECK(candidates[0] == "city");
        CHECK(contains(candidates, "case"));
        CHECK(contains(candidates, "customers"));
        CHECK(contains(candidates, "customer_id")); // another table's column, last
        CHECK(candidates.back() == "customer_id");
    }
    SECTION("the replaced range is the word before the cursor")
    {
        const std::string sql = "SELECT na FROM customers";
        const SqlCompletion completion = completeSql(sql, 9, kSchema);
        CHECK(completion.begin == 7);
        CHECK(completion.end == 9);
        CHECK(completion.candidates.front() == "name");
    }
    SECTION("keywords follow the case of the word")
    {
        CHECK(candidatesAt("SEL", kSchema) == std::vector<std::string>{"SELECT", "\"select\""});
        CHECK(candidatesAt("sel", kSchema).front() == "select");
        CHECK(contains(candidatesAt("sel", kSchema), "\"select\"")); // the table, quoted
    }
    SECTION("tables after FROM and JOIN")
    {
        const auto candidates = candidatesAt("SELECT * FROM o", kSchema);
        REQUIRE_FALSE(candidates.empty());
        CHECK(candidates.front() == "orders");
        CHECK(candidatesAt("SELECT * FROM orders JOIN cu", kSchema).front() == "customers");
        CHECK(candidatesAt("SELECT * FROM sa", kSchema).front() == "sales.targets");
    }
    SECTION("after a table name, an alias or a schema")
    {
        CHECK(candidatesAt("SELECT orders.", kSchema) ==
              std::vector<std::string>{"id", "customer_id", "\"Order Date\"", "total"});
        CHECK(candidatesAt("SELECT c.n| FROM customers AS c", kSchema) == std::vector<std::string>{"name"});
        CHECK(candidatesAt("SELECT o.t| FROM orders o JOIN customers c ON o.customer_id = c.id", kSchema) ==
              std::vector<std::string>{"total"});
        CHECK(candidatesAt("SELECT x.| FROM orders, customers x", kSchema).size() == 3);
        CHECK(candidatesAt("SELECT * FROM sales.", kSchema) == std::vector<std::string>{"targets"});
        CHECK(candidatesAt("SELECT \"select\".", kSchema) == std::vector<std::string>{"\"from\""});
        CHECK(candidatesAt("SELECT nothing.", kSchema).empty());
    }
    SECTION("quoted names")
    {
        CHECK(candidatesAt("SELECT \"Ord", kSchema) == std::vector<std::string>{"\"orders\"", "\"Order Date\""});
        CHECK(candidatesAt("SELECT \"Order D", kSchema) == std::vector<std::string>{"\"Order Date\""});
        const SqlCompletion completion = completeSql("SELECT \"Ord", 11, kSchema);
        CHECK(completion.begin == 7);
    }
    SECTION("nothing in strings, comments and numbers, or without a word")
    {
        CHECK(candidatesAt("SELECT 'c", kSchema).empty());
        CHECK(candidatesAt("SELECT 1 -- c", kSchema).empty());
        CHECK(candidatesAt("SELECT /* c", kSchema).empty());
        CHECK(candidatesAt("SELECT 12", kSchema).empty());
        CHECK(candidatesAt("SELECT ", kSchema).empty());
        // A finished string before the word is fine.
        CHECK(candidatesAt("SELECT 'x', na| FROM customers", kSchema).front() == "name");
    }
    SECTION("only the statement around the cursor")
    {
        // "total" of a table read elsewhere comes after the keyword "to".
        CHECK(candidatesAt("SELECT * FROM orders; SELECT to| FROM customers", kSchema) ==
              std::vector<std::string>{"to", "total"});
        CHECK(candidatesAt("SELECT to| FROM orders; SELECT * FROM customers", kSchema) ==
              std::vector<std::string>{"total", "to"});
    }
}

TEST_CASE("SQL completion helpers")
{
    CHECK(commonCompletionPrefix({"customer_id", "customers", "custom"}) == "custom");
    CHECK(commonCompletionPrefix({"abc"}) == "abc");
    CHECK(commonCompletionPrefix({}).empty());
    // "日本" and "日付" share 日 only, not part of the next character.
    CHECK(commonCompletionPrefix({"日本", "日付"}) == "日");
    CHECK(sqlIdentifier("name") == "name");
    CHECK(sqlIdentifier("Order Date") == "\"Order Date\"");
    CHECK(sqlIdentifier("select") == "\"select\"");
    CHECK(sqlIdentifier("a\"b") == "\"a\"\"b\"");
    CHECK(sqlIdentifier("多言語") == "多言語");
    CHECK(sqlIdentifier("1st") == "\"1st\"");
}

TEST_CASE("SQL schema for completion")
{
    SECTION("DuckDB and SQLite files, with temporary tables")
    {
        const char *file = GENERATE("demo.duckdb", "demo.sqlite3");
        INFO(file);
        auto book = TableBook::open(kDemoData / file);
        auto schema = loadSchema(*book);
        const SqlTable *normal = tableNamed(schema, "normal");
        REQUIRE(normal);
        REQUIRE(normal->columns.size() == 9);
        CHECK(normal->columns[0] == "ID");
        CHECK(normal->columns[1] == "Name");
        CHECK(tableNamed(schema, "多言語サンプル"));

        REQUIRE(runSql(*book, "CREATE TEMP TABLE scratch AS SELECT 1 AS x").ok);
        REQUIRE(runSql(*book, "SELECT * FROM normal").ok); // TableSkim's own view is left out
        schema = loadSchema(*book);
        const SqlTable *scratch = tableNamed(schema, "scratch");
        REQUIRE(scratch);
        CHECK(scratch->columns == std::vector<std::string>{"x"});
        for (const auto &table : schema)
            CHECK(table.name.find("tableskim") == std::string::npos);
    }
    SECTION("a Parquet file is its view")
    {
        auto book = TableBook::open(kDemoData / "normal.parquet");
        const auto schema = loadSchema(*book);
        REQUIRE(schema.size() == 1);
        CHECK(schema[0].name == "normal");
        CHECK(schema[0].columns.size() == 9);
    }
    SECTION("nothing for files without SQL")
    {
        auto book = TableBook::open(kDemoData / "normal.csv");
        CHECK(loadSchema(*book).empty());
    }
}

TEST_CASE("SQL history")
{
    const auto path = std::filesystem::temp_directory_path() / "tableskim-test-history" / "sql_history";
    std::filesystem::remove_all(path.parent_path());

    SECTION("entries are trimmed, moved to the end when repeated, and limited")
    {
        SqlHistory history(path, 3);
        history.add("  SELECT 1;  ");
        history.add("SELECT 2");
        history.add("   ");
        history.add("SELECT 1");
        CHECK(history.entries() == std::vector<std::string>{"SELECT 2", "SELECT 1"});
        history.add("SELECT\n  3 -- a \\ backslash\r\n");
        history.add("SELECT 4");
        CHECK(history.entries() == std::vector<std::string>{"SELECT 1", "SELECT\n  3 -- a \\ backslash", "SELECT 4"});

        // Another window's history sees them, and keeps them when it adds.
        SqlHistory other(path, 3);
        CHECK(other.entries() == history.entries());
        other.add("SELECT 5");
        history.add("SELECT 6");
        CHECK(history.entries() == std::vector<std::string>{"SELECT 4", "SELECT 5", "SELECT 6"});
    }
    SECTION("one entry per line in the file")
    {
        SqlHistory history(path);
        history.add("SELECT 'a\nb'");
        std::ifstream in(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        CHECK(text == "SELECT 'a\\nb'\n");
        CHECK(unescapeSqlHistoryEntry(escapeSqlHistoryEntry("a\\n\r\nb")) == "a\\n\r\nb");
    }
    SECTION("saves at the same time leave a whole file")
    {
        // Each save writes a temporary file of its own (they used to share
        // one), and renames it over the history.
        std::vector<std::jthread> writers;
        for (int w = 0; w < 4; ++w)
            writers.emplace_back([&path, w]
                                 {
                SqlHistory history(path);
                for (int i = 0; i < 50; ++i)
                    history.add("SELECT " + std::to_string(w) + std::string(200, ' ') + std::to_string(i)); });
        writers.clear();
        std::ifstream in(path, std::ios::binary);
        std::string line;
        size_t lines = 0;
        while (std::getline(in, line))
        {
            ++lines;
            INFO(line);
            CHECK(line.starts_with("SELECT "));
            CHECK(line.size() > 200);
            CHECK(line.size() < 220);
        }
        CHECK(lines > 0);
        size_t files = 0;
        for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(path.parent_path()))
            ++files;
        CHECK(files == 1);
    }
    SECTION("in memory only without a file")
    {
        SqlHistory history({});
        history.add("SELECT 1");
        CHECK(history.entries().size() == 1);
    }
    std::filesystem::remove_all(path.parent_path());
}

TEST_CASE("SQL history cursor")
{
    const std::vector<std::string> entries = {"one", "two", "three"};
    SqlHistoryCursor cursor;
    CHECK_FALSE(cursor.next(entries));
    CHECK(cursor.previous(entries, "draft") == "three");
    CHECK(cursor.walking());
    CHECK(cursor.previous(entries, "three") == "two");
    CHECK(cursor.previous(entries, "two") == "one");
    CHECK_FALSE(cursor.previous(entries, "one")); // the oldest
    CHECK(cursor.next(entries) == "two");
    CHECK(cursor.next(entries) == "three");
    CHECK(cursor.next(entries) == "draft"); // back to what was being typed
    CHECK_FALSE(cursor.walking());
    CHECK_FALSE(cursor.next(entries));
    CHECK_FALSE(cursor.previous({}, "x"));
}
