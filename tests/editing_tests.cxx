#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>

#include <OpenXLSX.hpp>

#include "compress.hxx"
#include "decompress.hxx"
#include "file_util.hxx"
#include "table.hxx"
#include "test_helpers.hxx"
#include "text_encoding.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    std::filesystem::path tempDirectory()
    {
        const auto directory = std::filesystem::temp_directory_path() / "tableskim-test-editing";
        std::filesystem::create_directories(directory);
        return directory;
    }

    // A copy of a demo file to edit.
    std::filesystem::path copyDemo(const std::filesystem::path &name)
    {
        const auto path = tempDirectory() / name;
        std::filesystem::copy_file(kDemoData / name, path, std::filesystem::copy_options::overwrite_existing);
        return path;
    }

    std::filesystem::path writeFile(const std::string &name, const std::string &contents)
    {
        const auto path = tempDirectory() / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
        return path;
    }

    std::string readBytes(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::string decompressedBytes(const std::filesystem::path &path)
    {
        const Compression compression = detectCompression(path);
        if (compression == Compression::None)
            return readBytes(path);
        auto in = openDecompressingStream(path, compression);
        std::ostringstream text;
        text << in->rdbuf();
        return text.str();
    }

    std::shared_ptr<TableModel> openSheet(TableBook &book, const std::string &name = {})
    {
        auto model = book.sheet(name.empty() ? book.sheetNames().front() : name);
        REQUIRE(model);
        REQUIRE(waitFor([&]
                        { return model->state() != ModelState::Opening; }));
        INFO(model->errorMessage());
        REQUIRE(model->state() == ModelState::Ready);
        return model;
    }

    void readAll(TableModel &model)
    {
        model.requestRange(0, 1000000);
        REQUIRE(waitFor([&]
                        {
            const RowCount rows = model.rowCount();
            return rows.isExact() && (rows.value == 0 || model.peek(rows.value - 1, 0).state != CellState::Loading); }));
    }

    TableBook::SaveResult save(TableBook &book, const std::filesystem::path &target)
    {
        std::promise<TableBook::SaveResult> promise;
        auto future = promise.get_future();
        book.saveEdits(target, [&](const TableBook::SaveResult &result)
                       { promise.set_value(result); });
        REQUIRE(future.wait_for(std::chrono::seconds(60)) == std::future_status::ready);
        return future.get();
    }

    // Saves over the file, as the UIs do: the book is closed first.
    void saveInPlace(std::shared_ptr<TableBook> &book, const std::filesystem::path &path)
    {
        const TableBook::SaveResult result = save(*book, path);
        INFO(result.message);
        REQUIRE(result.ok);
        book.reset();
        commitSave(result.written, path);
    }

    bool setText(TableModel &model, size_t row, size_t column, const std::string &text)
    {
        return model.setCells({{row, column, text}}) == 1;
    }
} // namespace

TEST_CASE("editedCellValue keeps the type of the cell")
{
    CHECK(editedCellValue("42", CellValue{std::string("x")}, CellEditKind::Text) == CellValue{std::string("42")});
    CHECK(editedCellValue("", CellValue{std::string("x")}, CellEditKind::Text) == CellValue{std::string()});

    CHECK(editedCellValue("42", CellValue{int64_t{1}}, CellEditKind::Typed) == CellValue{int64_t{42}});
    CHECK(editedCellValue(" -7 ", CellValue{}, CellEditKind::Typed) == CellValue{int64_t{-7}});
    CHECK(editedCellValue("2.5", CellValue{int64_t{1}}, CellEditKind::Typed) == CellValue{2.5});
    CHECK(editedCellValue("1e3", CellValue{}, CellEditKind::Typed) == CellValue{1000.0});
    CHECK(editedCellValue("inf", CellValue{}, CellEditKind::Typed) == CellValue{std::string("inf")});
    CHECK(editedCellValue("abc", CellValue{1.5}, CellEditKind::Typed) == CellValue{std::string("abc")});
    CHECK(editedCellValue("", CellValue{1.5}, CellEditKind::Typed) == CellValue{});
    // Text stays text, so that "007" keeps its zeros.
    CHECK(editedCellValue("007", CellValue{std::string("x")}, CellEditKind::Typed) == CellValue{std::string("007")});
    CHECK(editedCellValue("false", CellValue{true}, CellEditKind::Typed) == CellValue{false});
    CHECK(editedCellValue("maybe", CellValue{true}, CellEditKind::Typed) == CellValue{std::string("maybe")});
    CHECK(editedCellValue("2024-02-29", CellValue{Date{0}}, CellEditKind::Typed) == CellValue{Date{19782}});
    CHECK(editedCellValue("1969-12-31", CellValue{Date{0}}, CellEditKind::Typed) == CellValue{Date{-1}});
    CHECK(editedCellValue("1970-01-02 00:00:01", CellValue{Date{0}}, CellEditKind::Typed) ==
          CellValue{DateTime{86401000000}});
    CHECK(editedCellValue("1970-01-02", CellValue{DateTime{0}}, CellEditKind::Typed) == CellValue{DateTime{86400000000}});
    CHECK(editedCellValue("02/29", CellValue{Date{0}}, CellEditKind::Typed) == CellValue{std::string("02/29")});
}

TEST_CASE("Editing: only in edit mode, with undo and redo")
{
    auto book = TableBook::open(copyDemo("normal.csv"));
    REQUIRE(book->supportsEditing());
    auto model = openSheet(*book);
    REQUIRE(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK(model->editKind() == CellEditKind::Text);

    // Off at first.
    CHECK_FALSE(book->editing());
    CHECK_FALSE(model->cellEditable(0, 1));
    CHECK_FALSE(setText(*model, 0, 1, "Changed"));
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");

    book->setEditing(true);
    CHECK(book->editing());
    CHECK(model->cellEditable(0, 1));
    CHECK_FALSE(model->cellEditable(0, 100)); // past the last column
    CHECK_FALSE(model->modified());
    REQUIRE(setText(*model, 0, 1, "Changed"));
    CHECK(cellText(*model, 0, 1) == "Changed");
    CHECK(model->modified());
    CHECK(book->modified());
    CHECK(model->cellModified(0, 1));
    CHECK_FALSE(model->cellModified(0, 2));
    // The same text again changes nothing.
    CHECK_FALSE(setText(*model, 0, 1, "Changed"));

    // Several cells are one step.
    REQUIRE(model->setCells({{1, 1, "A"}, {1, 2, "B"}, {1, 1, "C"}}) == 3);
    CHECK(cellText(*model, 1, 1) == "C");
    CHECK(cellText(*model, 1, 2) == "B");
    CellPosition cell;
    REQUIRE(model->undo(&cell));
    CHECK(cell == CellPosition{1, 1});
    CHECK(cellText(*model, 1, 1) == "Hanako Suzuki");
    CHECK(cellText(*model, 1, 2) == "hanako.suzuki2@example.com");
    REQUIRE(model->undo());
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK_FALSE(model->modified());
    CHECK_FALSE(model->cellModified(0, 1));
    CHECK_FALSE(model->undo());
    REQUIRE(model->redo());
    CHECK(cellText(*model, 0, 1) == "Changed");
    CHECK(model->canRedo());

    // A new edit drops what could be redone.
    REQUIRE(setText(*model, 2, 1, "Other"));
    CHECK_FALSE(model->canRedo());

    // Out of edit mode, nothing changes, but the edits stay.
    book->setEditing(false);
    CHECK_FALSE(model->canUndo());
    CHECK_FALSE(model->undo());
    CHECK_FALSE(setText(*model, 3, 1, "No"));
    CHECK(model->modified());

    book->discardEdits();
    CHECK_FALSE(book->modified());
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK(cellText(*model, 2, 1) == "Carol Wilson");
    book->setEditing(true);
    CHECK_FALSE(model->canUndo());
}

TEST_CASE("Editing: sheets opened later are in edit mode too")
{
    auto book = TableBook::open(copyDemo("multisheet.xlsx"));
    book->setEditing(true);
    for (const auto &name : book->sheetNames())
    {
        auto model = openSheet(*book, name);
        INFO(name);
        readAll(*model);
        if (model->rowCount().value > 0 && model->columnCount() > 0)
            CHECK(model->cellEditable(0, model->columnCount() - 1));
    }
}

TEST_CASE("Editing: a file that cannot be edited stays out of edit mode")
{
    auto book = TableBook::open(kDemoData / "normal.parquet");
    CHECK_FALSE(book->supportsEditing());
    book->setEditing(true);
    CHECK_FALSE(book->editing());
    auto model = openSheet(*book);
    REQUIRE(cellText(*model, 0, 0) == "1");
    CHECK(model->editKind() == CellEditKind::None);
    CHECK_FALSE(model->cellEditable(0, 0));
    CHECK(model->setCells({{0, 0, "2"}}) == 0);
}

TEST_CASE("Editing: cells read before an edit keep their value")
{
    auto book = TableBook::open(copyDemo("normal.csv"));
    book->setEditing(true);
    auto model = openSheet(*book);
    const CellRef before = waitForCell(*model, 0, 1);
    REQUIRE(before.state == CellState::Ready);
    REQUIRE(setText(*model, 0, 1, "Changed"));
    CHECK(formatCell(*before.value) == "Sakura Brown");
    CHECK(cellText(*model, 0, 1) == "Changed");
}

TEST_CASE("Editing: the view is applied again only when asked")
{
    auto book = TableBook::open(copyDemo("normal.csv"));
    book->setEditing(true);
    auto model = openSheet(*book);
    ViewSpec spec;
    spec.filter = Filter{};
    spec.filter->conditions.push_back({4, FilterOp::Equals, "Osaka"});
    model->setView(spec);
    REQUIRE(waitFor([&]
                    { return model->view() == spec; }));
    const size_t rows = model->rowCount().value;
    REQUIRE(rows > 1);
    REQUIRE(cellText(*model, 0, 4) == "Osaka");

    REQUIRE(setText(*model, 0, 4, "Kyoto"));
    CHECK(model->rowCount().value == rows);
    CHECK(cellText(*model, 0, 4) == "Kyoto");
    CHECK(model->cellModified(0, 4));

    // Applied again, the filter leaves the row out.
    model->setView({});
    REQUIRE(waitFor([&]
                    { return model->view().empty(); }));
    model->setView(spec);
    REQUIRE(waitFor([&]
                    { return model->view() == spec; }));
    CHECK(model->rowCount().value == rows - 1);

    // Undo finds the row, which is not in the view, by its source row.
    CellPosition cell{99, 99};
    REQUIRE(model->undo(&cell));
    CHECK(cell == CellPosition{99, 99});
}

TEST_CASE("Editing: statistics and searches start over")
{
    auto book = TableBook::open(copyDemo("normal.csv"));
    book->setEditing(true);
    auto model = openSheet(*book);
    readAll(*model);
    model->startColumnStats(4);
    REQUIRE(waitFor([&]
                    { return model->columnStatsState().done; }));
    model->startSearch({.text = "Kyoto"});
    REQUIRE(waitFor([&]
                    { return model->searchState().done; }));
    const size_t hits = model->searchState().hits.size();
    const uint64_t stats_generation = model->columnStatsState().generation;

    std::atomic<int> changed{0};
    auto subscription = model->subscribe([&](const TableEvent &event)
                                         {
        if (event.kind == TableEvent::Kind::CellsChanged)
            ++changed; });
    const std::string old_city = cellText(*model, 0, 4);
    REQUIRE(old_city != "Kyoto");
    REQUIRE(setText(*model, 0, 4, "Kyoto"));
    REQUIRE(waitFor([&]
                    { return changed > 0; }));
    REQUIRE(waitFor([&]
                    { return model->searchState().done && model->columnStatsState().done; }));
    CHECK(model->searchState().hits.size() == hits + 1);
    CHECK(model->columnStatsState().generation != stats_generation);
}

TEST_CASE("Editing: ragged rows get longer and back")
{
    auto book = TableBook::open(copyDemo("ragged.csv"));
    book->setEditing(true);
    auto model = openSheet(*book);
    REQUIRE(cellText(*model, 1, 0) == "r2c1");
    REQUIRE(model->peek(1, 4).state == CellState::Empty);
    REQUIRE(model->cellEditable(1, 4));
    REQUIRE(setText(*model, 1, 4, "new"));
    CHECK(cellText(*model, 1, 4) == "new");
    CHECK(model->peek(1, 3).state == CellState::Ready);
    // Empty text in a missing cell is no change.
    CHECK_FALSE(setText(*model, 1, 3, ""));
    REQUIRE(model->undo());
    CHECK(model->peek(1, 4).state == CellState::Empty);
    CHECK(model->peek(1, 3).state == CellState::Empty);
    CHECK_FALSE(model->modified());
}

TEST_CASE("Saving without edits keeps every byte")
{
    const auto name = GENERATE(as<std::string>{}, "normal.csv", "normal.csv.gz", "normal.csv.bz2", "normal.csv.xz",
                               "normal.csv.zst", "normal-cp932.csv", "normal-cp932.csv.gz", "normal-utf16be.csv",
                               "normal-utf16le.csv", "normal-utf16le-bom.tsv", "normal-utf8-bom.csv", "ragged.csv",
                               "dates.csv", "cp932-sample.csv", "large.tsv.gz", "多言語サンプル.csv");
    INFO(name);
    const auto path = copyDemo(std::filesystem::path(std::u8string(name.begin(), name.end())));
    auto book = TableBook::open(path);
    auto model = openSheet(*book);
    readAll(*model);
    const auto target = tempDirectory() / "saved";
    const TableBook::SaveResult result = save(*book, target);
    INFO(result.message);
    REQUIRE(result.ok);
    CHECK(result.written.parent_path() == target.parent_path());
    CHECK(decompressedBytes(result.written) == decompressedBytes(path));
    CHECK(detectCompression(result.written) == detectCompression(path));
    if (detectCompression(path) == Compression::None)
        CHECK(readBytes(result.written) == readBytes(path));
    std::filesystem::remove(result.written);
}

TEST_CASE("Saving an edit rewrites only its record")
{
    const auto name = GENERATE(as<std::string>{}, "normal.csv", "normal.csv.gz", "normal.csv.bz2", "normal.csv.xz",
                               "normal.csv.zst", "normal-cp932.csv", "normal-utf16be.csv", "normal-utf16le-bom.tsv",
                               "normal-utf8-bom.csv");
    INFO(name);
    const auto path = copyDemo(name);
    const std::string before = decompressedBytes(path);
    std::shared_ptr<TableBook> book = TableBook::open(path);
    auto model = openSheet(*book);
    const TextEncoding encoding = *book->textEncoding();
    const std::string old_name = cellText(*model, 2, 1);
    book->setEditing(true);
    const std::string value = "名前, \"引用\"";
    REQUIRE(setText(*model, 2, 1, value));
    saveInPlace(book, path);

    // Everything else reads as before.
    std::shared_ptr<TableBook> reopened = TableBook::open(path);
    auto saved = openSheet(*reopened);
    CHECK(reopened->textEncoding() == encoding);
    CHECK(cellText(*saved, 2, 1) == value);
    CHECK(cellText(*saved, 2, 2) == "carol.wilson3@example.com");
    CHECK(cellText(*saved, 1, 1) == "Hanako Suzuki");
    readAll(*saved);
    CHECK(saved->rowCount().value == 54);

    // Byte for byte, only the record changed.
    const std::string after = decompressedBytes(path);
    const std::string old_text = *encodeText(old_name, encoding);
    const size_t at = before.find(old_text);
    REQUIRE(at != std::string::npos);
    // Quoted for the comma and the quotes in it.
    const auto quoted = encodeText("\"名前, \"\"引用\"\"\"", encoding);
    REQUIRE(quoted);
    CHECK(after == before.substr(0, at) + *quoted + before.substr(at + old_text.size()));
}

TEST_CASE("Saving keeps quoted line breaks, CRLF, empty lines and ## lines")
{
    const std::string contents = "##meta,\"x\n"
                                 "name,note\r\n"
                                 "a,\"two\nlines\"\r\n"
                                 "\r\n"
                                 "##between\r\n"
                                 "b,  spaced \r\n"
                                 "c,\"q\"\"uote\"\r\n"
                                 "d,last";
    const auto path = writeFile("layout.csv", contents);
    OpenOptions options;
    options.skip_meta_lines = true;
    std::shared_ptr<TableBook> book = TableBook::open(path, options);
    auto model = openSheet(*book);
    readAll(*model);
    REQUIRE(model->rowCount().value == 5);
    REQUIRE(model->columnName(1) == "note");
    REQUIRE(cellText(*model, 0, 1) == "two\nlines");
    REQUIRE(cellText(*model, 3, 1) == "q\"uote");
    book->setEditing(true);
    REQUIRE(setText(*model, 2, 1, "x,y"));
    REQUIRE(setText(*model, 4, 0, "e"));
    saveInPlace(book, path);
    CHECK(readBytes(path) == "##meta,\"x\n"
                             "name,note\r\n"
                             "a,\"two\nlines\"\r\n"
                             "\r\n"
                             "##between\r\n"
                             "b,\"x,y\"\r\n"
                             "c,\"q\"\"uote\"\r\n"
                             "e,last");
}

TEST_CASE("Saving refuses what it cannot write")
{
    SECTION("a character that CP932 lacks")
    {
        const auto path = copyDemo("normal-cp932.csv");
        auto book = TableBook::open(path);
        auto model = openSheet(*book);
        REQUIRE(cellText(*model, 0, 1) == "Sakura Brown");
        book->setEditing(true);
        REQUIRE(setText(*model, 0, 1, "😀"));
        const TableBook::SaveResult result = save(*book, path);
        CHECK_FALSE(result.ok);
        CHECK(result.message.find("Shift_JIS") != std::string::npos);
        // Nothing is left behind.
        for (const auto &entry : std::filesystem::directory_iterator(tempDirectory()))
            CHECK(utf8String(entry.path().filename()).find("tableskim-save") == std::string::npos);
    }
    SECTION("a file changed since it was opened")
    {
        const auto path = writeFile("changed.csv", "a,b\n1,2\n");
        auto book = TableBook::open(path);
        auto model = openSheet(*book);
        book->setEditing(true);
        REQUIRE(setText(*model, 0, 0, "3"));
        {
            std::ofstream out(path, std::ios::binary | std::ios::app);
            out << "4,5\n";
        }
        const TableBook::SaveResult result = save(*book, path);
        CHECK_FALSE(result.ok);
        CHECK(result.message.find("changed") != std::string::npos);
    }
    SECTION("a line that would read as ## line")
    {
        const auto path = writeFile("meta.vcf", "##fileformat=VCFv4.2\n#CHROM\tPOS\nchr1\t1\n");
        auto book = TableBook::open(path);
        auto model = openSheet(*book);
        book->setEditing(true);
        REQUIRE(setText(*model, 0, 0, "x\n##y"));
        const TableBook::SaveResult result = save(*book, path);
        CHECK_FALSE(result.ok);
    }
}

TEST_CASE("Saving keeps BGZF compression")
{
    const auto path = tempDirectory() / "blocks.tsv.gz";
    {
        auto writer = openCompressedWriter(path, Compression::Gzip, true);
        writer->write("a\tb\n");
        for (int i = 0; i < 20000; ++i)
            writer->write(std::to_string(i) + "\tvalue " + std::to_string(i * 7) + "\n");
        writer->finish();
    }
    REQUIRE(isBgzf(path));
    std::shared_ptr<TableBook> book = TableBook::open(path);
    auto model = openSheet(*book);
    REQUIRE(cellText(*model, 5, 1) == "value 35");
    book->setEditing(true);
    REQUIRE(setText(*model, 5, 1, "edited"));
    saveInPlace(book, path);
    CHECK(isBgzf(path));
    auto reopened = TableBook::open(path);
    auto saved = openSheet(*reopened);
    CHECK(cellText(*saved, 5, 1) == "edited");
    readAll(*saved);
    CHECK(saved->rowCount().value == 20000);
    CHECK(cellText(*saved, 19999, 1) == "value 139993");
}

TEST_CASE("Saving Excel cells keeps the rest of the workbook")
{
    const auto path = copyDemo("multisheet.xlsx");
    std::shared_ptr<TableBook> book = TableBook::open(path);
    const auto names = book->sheetNames();
    REQUIRE(names.size() > 1);
    auto first = openSheet(*book, names[0]);
    auto second = openSheet(*book, names[1]);
    readAll(*first);
    readAll(*second);
    const std::string untouched = cellText(*second, 0, 0);
    const std::string neighbour = cellText(*first, 0, 1);
    book->setEditing(true);
    CHECK(first->editKind() == CellEditKind::Typed);
    REQUIRE(setText(*first, 0, 0, "edited text"));
    saveInPlace(book, path);

    auto reopened = TableBook::open(path);
    CHECK(reopened->sheetNames() == names);
    CHECK(cellText(*openSheet(*reopened, names[0]), 0, 0) == "edited text");
    CHECK(cellText(*openSheet(*reopened, names[0]), 0, 1) == neighbour);
    CHECK(cellText(*openSheet(*reopened, names[1]), 0, 0) == untouched);
}

TEST_CASE("Saving Excel cells keeps carriage returns")
{
    const auto path = copyDemo("normal.xlsx");
    std::shared_ptr<TableBook> book = TableBook::open(path);
    auto model = openSheet(*book);
    readAll(*model);
    book->setEditing(true);
    REQUIRE(setText(*model, 0, 0, "a\r\nb\rc"));
    saveInPlace(book, path);

    auto reopened = TableBook::open(path);
    CHECK(*waitForCell(*openSheet(*reopened), 0, 0).value == CellValue{std::string("a\r\nb\rc")});
}

TEST_CASE("Excel: typed edits, locked cells and merged ranges")
{
    const auto path = tempDirectory() / "typed.xlsx";
    {
        OpenXLSX::XLDocument doc;
        doc.create(path.string(), true);
        auto &styles = doc.styles();
        const auto date_style = styles.cellFormats().create();
        styles.cellFormats()[date_style].setNumberFormatId(14);
        const auto time_format = styles.numberFormats().create();
        styles.numberFormats()[time_format].setNumberFormatId(170);
        styles.numberFormats()[time_format].setFormatCode("hh:mm:ss");
        const auto time_style = styles.cellFormats().create();
        styles.cellFormats()[time_style].setNumberFormatId(170);

        auto sheet = doc.workbook().worksheet("Sheet1");
        for (const char *ref : {"A1", "B1", "C1", "D1", "E1", "F1"})
            sheet.cell(ref).value() = std::string("h") + ref;
        sheet.cell("A2").value() = 1;
        sheet.cell("B2").value() = 45000;
        sheet.cell("B2").setCellFormat(date_style);
        sheet.cell("C2").value() = 0.5;
        sheet.cell("C2").setCellFormat(time_style);
        sheet.cell("D2").formula() = "A2*2";
        sheet.cell("D2").value() = 2;
        sheet.cell("E2").value() = "text";
        sheet.cell("F2").value() = true;
        sheet.cell("A3").value() = "merged";
        sheet.mergeCells("A3:B3");
        doc.save();
    }
    std::shared_ptr<TableBook> book = TableBook::open(path);
    auto model = openSheet(*book);
    readAll(*model);
    book->setEditing(true);
    CHECK(model->cellEditable(0, 0));
    CHECK(model->cellEditable(0, 1));
    CHECK_FALSE(model->cellEditable(0, 2)); // time shown as text
    CHECK_FALSE(model->cellEditable(0, 3)); // formula
    CHECK(model->cellEditable(1, 0));
    CHECK_FALSE(model->cellEditable(1, 1)); // covered by the merge
    CHECK(model->setCells({{0, 3, "9"}}) == 0);

    REQUIRE(model->setCells({{0, 0, "41"}, {0, 1, "2024-02-29"}, {0, 4, "123"}, {0, 5, "no"}, {1, 2, "2.5"}}) == 5);
    saveInPlace(book, path);

    auto reopened = TableBook::open(path);
    auto saved = openSheet(*reopened);
    const auto value = [&](size_t row, size_t column)
    {
        return *waitForCell(*saved, row, column).value;
    };
    CHECK(value(0, 0) == CellValue{int64_t{41}});
    CHECK(value(0, 1) == CellValue{Date{19782}});
    CHECK(value(0, 4) == CellValue{std::string("123")});
    CHECK(value(0, 5) == CellValue{false});
    CHECK(value(1, 2) == CellValue{2.5});
    CHECK(cellText(*saved, 0, 2) == "12:00:00");
    CHECK(cellText(*saved, 0, 3) == "2");

    OpenXLSX::XLDocument doc;
    doc.open(path.string());
    CHECK(doc.workbook().worksheet("Sheet1").cell("D2").hasFormula());
    // Excel computes the formula again when it opens the file, but not on every change.
    std::ostringstream workbook;
    doc.workbook().print(workbook);
    CHECK(workbook.str().find("fullCalcOnLoad=\"1\"") != std::string::npos);
    CHECK(workbook.str().find("forceFullCalc") == std::string::npos);
}
