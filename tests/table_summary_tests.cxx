#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>

#include "cell_format.hxx"
#include "table_summary.hxx"

namespace
{
    const std::filesystem::path demo_dir = TABLESKIM_DEMO_DATA_DIR;

    FileSummary summarize(const std::string &file, SummaryOptions options = {})
    {
        return summarizeFile(demo_dir / file, file, {}, options);
    }

    ColumnSummary column(const SheetSummary &sheet, const std::string &name)
    {
        for (const auto &c : sheet.columns)
        {
            if (c.name == name)
                return c;
        }
        FAIL("no column " << name);
        throw std::logic_error("unreachable");
    }

    // A sequential source with the given rows, for values the demo files lack.
    class RowsSheet : public SheetSource
    {
    private:
        std::vector<ColumnInfo> columns_;
        std::vector<TableRow> rows_;

    public:
        RowsSheet(std::vector<ColumnInfo> columns, std::vector<TableRow> rows)
            : columns_(std::move(columns)), rows_(std::move(rows)) {}

        SourceCapabilities capabilities() const override { return {}; }
        std::vector<ColumnInfo> columns() override { return columns_; }
        RowCount rowCount() override { return {}; }
        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token) override
        {
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            for (size_t r = first_row; r < rows_.size() && r < first_row + max_rows; ++r)
                block->rows.push_back(rows_[r]);
            return block;
        }
    };
} // namespace

TEST_CASE("summarizeFile summarizes a CSV file", "[summary]")
{
    const auto summary = summarize("normal.csv");
    REQUIRE(summary.error.empty());
    CHECK(summary.encoding == TextEncoding::Utf8);
    REQUIRE(summary.sheets.size() == 1);
    const auto &sheet = summary.sheets.front();
    REQUIRE(sheet.error.empty());
    CHECK(sheet.rows == RowCount{54, RowCount::Kind::Exact});
    CHECK(sheet.stats_rows == 54);
    CHECK(sheet.stats_complete);
    REQUIRE(sheet.columns.size() == 9);
    CHECK(sheet.columns.front().name == "ID");

    const auto id = column(sheet, "ID");
    CHECK(id.type.kind == ColumnKind::Number);
    CHECK(id.type.integers);
    REQUIRE(id.stats);
    CHECK(id.stats->empty == 0);
    CHECK(id.stats->distinct == 54);
    CHECK(formatCell(*id.stats->min) == "1");
    CHECK(formatCell(*id.stats->max) == "54");
    CHECK(*id.stats->mean == Catch::Approx(27.5));
    CHECK(id.stats->top.size() == 5);

    const auto city = column(sheet, "City");
    CHECK(city.type.kind == ColumnKind::Text);
    CHECK(city.stats->empty == 1);
    CHECK(city.stats->distinct == 11);
    CHECK_FALSE(city.stats->mean);
    CHECK(column(sheet, "Birthday").type.kind == ColumnKind::Date);
    CHECK(column(sheet, "Active").type.kind == ColumnKind::Boolean);

    REQUIRE(sheet.head.size() == 10);
    CHECK(formatCell(sheet.head[0][0]) == "1");
    CHECK(formatCell(sheet.head[9][0]) == "10");
}

TEST_CASE("summarizeFile counts pushdown sources in the database like a scan", "[summary]")
{
    const auto counted = summarize("normal.parquet");
    // --scan-rows reads the rows instead, here every one of them.
    const auto scanned = summarize("normal.parquet", {.max_rows = std::numeric_limits<size_t>::max()});
    const auto csv = summarize("normal.csv");
    REQUIRE(counted.error.empty());
    REQUIRE(scanned.error.empty());
    const auto &a = counted.sheets.at(0);
    const auto &b = scanned.sheets.at(0);
    CHECK(a.rows == RowCount{54, RowCount::Kind::Exact});
    CHECK(b.rows == a.rows);
    CHECK(a.stats_complete);
    CHECK(b.stats_complete);
    REQUIRE(a.columns.size() == b.columns.size());
    for (size_t c = 0; c < a.columns.size(); ++c)
    {
        INFO(a.columns[c].name);
        CHECK(a.columns[c].name == b.columns[c].name);
        CHECK(a.columns[c].type == b.columns[c].type);
        const auto &x = *a.columns[c].stats;
        const auto &y = *b.columns[c].stats;
        CHECK(x.rows == y.rows);
        CHECK(x.empty == y.empty);
        CHECK(x.distinct == y.distinct);
        CHECK(x.min == y.min);
        CHECK(x.max == y.max);
        CHECK(x.numbers == y.numbers);
        if (x.mean)
            CHECK(*x.mean == Catch::Approx(*y.mean));
        CHECK(x.top == y.top);
        // The same data as the CSV file.
        const auto &z = *csv.sheets.at(0).columns.at(c).stats;
        CHECK(x.empty == z.empty);
        CHECK(x.distinct == z.distinct);
    }
    REQUIRE(a.head.size() == 10);
    CHECK(a.head == b.head);
}

TEST_CASE("summarizeFile limits the statistics to the rows scanned", "[summary]")
{
    const auto summary = summarize("normal.csv", {.max_rows = 20});
    const auto &sheet = summary.sheets.at(0);
    CHECK(sheet.stats_rows == 20);
    CHECK_FALSE(sheet.stats_complete);
    CHECK(sheet.rows.value >= 20);
    CHECK(column(sheet, "ID").stats->rows == 20);
    CHECK(formatCell(*column(sheet, "ID").stats->max) == "20");
    CHECK(summaryMarkdown(summary, {.max_rows = 20}).find("Statistics over the first 20 rows.") !=
          std::string::npos);
}

TEST_CASE("summary scan limits preserve the head and type sample", "[summary][regression]")
{
    for (const size_t max_rows : {size_t{0}, size_t{2}, size_t{54}, size_t{100}})
    {
        INFO(max_rows);
        const auto summary = summarize("normal.csv", {.head_rows = 20, .max_rows = max_rows});
        REQUIRE(summary.error.empty());
        const auto &sheet = summary.sheets.at(0);
        REQUIRE(sheet.error.empty());
        CHECK(sheet.head.size() == 20);
        CHECK(sheet.rows == RowCount{54, RowCount::Kind::Exact});
        CHECK(column(sheet, "ID").type.kind == ColumnKind::Number);
        CHECK(column(sheet, "Birthday").type.kind == ColumnKind::Date);
        const size_t counted = std::min(max_rows, size_t{54});
        CHECK(sheet.stats_rows == counted);
        CHECK(sheet.stats_complete == (counted == 54));
        for (const auto &c : sheet.columns)
        {
            REQUIRE(c.stats);
            CHECK(c.stats->rows == counted);
        }
        const auto id = column(sheet, "ID");
        if (counted)
            CHECK(formatCell(*id.stats->max) == std::to_string(counted));
        else
            CHECK_FALSE(id.stats->max);
    }
}

TEST_CASE("summary scan limits count missing cells in later columns", "[summary][regression]")
{
    RowsSheet source({{"a"}}, {{int64_t{1}}, {int64_t{2}}, {int64_t{3}, std::string("later")}});
    const auto sheet = summarizeSheet(source, "Sheet", {.max_rows = 2});
    REQUIRE(sheet.columns.size() == 2);
    CHECK(sheet.head.size() == 3);
    CHECK(sheet.columns[1].type.kind == ColumnKind::Text);
    REQUIRE(sheet.columns[1].stats);
    CHECK(sheet.columns[1].stats->rows == 2);
    CHECK(sheet.columns[1].stats->empty == 2);
    CHECK(sheet.columns[1].stats->distinct == 0);
}

TEST_CASE("summarizeFile without statistics shows the first rows", "[summary]")
{
    const SummaryOptions options{.stats = false, .head_rows = 3};
    const auto summary = summarize("large.tsv.gz", options);
    REQUIRE(summary.error.empty());
    const auto &sheet = summary.sheets.at(0);
    REQUIRE(sheet.error.empty());
    CHECK(sheet.head.size() == 3);
    CHECK_FALSE(sheet.columns.empty());
    for (const auto &c : sheet.columns)
        CHECK_FALSE(c.stats);
    // Only the rows for the column types were read.
    CHECK(sheet.rows.kind == RowCount::Kind::AtLeast);
    CHECK(sheet.rows.value == 1000);

    const auto markdown = summaryMarkdown(summary, options);
    CHECK(markdown.find("at least 1000 rows") != std::string::npos);
    CHECK(markdown.find("First 3 rows:") != std::string::npos);
    CHECK(markdown.find("Distinct") == std::string::npos);
    const auto json = summaryJson(summary, options);
    CHECK(json.find("\"rows_exact\": false") != std::string::npos);
    CHECK(json.find("\"stats\"") == std::string::npos);
}

TEST_CASE("summarizeFile counts missing cells of ragged rows as empty", "[summary]")
{
    const auto summary = summarize("ragged.csv");
    const auto &sheet = summary.sheets.at(0);
    REQUIRE(sheet.columns.size() == 50); // the widest row
    CHECK(sheet.columns[5].name == "F"); // no header name: the column letter
    for (const auto &c : sheet.columns)
        CHECK(c.stats->rows == sheet.stats_rows);
    CHECK(sheet.columns[49].stats->empty == sheet.stats_rows - 1);
}

TEST_CASE("summarizeFile reads every sheet or the ones named", "[summary]")
{
    const auto all = summarize("multisheet.xlsx");
    REQUIRE(all.error.empty());
    CHECK(all.sheets.size() > 3);

    const auto two = summarize("multisheet.xlsx", {.sheets = {"売上", "Offset"}});
    REQUIRE(two.error.empty());
    REQUIRE(two.sheets.size() == 2);
    CHECK(two.sheets[0].name == "売上");
    CHECK(two.sheets[0].rows == RowCount{12, RowCount::Kind::Exact});
    CHECK(column(two.sheets[0], "Month").type.kind == ColumnKind::Date);
    CHECK(two.sheets[1].name == "Offset");

    const auto missing = summarize("multisheet.xlsx", {.sheets = {"Nope"}});
    CHECK(missing.error == "No such sheet: Nope");
    CHECK(summaryJson(missing, {}).find("\"error\": \"No such sheet: Nope\"") != std::string::npos);

    const auto unreadable = summarize("no-such-file.csv");
    CHECK_FALSE(unreadable.error.empty());
    CHECK(summaryMarkdown(unreadable, {}).find("Error: ") != std::string::npos);
}

TEST_CASE("summaryJson and summaryMarkdown escape what they write", "[summary]")
{
    RowsSheet source({{"a|b"}, {"quote\"d"}},
                     {
                         {CellValue{std::string("x|y\nz")}, CellValue{std::numeric_limits<double>::quiet_NaN()}},
                         {CellValue{std::string("tab\tback\\slash\x01")}, CellValue{1.5}},
                         {CellValue{Date{0}}, CellValue{}},
                         {CellValue{true}, CellValue{int64_t{-3}}},
                     });
    const SummaryOptions options;
    FileSummary summary;
    summary.file = "test";
    summary.sheets.push_back(summarizeSheet(source, "Sheet", options));
    const auto &sheet = summary.sheets.front();
    CHECK(sheet.rows == RowCount{4, RowCount::Kind::Exact});

    const auto json = summaryJson(summary, options);
    CHECK(json.find("\"name\": \"quote\\\"d\"") != std::string::npos);
    CHECK(json.find("[\"x|y\\nz\", \"NaN\"]") != std::string::npos);
    CHECK(json.find("[\"tab\\tback\\\\slash\\u0001\", 1.5]") != std::string::npos);
    CHECK(json.find("[\"1970-01-01\", null]") != std::string::npos);
    CHECK(json.find("[true, -3]") != std::string::npos);
    CHECK(json.find("\"type\": \"mixed\"") != std::string::npos);

    const auto markdown = summaryMarkdown(summary, options);
    CHECK(markdown.find("| a\\|b |") != std::string::npos);
    // Line breaks become spaces, so that a row stays on one line.
    CHECK(markdown.find("| x\\|y z |") != std::string::npos);
    CHECK(markdown.find("4 rows, 2 columns.") != std::string::npos);
}
