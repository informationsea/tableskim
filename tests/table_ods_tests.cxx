#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

#include "file_util.hxx"
#include "table.hxx"
#include "test_helpers.hxx"
#include "test_zip.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;
    constexpr const char *kOdsMimeType = "application/vnd.oasis.opendocument.spreadsheet";

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

    // content.xml around the tables given, with the usual prefixes.
    std::string odsContent(const std::string &tables)
    {
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
 xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0"
 xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"
 xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"
 xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:xlink="http://www.w3.org/1999/xlink"
 xmlns:calcext="urn:org:documentfoundation:names:experimental:calc:xmlns:calcext:1.0"
 office:version="1.3"><office:body><office:spreadsheet>)" +
               tables + "</office:spreadsheet></office:body></office:document-content>";
    }

    std::filesystem::path writeOds(const std::string &file_name, const std::string &content,
                                   const std::string &mimetype = kOdsMimeType, std::vector<TestZipEntry> extra = {})
    {
        const auto path = std::filesystem::temp_directory_path() / file_name;
        std::vector<TestZipEntry> entries;
        if (!mimetype.empty())
            entries.push_back({"mimetype", mimetype});
        entries.push_back({"content.xml", content});
        for (auto &entry : extra)
            entries.push_back(std::move(entry));
        writeTestZip(path, entries);
        return path;
    }

    std::string cell(const std::string &attributes, const std::string &text = "")
    {
        return "<table:table-cell " + attributes + ">" + (text.empty() ? "" : "<text:p>" + text + "</text:p>") +
               "</table:table-cell>";
    }

    // A row of a label and a cell.
    std::string labelled(const std::string &label, const std::string &value_cell)
    {
        return "<table:table-row>" + cell(R"(office:value-type="string")", label) + value_cell + "</table:table-row>";
    }

    bool isEmpty(TableModel &model, size_t row, size_t col)
    {
        const CellRef ref = waitForCell(model, row, col);
        return ref.state == CellState::Empty ||
               (ref.state == CellState::Ready && std::holds_alternative<std::monostate>(*ref.value));
    }

    // Empty cells alike, whether missing, NULL or empty text.
    std::string shown(TableModel &model, size_t row, size_t col)
    {
        const CellRef ref = waitForCell(model, row, col);
        return ref.state == CellState::Ready ? formatCell(*ref.value) : std::string();
    }

    // Every cell of two sheets, as shown.
    void checkSameCells(TableModel &a, TableModel &b)
    {
        REQUIRE(a.rowCount() == b.rowCount());
        // Every row read first: the columns grow with wider rows as they come.
        if (a.rowCount().value > 0)
        {
            waitForCell(a, a.rowCount().value - 1, 0);
            waitForCell(b, b.rowCount().value - 1, 0);
        }
        REQUIRE(a.columnCount() == b.columnCount());
        CHECK(a.hasHeader() == b.hasHeader());
        for (size_t c = 0; c < a.columnCount(); ++c)
            CHECK(a.columnName(c) == b.columnName(c));
        for (size_t r = 0; r < a.rowCount().value; ++r)
        {
            for (size_t c = 0; c < a.columnCount(); ++c)
            {
                INFO("row " << r << ", column " << c);
                CHECK(shown(a, r, c) == shown(b, r, c));
            }
        }
    }
} // namespace

TEST_CASE("normal.ods: header, types and text")
{
    auto book = TableBook::open(kDemoData / "normal.ods");
    REQUIRE(book->sheetNames() == std::vector<std::string>{"normal"});
    CHECK(book->usesHeaderOption());
    auto model = openSheet(*book, "normal");

    CHECK(model->hasHeader());
    CHECK(model->columnName(0) == "ID");
    CHECK(model->columnName(7) == "Birthday");
    CHECK(model->rowCount() == RowCount{54, RowCount::Kind::Exact});
    CHECK(model->columnCount() == 9);

    const CellRef id = waitForCell(*model, 0, 0);
    REQUIRE(id.state == CellState::Ready);
    CHECK(std::holds_alternative<int64_t>(*id.value));
    const CellRef birthday = waitForCell(*model, 0, 7);
    REQUIRE(birthday.state == CellState::Ready);
    CHECK(std::holds_alternative<Date>(*birthday.value));
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK(cellText(*model, 0, 6) == "941751.65");
    CHECK(cellText(*model, 0, 7) == "1986-08-02");
    CHECK(cellText(*model, 0, 8) == "TRUE");
    CHECK(cellText(*model, 1, 5) == "人事部");
    CHECK(cellText(*model, 50, 1) == "Smith, John");
    CHECK(cellText(*model, 51, 1) == "複数行\nの名前");
    CHECK(isEmpty(*model, 52, 3));
    CHECK(cellText(*model, 53, 1) == "Leading and trailing spaces  ");
    CHECK(cellText(*model, 53, 2) == "  pad@example.com");
    CHECK(cellText(*model, 53, 4) == " Osaka ");
}

TEST_CASE("ODS demo files read as their .xlsx counterparts")
{
    for (const char *name : {"normal", "ragged", "large", "多言語サンプル"})
    {
        INFO(name);
        auto ods = TableBook::open(kDemoData / utf8Path(std::string(name) + ".ods"));
        auto xlsx = TableBook::open(kDemoData / utf8Path(std::string(name) + ".xlsx"));
        REQUIRE(ods->sheetNames().size() == 1);
        checkSameCells(*openSheet(*ods, ods->sheetNames().front()), *openSheet(*xlsx, xlsx->sheetNames().front()));
    }
    auto ods = TableBook::open(kDemoData / "multisheet.ods");
    auto xlsx = TableBook::open(kDemoData / "multisheet.xlsx");
    for (const std::string sheet : {"People", "売上", "Empty", "Offset", "Hidden", "Wide"})
    {
        INFO(sheet);
        checkSameCells(*openSheet(*ods, sheet), *openSheet(*xlsx, sheet));
    }
}

TEST_CASE("ragged.ods: missing rows and cells")
{
    auto book = TableBook::open(kDemoData / "ragged.ods");
    auto model = openSheet(*book, book->sheetNames().front());
    CHECK(cellText(*model, 0, 4) == "r1c5");
    CHECK(cellText(*model, 2, 6) == "r3c7");
    CHECK(isEmpty(*model, 1, 3));
    CHECK(isEmpty(*model, 4, 0)); // an empty row, written as a repeat
    CHECK(cellText(*model, 5, 0) == "r6c1");
}

TEST_CASE("dates.ods: dates, date-times, times and durations")
{
    auto book = TableBook::open(kDemoData / "dates.ods");
    auto dates = openSheet(*book, "dates");
    CHECK(dates->rowCount() == RowCount{61, RowCount::Kind::Exact});
    // No 1900 leap-year bug: ODF stores dates as ISO text.
    CHECK(cellText(*dates, 0, 1) == "1900-01-01");
    CHECK(cellText(*dates, 0, 2) == "1900-01-01 00:00:00");
    CHECK(cellText(*dates, 1, 1) == "1900-03-01");
    CHECK(cellText(*dates, 1, 2) == "1900-03-01 12:00:00");
    CHECK(cellText(*dates, 4, 2) == "2000-02-29 23:59:59");
    CHECK(cellText(*dates, 5, 2) == "2026-10-05 09:30:15.250");
    CHECK(cellText(*dates, 5, 3) == "09:30:15");
    CHECK(cellText(*dates, 6, 1) == "9999-12-31");
    CHECK(cellText(*dates, 0, 4) == "11:12:39");
    CHECK(cellText(*dates, 1, 4) == "148:36:00"); // a duration past a day
    CHECK(cellText(*dates, 5, 5) == "2026/10/05"); // text that looks like a date
    const CellRef stamp = waitForCell(*dates, 3, 2);
    REQUIRE(stamp.state == CellState::Ready);
    CHECK(std::holds_alternative<DateTime>(*stamp.value));
    for (size_t c = 1; c <= 4; ++c)
        CHECK(isEmpty(*dates, 60, c));
}

TEST_CASE("merged.ods and multisheet.ods")
{
    auto merged = TableBook::open(kDemoData / "merged.ods");
    CHECK(merged->sheetNames() == std::vector<std::string>{"report", "header_merge"});
    auto report = openSheet(*merged, "report");
    CHECK_FALSE(report->hasHeader());
    CHECK(cellText(*report, 0, 0) == "Quarterly report");
    CHECK(report->mergedCells().size() == 10);
    // The last merge (A15:C17) reaches past the last row with a value.
    CHECK(report->rowCount() == RowCount{17, RowCount::Kind::Exact});
    CHECK(cellText(*report, 9, 0) == "Total");
    CHECK(cellText(*report, 9, 2) == "2988"); // a formula's computed value
    const auto ranges = report->mergedCells();
    CHECK(std::find_if(ranges.begin(), ranges.end(), [](const CellRange &r)
                       { return r.row == 14 && r.col == 0 && r.rows == 3 && r.cols == 3; }) != ranges.end());
    auto header_merge = openSheet(*merged, "header_merge");
    CHECK(header_merge->hasHeader());
    CHECK(header_merge->columnName(1) == "Contact");
    CHECK(header_merge->mergedCells().empty()); // the merge is in the header row

    auto multi = TableBook::open(kDemoData / "multisheet.ods");
    CHECK(multi->sheetNames() == std::vector<std::string>{"People", "売上", "Empty", "Offset", "Hidden", "Wide"});
    CHECK(openSheet(*multi, "Empty")->rowCount() == RowCount{0, RowCount::Kind::Exact});
    auto offset = openSheet(*multi, "Offset");
    CHECK_FALSE(offset->hasHeader());
    CHECK(cellText(*offset, 2, 2) == "Key");
    CHECK(openSheet(*multi, "Wide")->columnCount() == 100);
    auto sales = openSheet(*multi, "売上");
    CHECK(cellText(*sales, 0, 0) == "2026-01-01");
}

TEST_CASE("large.ods: blocks, sorting and no memory checkpoints")
{
    TableModel::Options options;
    options.memory_checkpoints = {1};
    auto book = TableBook::open(kDemoData / "large.ods", {}, BackendRegistry::builtin(), options);
    auto model = openSheet(*book, "large");
    CHECK(model->rowCount() == RowCount{10000, RowCount::Kind::Exact});
    CHECK(cellText(*model, 9999, 0) == "10000");
    model->setView({.sort = {{.column = 0, .ascending = false}}});
    REQUIRE(waitFor([&]
                    { return !model->view().sort.empty(); }));
    CHECK(cellText(*model, 0, 0) == "10000");
    CHECK_FALSE(model->memoryCheckpoint());
}

TEST_CASE("ODS cell values and text")
{
    std::string rows = "<table:table-row>" + cell(R"(office:value-type="string")", "Label") +
                       cell(R"(office:value-type="string")", "Value") + "</table:table-row>";
    rows += labelled("integer", cell(R"(office:value-type="float" office:value="42")", "42"));
    rows += labelled("float", cell(R"(office:value-type="float" office:value="0.25")", "0.25"));
    rows += labelled("exponent", cell(R"(office:value-type="float" office:value="1E+20")", "1E+20"));
    rows += labelled("percent", cell(R"(office:value-type="percentage" office:value="0.125")", "12.50%"));
    rows += labelled("currency", cell(R"(office:value-type="currency" office:currency="JPY" office:value="-1234.5")",
                                      "-¥1,235"));
    rows += labelled("true", cell(R"(office:value-type="boolean" office:boolean-value="true")", "TRUE"));
    rows += labelled("false", cell(R"(office:value-type="boolean" office:boolean-value="false")", "FALSE"));
    rows += labelled("date", cell(R"(office:value-type="date" office:date-value="2026-10-05")", "10/05/26"));
    rows += labelled("datetime",
                     cell(R"(office:value-type="date" office:date-value="2026-10-05T09:30:15.25")", "shown"));
    rows += labelled("bad date", cell(R"(office:value-type="date" office:date-value="soon")", "soon"));
    rows += labelled("time", cell(R"(office:value-type="time" office:time-value="PT09H30M15S")", "09:30:15 AM"));
    rows += labelled("elapsed", cell(R"(office:value-type="time" office:time-value="PT50H03M04S")", "50:03:04"));
    rows += labelled("days", cell(R"(office:value-type="time" office:time-value="P1DT2H")", "26:00"));
    rows += labelled("negative", cell(R"(office:value-type="time" office:time-value="-PT01H00M00S")", "-1:00"));
    rows += labelled("fraction",
                     cell(R"(office:value-type="time" office:time-value="PT00H00M01.23456S")", "00:00:01"));
    rows += labelled("string value", R"(<table:table-cell office:value-type="string" office:string-value="from attribute"/>)");
    rows += labelled("formula", cell(R"(table:formula="of:=1+2" office:value-type="float" office:value="3" calcext:value-type="float")", "3"));
    rows += labelled("error", cell(R"(table:formula="of:=1/0" office:value-type="string" office:string-value="" calcext:value-type="error")", "#DIV/0!"));
    rows += labelled("spaces", cell(R"(office:value-type="string")",
                                    R"(<text:s/>lead<text:s text:c="3"/>mid<text:tab/>tab<text:s/>)"));
    rows += labelled("collapsed", cell(R"(office:value-type="string")", "  a \n\t b  "));
    // As LibreOffice writes " Osaka ".
    rows += labelled("trailing", cell(R"(office:value-type="string")", "<text:s/>Osaka "));
    rows += labelled("paragraphs", R"(<table:table-cell office:value-type="string"><text:p>one</text:p><text:p>two<text:line-break/>three</text:p></table:table-cell>)");
    rows += labelled("spans", cell(R"(office:value-type="string")",
                                   R"(a<text:span text:style-name="T1">b</text:span><text:a xlink:href="https://example.com">c</text:a> d)"));
    rows += labelled("annotation", R"(<table:table-cell office:value-type="string"><office:annotation><dc:creator>me</dc:creator><text:p>a comment</text:p></office:annotation><text:p>visible</text:p></table:table-cell>)");
    rows += labelled("shape", R"(<table:table-cell office:value-type="string"><draw:frame><draw:text-box><text:p>in a box</text:p></draw:text-box></draw:frame><text:p>cell</text:p></table:table-cell>)");
    rows += labelled("untyped", "<table:table-cell><text:p>no type</text:p></table:table-cell>");
    rows += labelled("escaped", cell(R"(office:value-type="string")", "&lt;a&gt; &amp; &#x1F600;"));
    const auto path = writeOds("tableskim-test-values.ods",
                               odsContent(R"(<table:table table:name="Values"><table:table-column table:number-columns-repeated="2"/>)" +
                                          rows + "</table:table>"));
    {
        auto book = TableBook::open(path);
        auto model = openSheet(*book, "Values");
        REQUIRE(model->hasHeader());
        std::map<std::string, std::string> values;
        for (size_t r = 0; r < model->rowCount().value; ++r)
            values[cellText(*model, r, 0)] = cellText(*model, r, 1);
        CHECK(values["integer"] == "42");
        CHECK(values["float"] == "0.25");
        CHECK(values["exponent"] == "1e+20");
        CHECK(values["percent"] == "0.125");
        CHECK(values["currency"] == "-1234.5");
        CHECK(values["true"] == "TRUE");
        CHECK(values["false"] == "FALSE");
        CHECK(values["date"] == "2026-10-05");
        CHECK(values["datetime"] == "2026-10-05 09:30:15.250");
        CHECK(values["bad date"] == "soon");
        CHECK(values["time"] == "09:30:15");
        CHECK(values["elapsed"] == "50:03:04");
        CHECK(values["days"] == "26:00:00");
        CHECK(values["negative"] == "-01:00:00");
        CHECK(values["fraction"] == "00:00:01.235");
        CHECK(values["string value"] == "from attribute");
        CHECK(values["formula"] == "3");
        CHECK(values["error"] == "#DIV/0!");
        CHECK(values["spaces"] == " lead   mid\ttab ");
        CHECK(values["collapsed"] == "a b ");
        CHECK(values["trailing"] == " Osaka ");
        CHECK(values["paragraphs"] == "one\ntwo\nthree");
        CHECK(values["spans"] == "abc d");
        CHECK(values["annotation"] == "visible");
        CHECK(values["shape"] == "cell");
        CHECK(values["untyped"] == "no type");
        CHECK(values["escaped"] == "<a> & \xF0\x9F\x98\x80");

        const CellRef integer = waitForCell(*model, 0, 1);
        REQUIRE(integer.state == CellState::Ready);
        CHECK(std::holds_alternative<int64_t>(*integer.value));
    }
    std::filesystem::remove(path);
}

TEST_CASE("ODS repeated, grouped and covered cells")
{
    const std::string empty_row = R"(<table:table-row><table:table-cell table:number-columns-repeated="1024"/></table:table-row>)";
    const std::string tables =
        R"(<table:table table:name="Repeats"><table:table-column table:number-columns-repeated="1024"/>)"
        R"(<table:table-header-rows><table:table-row>)" +
        cell(R"(office:value-type="string")", "A") + cell(R"(office:value-type="string")", "B") +
        cell(R"(office:value-type="string")", "C") + cell(R"(office:value-type="string")", "D") +
        cell(R"(office:value-type="string")", "E") +
        R"(<table:table-cell table:number-columns-repeated="1019"/></table:table-row></table:table-header-rows>)"
        // Three equal rows, each with a value repeated over two columns.
        R"(<table:table-row table:number-rows-repeated="3">)" + cell(R"(office:value-type="string")", "same") +
        cell(R"(table:number-columns-repeated="2" office:value-type="float" office:value="7")", "7") +
        R"(</table:table-row>)"
        // Two empty rows, then a row with a gap of three cells, in a group.
        R"(<table:table-row table:number-rows-repeated="2"><table:table-cell table:number-columns-repeated="1024"/></table:table-row>)"
        R"(<table:table-row-group><table:table-row>)" +
        cell(R"(office:value-type="string")", "x") + R"(<table:table-cell table:number-columns-repeated="3"/>)" +
        cell(R"(office:value-type="string")", "y") +
        R"(<table:table-cell table:number-columns-repeated="1000"/></table:table-row></table:table-row-group>)"
        // A merge over two rows and three columns.
        R"(<table:table-row>)" +
        cell(R"(table:number-columns-spanned="3" table:number-rows-spanned="2" office:value-type="string")", "merged") +
        R"(<table:covered-table-cell table:number-columns-repeated="2"/>)" + cell(R"(office:value-type="float" office:value="1")", "1") +
        R"(</table:table-row><table:table-row><table:covered-table-cell table:number-columns-repeated="3"/>)" +
        cell(R"(office:value-type="float" office:value="2")", "2") +
        R"(</table:table-row>)"
        // LibreOffice's filler to the end of the sheet.
        R"(<table:table-row table:number-rows-repeated="1048567"><table:table-cell table:number-columns-repeated="1024"/></table:table-row>)"
        "</table:table>"
        // A sheet that ends with a merge alone, and one that is empty.
        R"(<table:table table:name="Tail"><table:table-row>)" +
        cell(R"(office:value-type="string")", "head") + "</table:table-row>" + empty_row +
        R"(<table:table-row><table:table-cell table:number-rows-spanned="4"/></table:table-row>)" +
        R"(<table:table-row table:number-rows-repeated="3"><table:covered-table-cell/></table:table-row>)" +
        "</table:table>"
        R"(<table:table table:name="Blank">)" + empty_row + "</table:table>";
    const auto path = writeOds("tableskim-test-repeats.ods", odsContent(tables));
    {
        auto book = TableBook::open(path);
        CHECK(book->sheetNames() == std::vector<std::string>{"Repeats", "Tail", "Blank"});
        auto model = openSheet(*book, "Repeats");
        CHECK(model->hasHeader());
        CHECK(model->columnName(4) == "E");
        CHECK(model->columnCount() == 5);
        CHECK(model->rowCount() == RowCount{8, RowCount::Kind::Exact});
        for (size_t r = 0; r < 3; ++r)
        {
            CHECK(cellText(*model, r, 0) == "same");
            CHECK(cellText(*model, r, 1) == "7");
            CHECK(cellText(*model, r, 2) == "7");
            CHECK(isEmpty(*model, r, 3));
        }
        CHECK(isEmpty(*model, 3, 0));
        CHECK(isEmpty(*model, 4, 0));
        CHECK(cellText(*model, 5, 0) == "x");
        CHECK(isEmpty(*model, 5, 3));
        CHECK(cellText(*model, 5, 4) == "y");
        CHECK(cellText(*model, 6, 0) == "merged");
        CHECK(cellText(*model, 6, 3) == "1");
        CHECK(cellText(*model, 7, 3) == "2");
        const auto merged = model->mergedCells();
        REQUIRE(merged.size() == 1);
        CHECK(merged[0].row == 6);
        CHECK(merged[0].col == 0);
        CHECK(merged[0].rows == 2);
        CHECK(merged[0].cols == 3);

        auto tail = openSheet(*book, "Tail");
        const size_t header = tail->hasHeader() ? 1 : 0;
        CHECK(tail->rowCount() == RowCount{6 - header, RowCount::Kind::Exact});
        CHECK(isEmpty(*tail, 5 - header - 1, 0));
        CHECK(tail->mergedCells().size() == 1);
        CHECK(openSheet(*book, "Blank")->rowCount() == RowCount{0, RowCount::Kind::Exact});
    }
    std::filesystem::remove(path);
}

TEST_CASE("ODS repeats are bounded")
{
    const std::string tables =
        R"(<table:table table:name="Tall"><table:table-row table:number-rows-repeated="4000000000">)" +
        cell(R"(office:value-type="float" office:value="1")", "1") + "</table:table-row></table:table>" +
        R"(<table:table table:name="Wide"><table:table-row>)" +
        cell(R"(table:number-columns-repeated="4000000000" office:value-type="float" office:value="2")", "2") +
        "</table:table-row></table:table>";
    const auto path = writeOds("tableskim-test-huge.ods", odsContent(tables));
    {
        auto book = TableBook::open(path, {.header = OpenOptions::Header::No});
        auto tall = openSheet(*book, "Tall");
        CHECK(tall->rowCount() == RowCount{size_t{1} << 24, RowCount::Kind::Exact});
        CHECK(cellText(*tall, 0, 0) == "1");
        auto wide = openSheet(*book, "Wide");
        CHECK(wide->rowCount() == RowCount{1, RowCount::Kind::Exact});
        CHECK(cellText(*wide, 0, 16383) == "2");
        CHECK(wide->columnCount() == 16384);
    }
    std::filesystem::remove(path);
}

TEST_CASE("ODS header option and namespace prefixes")
{
    // Any prefixes may stand for the namespaces.
    const std::string content = R"(<?xml version="1.0"?>
<o:document-content xmlns:o="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
 xmlns:t="urn:oasis:names:tc:opendocument:xmlns:table:1.0"
 xmlns:p="urn:oasis:names:tc:opendocument:xmlns:text:1.0"
 xmlns:table="urn:example:not-odf"><o:body><o:spreadsheet>
<t:table t:name="Prefixed"><t:table-row>
<t:table-cell o:value-type="string"><p:p>Name</p:p></t:table-cell>
<t:table-cell o:value-type="string"><p:p>Score</p:p></t:table-cell></t:table-row>
<t:table-row><t:table-cell o:value-type="string"><p:p>a</p:p></t:table-cell>
<t:table-cell o:value-type="float" o:value="1"><p:p>1</p:p></t:table-cell></t:table-row>
<table:table-row><table:table-cell><p:p>not a row</p:p></table:table-cell></table:table-row>
</t:table></o:spreadsheet></o:body></o:document-content>)";
    const auto path = writeOds("tableskim-test-prefixes.ods", content);
    {
        auto book = TableBook::open(path);
        REQUIRE(book->sheetNames() == std::vector<std::string>{"Prefixed"});
        auto model = openSheet(*book, "Prefixed");
        CHECK(model->hasHeader());
        CHECK(model->columnName(1) == "Score");
        CHECK(model->rowCount() == RowCount{1, RowCount::Kind::Exact});
        CHECK(cellText(*model, 0, 1) == "1");
    }
    {
        auto book = TableBook::open(path, {.header = OpenOptions::Header::No});
        auto model = openSheet(*book, "Prefixed");
        CHECK_FALSE(model->hasHeader());
        CHECK(model->rowCount() == RowCount{2, RowCount::Kind::Exact});
        CHECK(cellText(*model, 0, 0) == "Name");
    }
    std::filesystem::remove(path);
}

TEST_CASE("ODS files are told by their contents")
{
    const std::string content = odsContent(R"(<table:table table:name="S"><table:table-row>)" +
                                           cell(R"(office:value-type="string")", "v") +
                                           "</table:table-row></table:table>");
    // The mimetype entry first tells the type, whatever the extension.
    const auto path = writeOds("tableskim-test-type.bin", content);
    {
        auto book = TableBook::open(path);
        CHECK(book->sheetNames() == std::vector<std::string>{"S"});
    }
    std::filesystem::remove(path);
    // A template holds a spreadsheet too.
    const auto ots = writeOds("tableskim-test-template.ots", content, std::string(kOdsMimeType) + "-template");
    CHECK(TableBook::open(ots)->sheetNames() == std::vector<std::string>{"S"});
    std::filesystem::remove(ots);
    // Without a mimetype entry, the extension tells.
    const auto bare = writeOds("tableskim-test-bare.ods", content, "");
    CHECK(TableBook::open(bare)->sheetNames() == std::vector<std::string>{"S"});
    std::filesystem::remove(bare);
}

TEST_CASE("broken or unsupported ODS files fail to open cleanly")
{
    const std::string table = R"(<table:table table:name="S"><table:table-row>)" +
                              cell(R"(office:value-type="string")", "v") + "</table:table-row></table:table>";
    std::filesystem::path path;
    SECTION("not a ZIP archive")
    {
        path = std::filesystem::temp_directory_path() / "tableskim-test-broken.ods";
        std::ofstream(path, std::ios::binary) << "PK\x03\x04 this is not really a zip";
    }
    SECTION("malformed XML")
    {
        path = writeOds("tableskim-test-malformed.ods", odsContent(table + "<table:table>"));
    }
    SECTION("a document type declaration")
    {
        const std::string content = odsContent(table);
        const size_t root = content.find("<office:document-content");
        path = writeOds("tableskim-test-doctype.ods",
                        content.substr(0, root) + R"(<!DOCTYPE x [<!ENTITY a "aaaaaaaa">]>)" + content.substr(root));
    }
    SECTION("another kind of document")
    {
        path = writeOds("tableskim-test-text.ods", odsContent(table), "application/vnd.oasis.opendocument.text");
    }
    SECTION("no content.xml")
    {
        path = std::filesystem::temp_directory_path() / "tableskim-test-empty.ods";
        writeTestZip(path, {{"mimetype", kOdsMimeType}});
    }
    SECTION("password-protected")
    {
        const std::string manifest =
            R"(<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0">)"
            R"(<manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml">)"
            R"(<manifest:encryption-data manifest:checksum-type="SHA1/1K" manifest:checksum="x"/>)"
            R"(</manifest:file-entry></manifest:manifest>)";
        path = writeOds("tableskim-test-encrypted.ods", "\x01\x02 encrypted bytes", kOdsMimeType,
                        {{"META-INF/manifest.xml", manifest}});
    }
    CHECK_THROWS_AS(TableBook::open(path), std::exception);
    std::filesystem::remove(path);
}

// Hidden (run with "[.super-large]"): one million rows in one table.
TEST_CASE("super-large.ods: reaches the last row", "[.super-large]")
{
    auto book = TableBook::open(kDemoData / "super-large.ods");
    REQUIRE(book->sheetNames() == std::vector<std::string>{"super-large"});
    auto model = book->sheet("super-large");
    REQUIRE(waitFor([&]
                    { return model->state() != ModelState::Opening; }, std::chrono::minutes(5)));
    INFO(model->errorMessage());
    REQUIRE(model->state() == ModelState::Ready);
    CHECK(model->rowCount() == RowCount{1000000, RowCount::Kind::Exact});
    model->requestRange(999990, 999999);
    REQUIRE(waitFor([&]
                    { return model->peek(999999, 0).state != CellState::Loading; }, std::chrono::minutes(5)));
    CHECK(cellText(*model, 999999, 0) == "1000000");
    CHECK(cellText(*model, 999999, 5) == "2026-12-03 00:47:24");
}
