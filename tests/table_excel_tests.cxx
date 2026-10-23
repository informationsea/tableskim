#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <OpenXLSX.hpp>
#include <XLXmlParser.hpp>

#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    class TestWorkbook : public OpenXLSX::XLWorkbook
    {
    public:
        explicit TestWorkbook(const OpenXLSX::XLWorkbook &book) : OpenXLSX::XLWorkbook(book) {}
        void setDate1904(const char *value)
        {
            auto root = xmlDocument().document_element();
            auto properties = root.child("workbookPr");
            if (!properties)
                properties = root.prepend_child("workbookPr");
            auto attribute = properties.attribute("date1904");
            if (!attribute)
                attribute = properties.append_attribute("date1904");
            attribute.set_value(value);
        }
    };
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

    // Two sheets, typed values, date/time formats, a merged range and gaps.
    std::filesystem::path createWorkbook()
    {
        const auto path = std::filesystem::temp_directory_path() / "tableskim-test-book.xlsx";
        OpenXLSX::XLDocument doc;
        doc.create(path.string(), true);
        auto &styles = doc.styles();

        const auto date_style = styles.cellFormats().create();
        styles.cellFormats()[date_style].setNumberFormatId(14); // built-in m/d/yyyy

        const auto time_format = styles.numberFormats().create();
        styles.numberFormats()[time_format].setNumberFormatId(170);
        styles.numberFormats()[time_format].setFormatCode("hh:mm:ss");
        const auto time_style = styles.cellFormats().create();
        styles.cellFormats()[time_style].setNumberFormatId(170);

        const auto datetime_format = styles.numberFormats().create();
        styles.numberFormats()[datetime_format].setNumberFormatId(171);
        styles.numberFormats()[datetime_format].setFormatCode("yyyy/mm/dd hh:mm");
        const auto datetime_style = styles.cellFormats().create();
        styles.cellFormats()[datetime_style].setNumberFormatId(171);

        doc.workbook().worksheet("Sheet1").setName("Data");
        auto data = doc.workbook().worksheet("Data");
        data.cell("A1").value() = "Name";
        data.cell("B1").value() = "Count";
        data.cell("C1").value() = "Ratio";
        data.cell("D1").value() = "Day";
        data.cell("E1").value() = "Time";
        data.cell("F1").value() = "Stamp";
        data.cell("G1").value() = "Flag";

        data.cell("A2").value() = "alpha";
        data.cell("B2").value() = 42;
        data.cell("C2").value() = 0.25;
        data.cell("D2").value() = 45000; // 2023-03-15
        data.cell("D2").setCellFormat(date_style);
        data.cell("E2").value() = 0.5; // 12:00:00
        data.cell("E2").setCellFormat(time_style);
        data.cell("F2").value() = 45000.75; // 2023-03-15 18:00
        data.cell("F2").setCellFormat(datetime_style);
        data.cell("G2").value() = true;

        // row 3 is missing; row 4 has a gap at column B
        data.cell("A4").value() = "gamma";
        data.cell("C4").value() = 1.5;
        data.mergeCells("A5:B6");
        data.cell("A5").value() = "merged";

        doc.workbook().addWorksheet("Numbers");
        auto numbers = doc.workbook().worksheet("Numbers");
        for (uint32_t r = 1; r <= 1200; ++r)
            numbers.cell(r, 1).value() = static_cast<int64_t>(r);

        doc.save();
        doc.close();
        return path;
    }
} // namespace

TEST_CASE("normal.xlsx: header, types and dates")
{
    auto book = TableBook::open(kDemoData / "normal.xlsx");
    REQUIRE(book->sheetNames() == std::vector<std::string>{"normal"});
    auto model = openSheet(*book, "normal");

    CHECK(model->hasHeader());
    CHECK(model->columnName(0) == "ID");
    CHECK(model->columnName(7) == "Birthday");
    CHECK(model->rowCount() == RowCount{54, RowCount::Kind::Exact});

    const CellRef id = waitForCell(*model, 0, 0);
    REQUIRE(id.state == CellState::Ready);
    CHECK(std::holds_alternative<int64_t>(*id.value));
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK(cellText(*model, 0, 6) == "941751.65");
    CHECK(cellText(*model, 0, 7) == "1986-08-02");
    CHECK(cellText(*model, 0, 8) == "TRUE");
    CHECK(cellText(*model, 1, 5) == "人事部");
}

TEST_CASE("ragged.xlsx: missing rows and cells")
{
    auto book = TableBook::open(kDemoData / "ragged.xlsx");
    auto model = openSheet(*book, book->sheetNames().front());

    CHECK(cellText(*model, 0, 4) == "r1c5");
    CHECK(cellText(*model, 2, 6) == "r3c7");
    CHECK(waitForCell(*model, 1, 3).state == CellState::Empty);
    // sheet row 6 is not stored in the file: it is an empty row
    CHECK(waitForCell(*model, 4, 0).state == CellState::Empty);
    CHECK(cellText(*model, 5, 0) == "r6c1");
}

TEST_CASE("generated workbook: sheets, formats, merges")
{
    const auto path = createWorkbook();
    {
        auto book = TableBook::open(path);
        REQUIRE(book->sheetNames() == std::vector<std::string>{"Data", "Numbers"});

        auto data = openSheet(*book, "Data");
        CHECK(data->columnName(6) == "Flag");
        CHECK(data->rowCount() == RowCount{5, RowCount::Kind::Exact});
        CHECK(cellText(*data, 0, 0) == "alpha");
        CHECK(cellText(*data, 0, 1) == "42");
        CHECK(cellText(*data, 0, 2) == "0.25");
        CHECK(cellText(*data, 0, 3) == "2023-03-15");
        CHECK(cellText(*data, 0, 4) == "12:00:00");
        CHECK(cellText(*data, 0, 5) == "2023-03-15 18:00:00");
        CHECK(cellText(*data, 0, 6) == "TRUE");
        CHECK(waitForCell(*data, 1, 0).state == CellState::Empty); // missing row
        CHECK(cellText(*data, 2, 0) == "gamma");
        const CellRef gap = waitForCell(*data, 2, 1);
        CHECK((gap.state == CellState::Empty ||
               (gap.state == CellState::Ready && std::holds_alternative<std::monostate>(*gap.value))));
        CHECK(cellText(*data, 2, 2) == "1.5");

        const auto merged = data->mergedCells();
        REQUIRE(merged.size() == 1);
        CHECK(merged[0].row == 3);
        CHECK(merged[0].col == 0);
        CHECK(merged[0].rows == 2);
        CHECK(merged[0].cols == 2);

        // No header (numeric first row); more than one block.
        auto numbers = openSheet(*book, "Numbers");
        CHECK_FALSE(numbers->hasHeader());
        CHECK(numbers->rowCount() == RowCount{1200, RowCount::Kind::Exact});
        CHECK(cellText(*numbers, 1199, 0) == "1200");
        CHECK(numbers->viewCapabilities().sort);
        numbers->setView({.sort = {{.column = 0, .ascending = false}}});
        REQUIRE(waitFor([&]
                        { return !numbers->view().sort.empty(); }));
        CHECK(cellText(*numbers, 0, 0) == "1200");
    }
    std::filesystem::remove(path);
}

TEST_CASE("Excel dates respect the workbook date system", "[regression]")
{
    for (const auto *setting : {"0", "false", "1", "true"})
    {
        INFO(setting);
        const bool date1904 = std::string(setting) == "1" || std::string(setting) == "true";
        const auto path = std::filesystem::temp_directory_path() / "tableskim-test-date-system.xlsx";
        OpenXLSX::XLDocument doc;
        doc.create(path.string(), true);
        TestWorkbook(doc.workbook()).setDate1904(setting);
        auto &styles = doc.styles();
        const auto date_style = styles.cellFormats().create();
        styles.cellFormats()[date_style].setNumberFormatId(14);
        const auto datetime_style = styles.cellFormats().create();
        styles.cellFormats()[datetime_style].setNumberFormatId(22);
        auto sheet = doc.workbook().worksheet("Sheet1");
        const double epoch = date1904 ? 24107.0 : 25569.0;
        sheet.cell("A1").value() = epoch;
        sheet.cell("A1").setCellFormat(date_style);
        sheet.cell("B1").value() = epoch + 0.5;
        sheet.cell("B1").setCellFormat(datetime_style);
        sheet.cell("A2").value() = epoch - 1;
        sheet.cell("A2").setCellFormat(date_style);
        doc.save();
        doc.close();
        {
            auto book = TableBook::open(path, {.header = OpenOptions::Header::No});
            auto model = openSheet(*book, "Sheet1");
            CHECK(cellText(*model, 0, 0) == "1970-01-01");
            CHECK(cellText(*model, 0, 1) == "1970-01-01 12:00:00");
            CHECK(cellText(*model, 1, 0) == "1969-12-31");
        }
        std::filesystem::remove(path);
    }
}

TEST_CASE("unsupported or broken Excel files fail to open cleanly")
{
    const auto path = std::filesystem::temp_directory_path() / "tableskim-test-broken.xlsx";
    {
        std::ofstream out(path, std::ios::binary);
        out << "PK\x03\x04 this is not really a zip";
    }
    CHECK_THROWS_AS(TableBook::open(path), std::exception);
    std::filesystem::remove(path);
}

TEST_CASE("dates.xlsx: 1900 leap-year bug, rounding, time formats")
{
    auto book = TableBook::open(kDemoData / "dates.xlsx");
    auto dates = openSheet(*book, "dates");
    // serials before 1900-03-01 are shifted by Excel's fictitious 1900-02-29
    CHECK(cellText(*dates, 0, 1) == "1900-01-01");
    CHECK(cellText(*dates, 0, 2) == "1900-01-01 00:00:00");
    CHECK(cellText(*dates, 1, 1) == "1900-03-01");
    CHECK(cellText(*dates, 4, 2) == "2000-02-29 23:59:59");
    CHECK(cellText(*dates, 5, 2) == "2026-10-05 09:30:15.250");
    CHECK(cellText(*dates, 5, 3) == "09:30:15");
    CHECK(cellText(*dates, 6, 1) == "9999-12-31");
    CHECK(cellText(*dates, 0, 4) == "11:12:39");  // [hh]:mm:ss duration
    CHECK(cellText(*dates, 5, 5) == "2026/10/05"); // text that looks like a date

    // Every sample on the "formats" sheet: Value (col 1) vs Kind (3) / ISO (4).
    auto formats = openSheet(*book, "formats");
    REQUIRE(formats->rowCount().isExact());
    for (size_t r = 0; r < formats->rowCount().value; ++r)
    {
        const std::string label = cellText(*formats, r, 0);
        const std::string kind = cellText(*formats, r, 3);
        const std::string iso = cellText(*formats, r, 4);
        const CellRef value = waitForCell(*formats, r, 1);
        REQUIRE(value.state == CellState::Ready);
        INFO(label << ": " << formatCell(*value.value) << " vs " << iso);
        if (kind == "date")
        {
            CHECK(std::holds_alternative<Date>(*value.value));
            CHECK(formatCell(*value.value) == iso);
        }
        else if (kind == "datetime" && label != "fractional seconds")
        {
            CHECK(std::holds_alternative<DateTime>(*value.value));
            CHECK(formatCell(*value.value) == iso);
        }
        else if (kind == "time" || kind == "duration")
        {
            CHECK(formatCell(*value.value) == iso);
        }
        else if (kind == "number")
        {
            CHECK((std::holds_alternative<int64_t>(*value.value) || std::holds_alternative<double>(*value.value)));
        }
    }
}

TEST_CASE("dates1904.xlsx uses the 1904 date system")
{
    auto book = TableBook::open(kDemoData / "dates1904.xlsx");
    auto dates = openSheet(*book, "dates");
    CHECK(cellText(*dates, 0, 0) == "4");
    CHECK(cellText(*dates, 0, 1) == "1970-01-01");
    CHECK(cellText(*dates, 2, 2) == "2026-10-05 09:30:15.250");
}

TEST_CASE("merged.xlsx and multisheet.xlsx")
{
    auto merged = TableBook::open(kDemoData / "merged.xlsx");
    auto report = openSheet(*merged, "report");
    // A merged title row is not a header.
    CHECK_FALSE(report->hasHeader());
    CHECK(cellText(*report, 0, 0) == "Quarterly report");
    CHECK(report->mergedCells().size() == 10);
    // The last merge (A15:C17) reaches past the last stored row.
    CHECK(report->rowCount() == RowCount{17, RowCount::Kind::Exact});
    auto header_merge = openSheet(*merged, "header_merge");
    CHECK(header_merge->hasHeader());
    CHECK(header_merge->columnName(1) == "Contact");

    auto multi = TableBook::open(kDemoData / "multisheet.xlsx");
    CHECK(multi->sheetNames() == std::vector<std::string>{"People", "売上", "Empty", "Offset", "Hidden", "Wide"});
    CHECK(openSheet(*multi, "Empty")->rowCount() == RowCount{0, RowCount::Kind::Exact});
    auto offset = openSheet(*multi, "Offset");
    CHECK_FALSE(offset->hasHeader());
    CHECK(cellText(*offset, 2, 2) == "Key");
    CHECK(openSheet(*multi, "Wide")->columnCount() == 100);
}

// Hidden (run with "[.super-large]"): one million rows in one worksheet.
TEST_CASE("super-large.xlsx: reaches the last row", "[.super-large]")
{
    auto book = TableBook::open(kDemoData / "super-large.xlsx");
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

TEST_CASE("Excel files have no memory checkpoints")
{
    TableModel::Options options;
    options.memory_checkpoints = {1};
    auto book = TableBook::open(kDemoData / "large.xlsx", {}, BackendRegistry::builtin(), options);
    auto model = book->sheet(book->sheetNames().front());
    model->setView({.sort = {{.column = 0, .ascending = false}}});
    REQUIRE(waitFor([&]
                    { return !model->view().sort.empty(); }));
    CHECK_FALSE(model->memoryCheckpoint());
    CHECK(model->rowCount().isExact());
}
