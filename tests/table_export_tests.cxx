#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>

#include <duckdb.hpp>
#include <sqlite3.h>

#include "table_export.hxx"
#include "xlsx_writer.hxx"
#include "sql_text.hxx"
#include "table.hxx"
#include "table_duckdb.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    std::filesystem::path tempFile(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / ("tableskim-test-" + name);
    }

    std::string utf8String(const std::filesystem::path &path)
    {
        const std::u8string u8 = path.u8string();
        return std::string(u8.begin(), u8.end());
    }

    // Column name -> DuckDB type name, in file order. A DuckDB export holds
    // one table named `table`.
    std::vector<std::pair<std::string, std::string>> exportedSchema(const std::filesystem::path &path,
                                                                    ExportFormat format = ExportFormat::Parquet,
                                                                    const std::string &table = {})
    {
        duckdb::DuckDB db(nullptr);
        duckdb::Connection conn(db);
        const std::string literal = quoteSqlString(utf8String(path));
        std::unique_ptr<duckdb::MaterializedQueryResult> result;
        if (format == ExportFormat::Parquet)
            result = conn.Query("DESCRIBE SELECT * FROM read_parquet(" + literal + ")");
        else
        {
            REQUIRE_FALSE(conn.Query("ATTACH " + literal + " AS f (READ_ONLY)")->HasError());
            auto tables = conn.Query("SELECT table_name FROM duckdb_tables() WHERE database_name = 'f'");
            REQUIRE(tables->RowCount() == 1);
            CHECK(tables->GetValue(0, 0).ToString() == table);
            result = conn.Query("DESCRIBE f.main.\"" + table + "\"");
        }
        INFO(result->GetError());
        REQUIRE_FALSE(result->HasError());
        std::vector<std::pair<std::string, std::string>> schema;
        for (size_t i = 0; i < result->RowCount(); ++i)
            schema.emplace_back(result->GetValue(0, i).ToString(), result->GetValue(1, i).ToString());
        return schema;
    }

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
} // namespace

TEST_CASE("export paths and table names drop compression and data extensions")
{
    CHECK(defaultExportPath("dir/data.csv", ExportFormat::Parquet) == std::filesystem::path("dir/data.parquet"));
    CHECK(defaultExportPath("dir/data.vcf.gz", ExportFormat::Parquet) == std::filesystem::path("dir/data.parquet"));
    CHECK(defaultExportPath("data", ExportFormat::Parquet) == std::filesystem::path("data.parquet"));
    CHECK(defaultExportPath("dir/data.tsv.zst", ExportFormat::DuckDb) == std::filesystem::path("dir/data.duckdb"));
    CHECK(exportTableName("dir/data.vcf.gz") == "data");
    CHECK(exportFormatFromPath("dir/out.parquet") == ExportFormat::Parquet);
    CHECK(exportFormatFromPath("OUT.DuckDB") == ExportFormat::DuckDb);
    CHECK(exportFormatFromPath("out.ddb") == ExportFormat::DuckDb);
    CHECK_FALSE(exportFormatFromPath("out"));
    CHECK_FALSE(exportFormatFromPath("out.csv"));
    CHECK(exportTableName("dir/my data.csv") == "my data");
}

TEST_CASE("exportTextFile: types are inferred and the result opens")
{
    const ExportFormat format = GENERATE(ExportFormat::Parquet, ExportFormat::DuckDb);
    INFO(exportFormatName(format));
    const auto target = tempFile(std::string("normal-converted") + exportFormatExtension(format));
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    size_t last_progress = 0;
    const auto rows = exportTextFile(kDemoData / "normal.csv.gz", target, format, options, {}, [&](size_t n)
                                     { last_progress = n; });
    REQUIRE(rows);
    CHECK(*rows == 54);
    CHECK(last_progress == 54);
    CHECK_FALSE(std::filesystem::exists(target.string() + ".tableskim-partial"));
    CHECK_FALSE(std::filesystem::exists(target.string() + ".wal"));

    const auto schema = exportedSchema(target, format, "normal");
    REQUIRE(schema.size() == 9);
    const std::map<std::string, std::string> types(schema.begin(), schema.end());
    CHECK(schema[0].first == "ID");
    CHECK(types.at("ID") == "BIGINT");
    CHECK(types.at("Name") == "VARCHAR");
    CHECK(types.at("Salary") == "DOUBLE");
    CHECK(types.at("Birthday") == "DATE");
    CHECK(types.at("Active") == "BOOLEAN");

    {
        auto book = TableBook::open(target);
        auto model = firstSheet(*book);
        CHECK(model->columnName(1) == "Name");
        CHECK(cellText(*model, 0, 1) == "Sakura Brown");
        CHECK(cellText(*model, 1, 5) == "人事部");
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        CHECK(model->rowCount().value == 54);
    }
    std::filesystem::remove(target);
}

TEST_CASE("exportTextFile: integers beyond BIGINT keep their digits")
{
    const auto source = tempFile("big.csv");
    const auto target = tempFile("big.parquet");
    {
        std::ofstream out(source, std::ios::binary);
        out << "big,mixed,small\n"
               "12345678901234567890,1.5,1\n"
               "1,2,-2\n";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    REQUIRE(exportTextFile(source, target, ExportFormat::Parquet, options) == 2);
    const auto schema = exportedSchema(target);
    REQUIRE(schema.size() == 3);
    // As a DOUBLE it would be 12345678901234567168.
    CHECK(schema[0] == std::pair<std::string, std::string>{"big", "VARCHAR"});
    CHECK(schema[1] == std::pair<std::string, std::string>{"mixed", "DOUBLE"});
    CHECK(schema[2] == std::pair<std::string, std::string>{"small", "BIGINT"});
    std::filesystem::remove(source);
    std::filesystem::remove(target);
}

TEST_CASE("exportTextFile: decimal text that would change as a number stays text", "[regression]")
{
    const auto source = tempFile("decimals.csv");
    const auto target = tempFile("decimals.parquet");
    {
        std::ofstream out(source, std::ios::binary);
        out << "price,exact,long\n"
               "1.10,1.5,0.1234567890123456789\n"
               "2.50,2,3.25\n";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    REQUIRE(exportTextFile(source, target, ExportFormat::Parquet, options) == 2);
    const auto schema = exportedSchema(target);
    REQUIRE(schema.size() == 3);
    CHECK(schema[0] == std::pair<std::string, std::string>{"price", "VARCHAR"});
    CHECK(schema[1] == std::pair<std::string, std::string>{"exact", "DOUBLE"});
    CHECK(schema[2] == std::pair<std::string, std::string>{"long", "VARCHAR"});
    std::filesystem::remove(source);
    std::filesystem::remove(target);
}

TEST_CASE("exportTextFile: mixed integer and real columns retain exact integers", "[regression]")
{
    const ExportFormat format = GENERATE(ExportFormat::Parquet, ExportFormat::DuckDb);
    const auto source = tempFile("mixed-precision.csv");
    const auto target = tempFile(std::string("mixed-precision") + exportFormatExtension(format));
    {
        std::ofstream out(source, std::ios::binary);
        out << "unsafe,negative,exact,only_integer,maximum\n"
               "9007199254740993,-9007199254740993,9007199254740992,9007199254740993,9223372036854775807\n"
               "0.5,0.5,0.5,1,0.5\n";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    REQUIRE(exportTextFile(source, target, format, options) == 2);
    const auto schema = exportedSchema(target, format, "tableskim-test-mixed-precision");
    REQUIRE(schema.size() == 5);
    CHECK(schema[0].second == "VARCHAR");
    CHECK(schema[1].second == "VARCHAR");
    CHECK(schema[2].second == "DOUBLE");
    CHECK(schema[3].second == "BIGINT");
    CHECK(schema[4].second == "VARCHAR");
    {
        auto book = TableBook::open(target);
        auto model = firstSheet(*book);
        CHECK(cellText(*model, 0, 0) == "9007199254740993");
        CHECK(cellText(*model, 0, 1) == "-9007199254740993");
        CHECK(cellText(*model, 1, 0) == "0.5");
        CHECK(cellText(*model, 0, 3) == "9007199254740993");
        CHECK(cellText(*model, 0, 4) == "9223372036854775807");
    }
    std::filesystem::remove(source);
    std::filesystem::remove(target);
}

TEST_CASE("exportTextFile: ragged rows, missing names, leading zeros and ## lines")
{
    const auto source = tempFile("convert.vcf");
    const auto target = tempFile("convert.parquet");
    {
        std::ofstream out(source, std::ios::binary);
        out << "##fileformat=VCFv4.2\n"
               "#CHROM\tPOS\t\tPOS\n"
               "1\t100\t007\tx\n"
               "2\t\t1\ty\textra\n"
               "3\t300\t2\n";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    const auto rows = exportTextFile(source, target, ExportFormat::Parquet, options);
    REQUIRE(rows);
    CHECK(*rows == 3);
    const auto schema = exportedSchema(target);
    REQUIRE(schema.size() == 5);
    CHECK(schema[0] == std::pair<std::string, std::string>{"#CHROM", "BIGINT"});
    // An empty cell is NULL in a typed column.
    CHECK(schema[1] == std::pair<std::string, std::string>{"POS", "BIGINT"});
    // "007" must not become 7.
    CHECK(schema[2] == std::pair<std::string, std::string>{"C", "VARCHAR"});
    CHECK(schema[3] == std::pair<std::string, std::string>{"POS_2", "VARCHAR"});
    CHECK(schema[4] == std::pair<std::string, std::string>{"E", "VARCHAR"});

    // Without a header every line is data and columns are lettered.
    options.header = OpenOptions::Header::No;
    options.skip_meta_lines = false;
    REQUIRE(exportTextFile(source, target, ExportFormat::Parquet, options) == 5);
    const auto plain = exportedSchema(target);
    REQUIRE(plain.size() == 5);
    CHECK(plain[0] == std::pair<std::string, std::string>{"A", "VARCHAR"});

    std::filesystem::remove(source);
    std::filesystem::remove(target);
}

TEST_CASE("exportTextFile: invalid UTF-8 past the detected part", "[encoding]")
{
    // Detected as UTF-8 from its first 64 KiB, then a Latin-1 byte, which
    // DuckDB would refuse.
    const auto source = tempFile("latin1.csv");
    {
        std::ofstream out(source, std::ios::binary);
        out << "name\n";
        for (int i = 0; i < 20000; ++i)
            out << "ascii\n";
        out << "caf\xe9\n";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    for (const ExportFormat format : {ExportFormat::Parquet, ExportFormat::DuckDb})
    {
        const auto target = tempFile(format == ExportFormat::Parquet ? "latin1.parquet" : "latin1.duckdb");
        CHECK(exportTextFile(source, target, format, options) == 20001);
        std::filesystem::remove(target);
    }
    std::filesystem::remove(source);
}

TEST_CASE("exportTextFile: reads the source in its encoding", "[encoding]")
{
    const auto target = tempFile("cp932.parquet");
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    options.encoding = TextEncoding::Cp932;
    REQUIRE(exportTextFile(kDemoData / "normal-cp932.csv", target, ExportFormat::Parquet, options) == 54);
    {
        auto book = TableBook::open(target);
        auto model = book->sheet(book->sheetNames().front());
        REQUIRE(model);
        CHECK(cellText(*model, 1, 5) == "人事部");
    }
    std::filesystem::remove(target);
}

TEST_CASE("exportTextFile: stopping leaves no file behind")
{
    const ExportFormat format = GENERATE(ExportFormat::Parquet, ExportFormat::DuckDb);
    INFO(exportFormatName(format));
    const auto target = tempFile(std::string("stopped") + exportFormatExtension(format));
    std::filesystem::remove(target);
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(exportTextFile(kDemoData / "large.tsv.gz", target, format, {}, stop.get_token()));
    CHECK_FALSE(std::filesystem::exists(target));

    // Stopped while reading.
    std::stop_source later;
    CHECK_FALSE(exportTextFile(kDemoData / "large.tsv.gz", target, format, {}, later.get_token(), [&](size_t)
                                 { later.request_stop(); }));
    CHECK_FALSE(std::filesystem::exists(target));
    CHECK_FALSE(std::filesystem::exists(target.string() + ".tableskim-partial"));
}

TEST_CASE("exportTextFile: only text files")
{
    CHECK_THROWS(exportTextFile(kDemoData / "normal.xlsx", tempFile("excel.parquet"), ExportFormat::Parquet, {}));
}

TEST_CASE("exportTextFile: a DuckDB export replaces an existing database")
{
    const auto target = tempFile("replace.duckdb");
    const auto source = tempFile("replace.csv");
    {
        std::ofstream out(source, std::ios::binary);
        out << "x\n1\n";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    REQUIRE(exportTextFile(kDemoData / "normal.csv", target, ExportFormat::DuckDb, options) == 54);
    REQUIRE(exportTextFile(source, target, ExportFormat::DuckDb, options) == 1);
    // Only the new table: the old database is gone, not added to.
    const auto schema = exportedSchema(target, ExportFormat::DuckDb, "tableskim-test-replace");
    REQUIRE(schema.size() == 1);
    CHECK(schema[0] == std::pair<std::string, std::string>{"x", "BIGINT"});
    std::filesystem::remove(source);
    std::filesystem::remove(target);
}

namespace
{
    std::shared_ptr<TableModel> openedSheet(const std::shared_ptr<TableBook> &book, const std::string &sheet = {})
    {
        auto model = sheet.empty() ? firstSheet(*book) : book->sheet(sheet);
        REQUIRE(model);
        REQUIRE(waitFor([&]
                        { return model->state() != ModelState::Opening; }));
        REQUIRE(model->state() == ModelState::Ready);
        return model;
    }

    void applyView(TableModel &model, const ViewSpec &spec)
    {
        model.setView(spec);
        REQUIRE(waitFor([&]
                        { return model.view() == spec; }));
    }

    // Every row of the view as formatCell() text, with the header names
    // first; `columns` empty: all of them.
    std::vector<std::vector<std::string>> viewTexts(TableModel &model, std::vector<size_t> columns = {})
    {
        // Sequential sources count their rows only once all are read.
        for (size_t row = 0; !model.rowCount().isExact(); row += 100)
            waitForCell(model, row, 0);
        if (columns.empty())
        {
            for (size_t c = 0; c < model.columnCount(); ++c)
                columns.push_back(c);
        }
        std::vector<std::vector<std::string>> texts(1);
        for (const size_t c : columns)
            texts[0].push_back(model.columnName(c));
        for (size_t r = 0; r < model.rowCount().value; ++r)
        {
            std::vector<std::string> row;
            for (const size_t c : columns)
            {
                const CellRef ref = waitForCell(model, r, c);
                row.push_back(ref.state == CellState::Ready ? formatCell(*ref.value) : std::string());
            }
            texts.push_back(std::move(row));
        }
        return texts;
    }

    std::vector<std::vector<std::string>> fileTexts(const std::filesystem::path &path)
    {
        OpenOptions options;
        options.header = OpenOptions::Header::Yes;
        auto book = TableBook::open(path, options);
        return viewTexts(*openedSheet(book));
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    ViewSpec osakaByAgeDescending()
    {
        ViewSpec spec;
        spec.sort = {{3, false}};
        spec.filter = Filter{Filter::Combine::Or, {{4, FilterOp::Equals, "Osaka"}, {4, FilterOp::Equals, "Sendai"}}};
        return spec;
    }
} // namespace

TEST_CASE("view export formats and default paths")
{
    for (const ViewExportFormat format : VIEW_EXPORT_FORMATS)
        CHECK(viewExportFormatFromPath(std::string("out") + viewExportFormatExtension(format)) == format);
    CHECK(viewExportFormatFromPath("OUT.TAB") == ViewExportFormat::Tsv);
    CHECK_FALSE(viewExportFormatFromPath("out.xls"));
    CHECK(std::string(viewExportFormatName(ViewExportFormat::Excel)) == "Excel");

    CHECK(defaultViewExportPath("dir/data.csv.gz", "", ViewExportFormat::Csv) ==
          std::filesystem::path("dir/data-export.csv"));
    CHECK(defaultViewExportPath("dir/book.xlsx", "Q1/Q2", ViewExportFormat::Parquet) ==
          std::filesystem::path("dir/book-Q1_Q2-export.parquet"));
}

TEST_CASE("exportView: the sorted, filtered rows without hidden columns")
{
    const ViewExportFormat format = GENERATE(ViewExportFormat::Csv, ViewExportFormat::Tsv, ViewExportFormat::Excel, ViewExportFormat::Parquet);
    INFO(viewExportFormatName(format));
    auto book = TableBook::open(kDemoData / "normal.csv");
    auto model = openedSheet(book);
    applyView(*model, osakaByAgeDescending());
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));

    const auto target = tempFile(std::string("view") + viewExportFormatExtension(format));
    ViewExportOptions options;
    options.hidden.hide(2, 2, model->columnCount()); // Email
    size_t last_progress = 0;
    const auto rows = exportView(model, target, format, options, {}, [&](size_t n)
                                 { last_progress = n; });
    REQUIRE(rows);
    CHECK(*rows == model->rowCount().value);
    CHECK(last_progress == *rows);
    CHECK(*rows > 5);
    CHECK_FALSE(std::filesystem::exists(target.string() + ".tableskim-partial"));

    // Typed formats turn "45" into 45 and "1986-08-02" into a date, which
    // show the same text again.
    CHECK(fileTexts(target) == viewTexts(*model, {0, 1, 3, 4, 5, 6, 7, 8}));
    if (format == ViewExportFormat::Parquet)
    {
        const auto schema = exportedSchema(target);
        const std::map<std::string, std::string> types(schema.begin(), schema.end());
        CHECK(types.at("Age") == "BIGINT");
        CHECK(types.at("Salary") == "DOUBLE");
        CHECK(types.at("Birthday") == "DATE");
        CHECK(types.at("Active") == "BOOLEAN");
        CHECK_FALSE(types.contains("Email"));
    }
    if (format == ViewExportFormat::Excel)
    {
        auto excel = TableBook::open(target);
        auto sheet = openedSheet(excel);
        CHECK(excel->sheetNames() == std::vector<std::string>{"normal.csv"});
        CHECK(std::holds_alternative<int64_t>(*waitForCell(*sheet, 0, 0).value));
        CHECK(std::holds_alternative<double>(*waitForCell(*sheet, 0, 5).value));
        CHECK(std::holds_alternative<Date>(*waitForCell(*sheet, 0, 6).value));
        CHECK(std::holds_alternative<bool>(*waitForCell(*sheet, 0, 7).value));
    }
    std::filesystem::remove(target);
}

TEST_CASE("exportView: CSV and TSV quote only where needed")
{
    const auto source = tempFile("quoting.csv");
    {
        std::ofstream out(source, std::ios::binary);
        out << "name,note\n"
               "plain,\"a, b\"\n"
               "\"say \"\"hi\"\"\",\"two\nlines\"\n"
               "tab,\"x\ty\"\n"
               "empty,\n";
    }
    auto book = TableBook::open(source);
    auto model = openedSheet(book);

    const auto csv = tempFile("quoting-out.csv");
    REQUIRE(exportView(model, csv, ViewExportFormat::Csv) == 4);
    CHECK(readFile(csv) == "name,note\n"
                           "plain,\"a, b\"\n"
                           "\"say \"\"hi\"\"\",\"two\nlines\"\n"
                           "tab,x\ty\n"
                           "empty,\n");
    const auto tsv = tempFile("quoting-out.tsv");
    REQUIRE(exportView(model, tsv, ViewExportFormat::Tsv) == 4);
    CHECK(readFile(tsv) == "name\tnote\n"
                           "plain\ta, b\n"
                           "\"say \"\"hi\"\"\"\t\"two\nlines\"\n"
                           "tab\t\"x\ty\"\n"
                           "empty\t\n");
    CHECK(fileTexts(tsv) == viewTexts(*model));
    // The source stays open while the book lives, and Windows cannot remove it.
    model.reset();
    book.reset();
    for (const auto &path : {source, csv, tsv})
        std::filesystem::remove(path);
}

TEST_CASE("exportView: an empty value in a single column is quoted", "[regression]")
{
    const auto source = tempFile("single.csv");
    {
        std::ofstream out(source, std::ios::binary);
        out << "only\na\n\"\"\nb\n";
    }
    auto book = TableBook::open(source);
    auto model = openedSheet(book);
    const auto csv = tempFile("single-out.csv");
    REQUIRE(exportView(model, csv, ViewExportFormat::Csv) == 3);
    // A bare empty line would be skipped by readers.
    CHECK(readFile(csv) == "only\na\n\"\"\nb\n");
    model.reset();
    book.reset();
    for (const auto &path : {source, csv})
        std::filesystem::remove(path);
}

TEST_CASE("exportView: typed sources keep their types")
{
    auto book = TableBook::open(kDemoData / "dates.parquet");
    auto model = openedSheet(book);
    const auto expected = viewTexts(*model);
    REQUIRE(expected.size() > 10);

    const auto parquet = tempFile("dates-out.parquet");
    REQUIRE(exportView(model, parquet, ViewExportFormat::Parquet));
    CHECK(fileTexts(parquet) == expected);
    {
        duckdb::DuckDB db(nullptr);
        duckdb::Connection conn(db);
        const std::string literal = quoteSqlString(utf8String(parquet));
        auto original = conn.Query("SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM read_parquet(" +
                                   quoteSqlString(utf8String(kDemoData / "dates.parquet")) + "))");
        auto exported = conn.Query("SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM read_parquet(" +
                                   literal + "))");
        REQUIRE_FALSE(original->HasError());
        REQUIRE_FALSE(exported->HasError());
        INFO(original->ToString());
        INFO(exported->ToString());
        // DATE and TIMESTAMP columns stay so; others become what TableSkim
        // reads them as (e.g. TIME as text).
        for (size_t i = 0; i < original->RowCount(); ++i)
        {
            const std::string type = original->GetValue(1, i).ToString();
            if (type == "DATE" || type == "TIMESTAMP" || type == "BIGINT")
                CHECK(exported->GetValue(1, i).ToString() == type);
        }
    }

    const auto excel = tempFile("dates-out.xlsx");
    REQUIRE(exportView(model, excel, ViewExportFormat::Excel));
    auto excel_book = TableBook::open(excel);
    auto excel_sheet = openedSheet(excel_book);
    const auto texts = viewTexts(*excel_sheet);
    REQUIRE(texts.size() == expected.size());
    CHECK(texts[0] == expected[0]);
    CHECK(texts == expected);
    // 1900-01-01, before Excel's 1900-02-29, is a date as well.
    REQUIRE(expected[1][1] == "1900-01-01");
    CHECK(std::holds_alternative<Date>(*waitForCell(*excel_sheet, 0, 1).value));
    std::filesystem::remove(parquet);
    std::filesystem::remove(excel);
}

TEST_CASE("exportView: an SQL result")
{
    auto book = TableBook::open(kDemoData / "demo.duckdb");
    // The callback runs on the worker thread.
    auto promise = std::make_shared<std::promise<TableBook::SqlResult>>();
    auto future = promise->get_future();
    book->executeSql("SELECT 1 AS n, 'a,b' AS s UNION ALL SELECT 2, NULL ORDER BY n", [promise](const auto &r)
                     { promise->set_value(r); });
    REQUIRE(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    const auto result = future.get();
    REQUIRE(result.ok);
    auto model = openedSheet(book, result.sheet);
    const auto target = tempFile("sql-out.csv");
    REQUIRE(exportView(model, target, ViewExportFormat::Csv) == 2);
    CHECK(readFile(target) == "n,s\n1,\"a,b\"\n2,\n");
    std::filesystem::remove(target);
}

TEST_CASE("exportView: mixed typed integers and reals retain exact integers", "[regression]")
{
    const ViewExportFormat format = GENERATE(ViewExportFormat::Excel, ViewExportFormat::Parquet);
    const auto source = tempFile("typed-mixed.sqlite3");
    // SQLite expressions can return INTEGER and REAL in the same column.
    std::filesystem::remove(source);
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(utf8String(source).c_str(), &db) == SQLITE_OK);
    const int result = sqlite3_exec(db,
        "CREATE TABLE data (unsafe, negative, exact); "
        "INSERT INTO data VALUES (9007199254740993, -9007199254740993, 9007199254740992), (0.5, 0.5, 0.5)",
        nullptr, nullptr, nullptr);
    sqlite3_close(db);
    REQUIRE(result == SQLITE_OK);
    const auto target = tempFile(std::string("typed-mixed") + viewExportFormatExtension(format));
    {
        auto book = TableBook::open(source);
        auto model = openedSheet(book);
        REQUIRE(exportView(model, target, format) == 2);
        CHECK(fileTexts(target) == viewTexts(*model));
        if (format == ViewExportFormat::Parquet)
        {
            const auto schema = exportedSchema(target);
            REQUIRE(schema.size() == 3);
            CHECK(schema[0].second == "VARCHAR");
            CHECK(schema[1].second == "VARCHAR");
            CHECK(schema[2].second == "DOUBLE");
        }
    }
    std::filesystem::remove(source);
    std::filesystem::remove(target);
}

TEST_CASE("exportView: overlapping exports own their partial files", "[regression]")
{
    const auto directory = tempFile("overlapping-exports");
    std::filesystem::create_directories(directory);
    const auto a_path = directory / "a.csv";
    const auto b_path = directory / "b.csv";
    const auto target = directory / "out.csv";
    {
        std::ofstream a(a_path), b(b_path);
        a << "tag\nA\nA\n";
        b << "tag\nB\nB\n";
    }
    {
        auto a_book = TableBook::open(a_path);
        auto b_book = TableBook::open(b_path);
        auto a = openedSheet(a_book), b = openedSheet(b_book);
        bool overlapped = false;
        REQUIRE(exportView(a, target, ViewExportFormat::Csv, {}, {}, [&](size_t)
                           {
                               if (!overlapped)
                               {
                                   overlapped = true;
                                   REQUIRE(exportView(b, target, ViewExportFormat::Csv) == 2);
                                   CHECK(readFile(target) == "tag\nB\nB\n");
                               }
                           }) == 2);
        CHECK(overlapped);
        CHECK(readFile(target) == "tag\nA\nA\n");
    }
    std::filesystem::remove(a_path);
    std::filesystem::remove(b_path);
    std::filesystem::remove(target);
    CHECK(std::filesystem::is_empty(directory));
    std::filesystem::remove(directory);
}

TEST_CASE("exportTextFile: partial files are private and cleaned after success and cancellation", "[regression]")
{
    const ExportFormat format = GENERATE(ExportFormat::Parquet, ExportFormat::DuckDb);
    const auto directory = tempFile("private-export");
    std::filesystem::create_directories(directory);
    const auto source = directory / "input.csv";
    const auto target = directory / (std::string("output") + exportFormatExtension(format));
    auto other_partial = target;
    other_partial += ".tableskim-partial";
    {
        std::ofstream out(source), other(other_partial);
        out << "n\n1\n2\n";
        other << "another export";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    REQUIRE(exportTextFile(source, target, format, options) == 2);
    CHECK(readFile(other_partial) == "another export");
    const auto complete = readFile(target);
    std::stop_source stop;
    CHECK_FALSE(exportTextFile(source, target, format, options, stop.get_token(), [&](size_t)
                               { stop.request_stop(); }));
    CHECK(readFile(target) == complete);
    CHECK(readFile(other_partial) == "another export");
    std::filesystem::remove(source);
    std::filesystem::remove(target);
    std::filesystem::remove(other_partial);
    CHECK(std::filesystem::is_empty(directory));
    std::filesystem::remove(directory);
}

TEST_CASE("exportView: Excel text is escaped and the sheet name made valid")
{
    const auto source = tempFile("escape[1].csv");
    {
        std::ofstream out(source, std::ios::binary);
        out << "text,number\n"
               "<a & b>,007\n"
               "ctrl\x01char,1e3\n"
               "_x0041_,nan\n";
    }
    auto book = TableBook::open(source);
    auto model = openedSheet(book);
    const auto target = tempFile("escape.xlsx");
    REQUIRE(exportView(model, target, ViewExportFormat::Excel) == 3);
    auto excel = TableBook::open(target);
    CHECK(excel->sheetNames() == std::vector<std::string>{"tableskim-test-escape_1_.csv"});
    auto sheet = openedSheet(excel);
    CHECK(cellText(*sheet, 0, 0) == "<a & b>");
    CHECK(cellText(*sheet, 2, 0) == "_x0041_");
    // "007" keeps the column as text.
    CHECK(cellText(*sheet, 0, 1) == "007");
    CHECK(cellText(*sheet, 1, 1) == "1e3");
    // Windows cannot remove files that are still open.
    sheet.reset();
    excel.reset();
    model.reset();
    book.reset();
    std::filesystem::remove(source);
    std::filesystem::remove(target);
}

TEST_CASE("xlsxSheetName")
{
    CHECK(xlsxSheetName("a/b:c") == "a_b_c");
    CHECK(xlsxSheetName("'quoted'") == "quoted");
    CHECK(xlsxSheetName("") == "Sheet1");
    CHECK(xlsxSheetName(std::string(40, 'x')) == std::string(31, 'x'));
    CHECK(xlsxSheetName("日本語のシート") == "日本語のシート");
}

TEST_CASE("XlsxWriter: numbers read back unchanged")
{
    // Shortest notation writes 1e+06, which OpenXLSX used to read as 1.
    const std::vector<CellValue> numbers{1000000.0, 2e20, -5e-7, 1500000.0, CellValue{int64_t{-42}},
                                         CellValue{std::numeric_limits<int64_t>::max()},
                                         CellValue{XlsxWriter::MAX_EXACT_INTEGER}};
    const auto target = tempFile("numbers.xlsx");
    {
        XlsxWriter writer(target, "numbers", false);
        writer.addRow(numbers);
        writer.finish();
    }
    auto book = TableBook::open(target);
    auto sheet = openedSheet(book);
    // Excel does not tell integers from doubles: 1500000.0 comes back as an integer.
    CHECK(formatCell(*waitForCell(*sheet, 0, 0).value) == formatCell(CellValue{1000000.0}));
    CHECK(formatCell(*waitForCell(*sheet, 0, 1).value) == formatCell(CellValue{2e20}));
    CHECK(formatCell(*waitForCell(*sheet, 0, 2).value) == formatCell(CellValue{-5e-7}));
    CHECK(formatCell(*waitForCell(*sheet, 0, 3).value) == "1500000");
    CHECK(formatCell(*waitForCell(*sheet, 0, 4).value) == "-42");
    CHECK(formatCell(*waitForCell(*sheet, 0, 5).value) == "9223372036854775807");
    // Beyond Excel's 15 digits an integer is text, which keeps every digit.
    CHECK(std::holds_alternative<std::string>(*waitForCell(*sheet, 0, 5).value));
    CHECK(std::holds_alternative<int64_t>(*waitForCell(*sheet, 0, 6).value));
    CHECK(formatCell(*waitForCell(*sheet, 0, 6).value) == "999999999999999");
    std::filesystem::remove(target);
}

TEST_CASE("XlsxWriter: line breaks read back unchanged", "[regression]")
{
    const std::vector<CellValue> texts{std::string("a\rb"), std::string("c\r\nd"), std::string("e\nf\tg")};
    const auto target = tempFile("breaks.xlsx");
    {
        XlsxWriter writer(target, "breaks", false);
        writer.addRow(texts);
        writer.finish();
    }
    OpenOptions options;
    options.header = OpenOptions::Header::No; // a row of text would look like one
    auto book = TableBook::open(target, options);
    auto sheet = openedSheet(book);
    // A raw CR came back as LF.
    for (size_t c = 0; c < texts.size(); ++c)
    {
        const auto cell = waitForCell(*sheet, 0, c);
        REQUIRE(cell.value);
        CHECK(*cell.value == texts[c]);
    }
    std::filesystem::remove(target);
}

TEST_CASE("XlsxWriter: at most 1,048,576 rows and 16,384 columns")
{
    const auto target = tempFile("rows.xlsx");
    {
        XlsxWriter writer(target, "rows", false);
        const std::vector<CellValue> empty;
        for (size_t i = 0; i < XlsxWriter::MAX_ROWS; ++i)
            writer.addRow(empty);
        CHECK_THROWS(writer.addRow(empty));
    }
    {
        XlsxWriter writer(target, "columns", false);
        const std::vector<CellValue> wide(XlsxWriter::MAX_COLUMNS + 1, CellValue{int64_t{1}});
        CHECK_THROWS(writer.addRow(wide));
    }
    std::filesystem::remove(target);
}

TEST_CASE("exportView: stopping, the viewed file and a changing view")
{
    // Small reads, so that the view changes long before the last row is read.
    TableModel::Options model_options;
    model_options.block_rows = 100;
    model_options.scan_read_rows = 100;
    auto book = TableBook::open(kDemoData / "large.parquet", {}, BackendRegistry::builtin(), model_options);
    auto model = openedSheet(book);
    const auto target = tempFile("large-out.csv");
    std::filesystem::remove(target);

    std::stop_source stop;
    CHECK_FALSE(exportView(model, target, ViewExportFormat::Csv, {}, stop.get_token(), [&](size_t)
                           { stop.request_stop(); }));
    CHECK_FALSE(std::filesystem::exists(target));
    CHECK_FALSE(std::filesystem::exists(target.string() + ".tableskim-partial"));

    ViewExportOptions options;
    options.source = kDemoData / "large.parquet";
    CHECK_THROWS_WITH(exportView(model, kDemoData / "large.parquet", ViewExportFormat::Parquet, options),
                      "The file being viewed cannot be overwritten");

    // The view changes after the first rows were read.
    bool changed = false;
    CHECK_THROWS_WITH(exportView(model, target, ViewExportFormat::Csv, {}, {}, [&](size_t)
                                 {
                                     if (!changed)
                                         model->setView({{{0, false}}, std::nullopt});
                                     changed = true; }),
                      "The view changed while its rows were being read");
    CHECK_FALSE(std::filesystem::exists(target));

    // Exports one after another, and of the new view.
    REQUIRE(waitFor([&]
                    { return !model->view().sort.empty(); }));
    REQUIRE(exportView(model, target, ViewExportFormat::Csv));
    std::ifstream in(target);
    std::string header, first;
    std::getline(in, header);
    std::getline(in, first);
    CHECK(first.substr(0, first.find(',')) == cellText(*model, 0, 0));
    in.close();
    std::filesystem::remove(target);
}

// Hidden (run with "[.super-large]"): one million rows, in every format.
TEST_CASE("exportView: super-large.parquet", "[.super-large]")
{
    const ViewExportFormat format = GENERATE(ViewExportFormat::Csv, ViewExportFormat::Tsv, ViewExportFormat::Excel,
                                             ViewExportFormat::Parquet);
    INFO(viewExportFormatName(format));
    auto book = TableBook::open(kDemoData / "super-large.parquet");
    auto model = openedSheet(book);
    const auto target = tempFile(std::string("super-large-out") + viewExportFormatExtension(format));
    const auto start = std::chrono::steady_clock::now();
    const auto rows = exportView(model, target, format);
    REQUIRE(std::filesystem::exists(target));
    WARN(viewExportFormatName(format) << ": "
                                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count()
                                      << " ms, " << std::filesystem::file_size(target) << " bytes");
    REQUIRE(rows);
    CHECK(*rows == 1000000);
    std::filesystem::remove(target);
}
