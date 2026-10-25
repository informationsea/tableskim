#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <tuple>

#include "cell_format.hxx"
#include "file_util.hxx"
#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    struct CopyGate
    {
        std::mutex mutex;
        std::condition_variable_any cv;
        bool released = false;
    };

    class CopySheet : public SheetSource
    {
    private:
        std::shared_ptr<CopyGate> gate_;
        const std::vector<TableRow> rows_{
            {std::string("alpha"), std::monostate{}}, {std::string("beta")},
            {std::string(), int64_t{2}}, {},
            {std::string("delta"), int64_t{4}}, {std::string("epsilon"), int64_t{5}},
            {std::string("zeta"), int64_t{6}}, {std::string("eta"), int64_t{7}}};

    public:
        explicit CopySheet(std::shared_ptr<CopyGate> gate) : gate_(std::move(gate)) {}
        SourceCapabilities capabilities() const override { return {.random_access = true}; }
        std::vector<ColumnInfo> columns() override { return {{"name"}, {"value"}}; }
        RowCount rowCount() override { return {rows_.size(), RowCount::Kind::Exact}; }
        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token stop) override
        {
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            if (first_row == 4)
            {
                std::unique_lock lock(gate_->mutex);
                if (!gate_->cv.wait(lock, stop, [&] { return gate_->released; }))
                    return block;
            }
            for (size_t r = first_row; r < std::min(rows_.size(), first_row + max_rows); ++r)
                block->rows.push_back(rows_[r]);
            return block;
        }
    };

    class CopyData : public DataSource
    {
    public:
        std::shared_ptr<CopyGate> gate = std::make_shared<CopyGate>();
        std::vector<std::string> sheetNames() override { return {"copy"}; }
        std::unique_ptr<SheetSource> openSheet(const std::string &) override
        {
            return std::make_unique<CopySheet>(gate);
        }
    };

    std::filesystem::path tempFile(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / ("tableskim-test-" + name);
    }

    std::shared_ptr<TableModel> openCsv(const std::filesystem::path &path, std::shared_ptr<TableBook> &book)
    {
        OpenOptions options;
        options.header = OpenOptions::Header::Yes;
        book = TableBook::open(path, options);
        auto model = book->sheet(book->sheetNames().front());
        REQUIRE(model);
        REQUIRE(waitFor([&]
                        { return model->state() != ModelState::Opening; }));
        model->requestRange(0, 10);
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        waitForCell(*model, 0, 0);
        return model;
    }
} // namespace

TEST_CASE("cellDisplayText flattens and truncates")
{
    CHECK(cellDisplayText(CellValue{std::string("a\nb\r\nc\td")}) == "a b  c d");
    CHECK(cellDisplayText(CellValue{int64_t{42}}) == "42");
    CHECK(cellDisplayText(CellValue{}) == "");

    // "あ" is 3 bytes; a cut inside a character backs off to its start.
    CHECK(cellDisplayText(CellValue{std::string("あいう")}, 4) == "あ…");
    CHECK(cellDisplayText(CellValue{std::string("abcdef")}, 3) == "abc…");
    CHECK(cellDisplayText(CellValue{std::string("abc")}, 3) == "abc");
}

TEST_CASE("isNumericCell")
{
    CHECK(isNumericCell(CellValue{int64_t{1}}));
    CHECK(isNumericCell(CellValue{2.5}));
    CHECK(isNumericCell(CellValue{std::string("3.25")}));
    CHECK_FALSE(isNumericCell(CellValue{std::string("x1")}));
    CHECK_FALSE(isNumericCell(CellValue{true}));
    CHECK_FALSE(isNumericCell(CellValue{}));
}

TEST_CASE("rangeToTsv and fittingColumnWidth")
{
    const auto path = tempFile("cell-format.csv");
    {
        std::ofstream out(path, std::ios::binary);
        out << "name,value,note\n"
               "alpha,1,x\n"
               "be,22,a much longer note\n";
    }
    std::shared_ptr<TableBook> book;
    auto model = openCsv(path, book);
    REQUIRE(cellText(*model, 1, 2) == "a much longer note");

    const TsvCopyResult copied = rangeToTsv(*model, 0, 0, 1, 1);
    CHECK(copied.text == "alpha\t1\nbe\t22\n");
    CHECK(copied.copied_rows == 2);
    CHECK(copied.skipped_rows == 0);
    CHECK(rangeToTsv(*model, 1, 2, 1, 2).text == "a much longer note");

    // Hidden columns are left out; one left over is copied as a single cell.
    HiddenColumns hidden;
    hidden.hide(1, 1, 3);
    CHECK(rangeToTsv(*model, 0, 0, 1, 2, &hidden).text == "alpha\tx\nbe\ta much longer note\n");
    hidden.hide(0, 0, 3);
    CHECK(rangeToTsv(*model, 1, 0, 1, 2, &hidden).text == "a much longer note");

    ColumnWidthMetrics metrics{
        .measure = [](const std::string &text)
        { return static_cast<int>(text.size()); },
        .header_padding = 2,
        .cell_padding = 1,
        .min_width = 3,
        .max_width = 10,
    };
    CHECK(fittingColumnWidth(*model, 0, metrics) == 6);  // "alpha" + 1 > "name" + 2
    CHECK(fittingColumnWidth(*model, 1, metrics) == 7);  // "value" + 2
    CHECK(fittingColumnWidth(*model, 2, metrics) == 10); // clamped
    metrics.sample_rows = 1;
    CHECK(fittingColumnWidth(*model, 2, metrics) == 6); // "note" + 2; row 1 is not sampled

    book.reset();
    std::filesystem::remove(path);
}

TEST_CASE("rangeToTsv preserves one cell and flattens multiple cells", "[regression]")
{
    const auto path = tempFile("copy-special-characters.csv");
    const std::string original = std::string(600, 'x') + "\t日本語\nnext\rlast\r\nend";
    const std::string flattened = std::string(600, 'x') + " 日本語 next last  end";
    {
        std::ofstream out(path, std::ios::binary);
        out << "a,b,c\n\"" << original << "\",plain,123\n,x\ty,\n";
    }
    std::shared_ptr<TableBook> book;
    auto model = openCsv(path, book);
    REQUIRE(cellText(*model, 0, 0) == original);
    const TsvCopyResult single = rangeToTsv(*model, 0, 0, 0, 0);
    CHECK(single.text == original);
    CHECK(single.copied_rows == 1);
    CHECK(single.skipped_rows == 0);
    CHECK(rangeToTsv(*model, 0, 2, 0, 2).text == "123");

    const TsvCopyResult empty = rangeToTsv(*model, 1, 0, 1, 0);
    CHECK(empty.text.empty());
    CHECK(empty.copied_rows == 1);
    CHECK(empty.skipped_rows == 0);

    CHECK(rangeToTsv(*model, 0, 0, 0, 2).text == flattened + "\tplain\t123\n");
    CHECK(rangeToTsv(*model, 0, 0, 1, 0).text == flattened + "\n\n");
    const TsvCopyResult multiple = rangeToTsv(*model, 0, 0, 1, 2);
    CHECK(multiple.text == flattened + "\tplain\t123\n\tx y\t\n");
    CHECK(multiple.copied_rows == 2);
    CHECK(multiple.skipped_rows == 0);

    book.reset();
    std::filesystem::remove(path);
}

TEST_CASE("rangeToTsv quotes cells that start with a quote, so that they paste back", "[regression]")
{
    const auto path = tempFile("copy-quotes.csv");
    {
        std::ofstream out(path, std::ios::binary);
        out << "a,b\n\"\"\"q\"\" x\",a\"b\n";
    }
    std::shared_ptr<TableBook> book;
    auto model = openCsv(path, book);
    REQUIRE(cellText(*model, 0, 0) == "\"q\" x");
    REQUIRE(cellText(*model, 0, 1) == "a\"b");
    const TsvCopyResult copied = rangeToTsv(*model, 0, 0, 0, 1);
    CHECK(copied.text == "\"\"\"q\"\" x\"\ta\"b\n");
    CHECK(parseTsv(copied.text) == std::vector<std::vector<std::string>>{{"\"q\" x", "a\"b"}});
    // One cell is still copied verbatim.
    CHECK(rangeToTsv(*model, 0, 0, 0, 0).text == "\"q\" x");

    book.reset();
    std::filesystem::remove(path);
}

TEST_CASE("rangeToTsv skips unavailable rows and preserves loaded empty cells", "[regression]")
{
    auto data = std::make_unique<CopyData>();
    auto gate = data->gate;
    TableBook book(std::move(data), {.block_rows = 2});
    auto model = book.sheet("copy");
    REQUIRE(waitForCell(*model, 3, 0).state == CellState::Empty);
    model->requestRange(6, 7);
    REQUIRE(waitForCell(*model, 7, 0).state == CellState::Ready);

    const TsvCopyResult partial = rangeToTsv(*model, 0, 0, 5, 1);
    CHECK(partial.text == "alpha\t\nbeta\t\n\t2\n\t\n");
    CHECK(partial.copied_rows == 4);
    CHECK(partial.skipped_rows == 2);

    const TsvCopyResult unavailable = rangeToTsv(*model, 4, 0, 5, 1);
    CHECK(unavailable.text.empty());
    CHECK(unavailable.copied_rows == 0);
    CHECK(unavailable.skipped_rows == 2);

    const TsvCopyResult gap = rangeToTsv(*model, 0, 0, 7, 1);
    CHECK(gap.text == "alpha\t\nbeta\t\n\t2\n\t\nzeta\t6\neta\t7\n");
    CHECK(gap.copied_rows == 6);
    CHECK(gap.skipped_rows == 2);

    {
        std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->cv.notify_all();
    REQUIRE(waitForCell(*model, 5, 0).state == CellState::Ready);
    const TsvCopyResult complete = rangeToTsv(*model, 0, 0, 5, 1);
    CHECK(complete.text == "alpha\t\nbeta\t\n\t2\n\t\ndelta\t4\nepsilon\t5\n");
    CHECK(complete.copied_rows == 6);
    CHECK(complete.skipped_rows == 0);
}

TEST_CASE("utf8Path and readFileStamp")
{
    const std::string name = utf8String(tempFile("stamp-表.csv"));
    const auto path = utf8Path(name);
    CHECK(utf8String(path) == name);

    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    std::filesystem::remove(path);
    CHECK_FALSE(readFileStamp(path, mtime, size));

    {
        std::ofstream out(path, std::ios::binary);
        out << "abc";
    }
    REQUIRE(readFileStamp(path, mtime, size));
    CHECK(size == 3);
    std::filesystem::remove(path);
}

TEST_CASE("nextSortCycle goes ascending, descending, then unsorted")
{
    using Sort = std::vector<SortKey>;
    CHECK(nextSortCycle({}, 2) == Sort{{2, true}});
    CHECK(nextSortCycle({{2, true}}, 2) == Sort{{2, false}});
    CHECK(nextSortCycle({{2, false}}, 2).empty());
    // Another column starts over and replaces the old sort.
    CHECK(nextSortCycle({{1, false}}, 2) == Sort{{2, true}});
}

TEST_CASE("row count texts")
{
    CHECK(rowCountText({1, RowCount::Kind::Exact}) == "1 row");
    CHECK(rowCountText({100, RowCount::Kind::Exact}) == "100 rows");
    CHECK(rowCountText({512, RowCount::Kind::AtLeast}) == "≥512 rows");
    CHECK(rowCountText({}) == "? rows");

    const RowCount rows{12, RowCount::Kind::Exact};
    CHECK(filteredRowCountText(rows, {100, RowCount::Kind::Exact}, false) == "12 rows");
    CHECK(filteredRowCountText(rows, {100, RowCount::Kind::Exact}, true) == "12 rows of 100");
    CHECK(filteredRowCountText(rows, {}, true) == "12 rows (filtered)");
}

TEST_CASE("filter operator helpers")
{
    for (const FilterOp op : FILTER_OPS)
    {
        CHECK(std::string(filterOpLabel(op)) != "?");
        CHECK(filterOpTakesValue(op) == (op != FilterOp::IsEmpty && op != FilterOp::IsNotEmpty &&
                                         op != FilterOp::IsNumber && op != FilterOp::IsDate));
    }
    CHECK(std::string(filterOpLabel(FilterOp::GreaterEqual)) == "≥");
    CHECK(std::string(filterOpLabel(FilterOp::Matches)) == "matches");
    CHECK(std::string(filterOpLabel(FilterOp::IsNumber)) == "is a number");
    CHECK(std::size(FILTER_OPS) == 14);
}

TEST_CASE("quick filters on a cell value")
{
    CHECK(cellFilterCondition(2, CellValue{std::string("Tokyo")}, true) ==
          FilterCondition{2, FilterOp::Equals, "Tokyo"});
    CHECK(cellFilterCondition(2, CellValue{1.5}, false) == FilterCondition{2, FilterOp::NotEquals, "1.5"});
    CHECK(cellFilterCondition(0, CellValue{}, true) == FilterCondition{0, FilterOp::IsEmpty, ""});
    CHECK(cellFilterCondition(0, CellValue{std::string()}, false) == FilterCondition{0, FilterOp::IsNotEmpty, ""});

    // The condition matches the cell it was made from.
    for (const CellValue &value : {CellValue{std::string("x")}, CellValue{int64_t{-3}}, CellValue{0.1},
                                   CellValue{true}, CellValue{Date{20366}}, CellValue{}})
    {
        CHECK(cellMatchesCondition(value, cellFilterCondition(0, value, true)));
        CHECK_FALSE(cellMatchesCondition(value, cellFilterCondition(0, value, false)));
    }
}

TEST_CASE("addFilterCondition, filtersColumn and filterSummary")
{
    ViewSpec spec{.sort = {{1, false}}};
    CHECK_FALSE(spec.filtered());
    addFilterCondition(spec, {1, FilterOp::Equals, "a"});
    REQUIRE(spec.filter);
    CHECK(spec.filter->combine == Filter::Combine::And);
    addFilterCondition(spec, {1, FilterOp::Equals, "a"});
    addFilterCondition(spec, {0, FilterOp::IsEmpty, ""});
    CHECK(spec.filter->conditions.size() == 2);
    CHECK(spec.sort == std::vector<SortKey>{{1, false}});
    CHECK(spec.filtered());
    CHECK(filtersColumn(spec, 0));
    CHECK(filtersColumn(spec, 1));
    CHECK_FALSE(filtersColumn(spec, 2));

    TableBook book(std::make_unique<CopyData>());
    auto model = book.sheet("copy");
    REQUIRE(waitFor([&] { return model->state() == ModelState::Ready; }));
    CHECK(filterSummary(*model, *spec.filter) == "value = \"a\" and name is empty");
    spec.filter->combine = Filter::Combine::Or;
    CHECK(filterSummary(*model, *spec.filter) == "value = \"a\" or name is empty");
}

TEST_CASE("memorySizeText")
{
    CHECK(memorySizeText(0) == "0 MB");
    CHECK(memorySizeText((size_t{100} << 20) + 12345) == "100 MB");
    CHECK(memorySizeText(size_t{500} << 20) == "500 MB");
    CHECK(memorySizeText(size_t{1} << 30) == "1.0 GB");
    CHECK(memorySizeText((size_t{2} << 30) + (size_t{100} << 20)) == "2.1 GB");
}

TEST_CASE("column statistics texts", "[stats]")
{
    CHECK(percentText(0, 0) == "0%");
    CHECK(percentText(1, 8) == "12.5%");
    CHECK(percentText(8, 8) == "100%");
    CHECK(percentText(0, 8) == "0%");
    CHECK(percentText(1, 3) == "33.3%");

    ColumnStats numbers;
    numbers.rows = 4;
    numbers.empty = 1;
    numbers.distinct = 2;
    numbers.numbers = 3;
    numbers.min = CellValue{std::string("1")};
    numbers.max = CellValue{std::string("4")};
    numbers.sum = 6.0;
    numbers.mean = 2.0;
    numbers.stddev = 1.5;
    CHECK(columnStatsLines(numbers) == std::vector<StatsLine>{
                                           {"Rows", "4"},
                                           {"Empty", "1 (25%)"},
                                           {"Distinct", "2"},
                                           {"Numbers", "3 (75%)"},
                                           {"Min", "1"},
                                           {"Max", "4"},
                                           {"Sum", "6"},
                                           {"Mean", "2"},
                                           {"Std dev", "1.5"},
                                       });

    // Text only, too many values to count: no number lines.
    ColumnStats text;
    text.rows = 10;
    text.distinct = 5;
    text.distinct_exact = false;
    text.min = CellValue{std::string("a")};
    text.max = CellValue{std::string("z")};
    CHECK(columnStatsLines(text) == std::vector<StatsLine>{
                                        {"Rows", "10"},
                                        {"Empty", "0 (0%)"},
                                        {"Distinct", "≥5"},
                                        {"Min", "a"},
                                        {"Max", "z"},
                                    });
}

TEST_CASE("parseGoTo", "[goto]")
{
    const std::vector<std::string> names = {"ID", "Name", "B", "Total sales"};
    const RowCount rows{100, RowCount::Kind::Exact};
    auto target = [&](std::string_view text, const std::vector<std::string> &header = {}) -> std::optional<GoToTarget>
    {
        const GoToResult result = parseGoTo(text, header, 30, rows);
        INFO(text << ": " << result.error);
        CHECK(result.target.has_value() == result.error.empty());
        return result.target;
    };
    auto error = [&](std::string_view text, const std::vector<std::string> &header = {})
    { return parseGoTo(text, header, 30, rows).error; };

    // Rows and cells are 1-based in the text.
    CHECK(target("12") == GoToTarget{11, std::nullopt});
    CHECK(target("  7 ") == GoToTarget{6, std::nullopt});
    CHECK(target("B12") == GoToTarget{11, 1});
    CHECK(target("ab3") == GoToTarget{2, 27});
    CHECK(target("R12C2") == GoToTarget{11, 1});
    CHECK(target("r1c30") == GoToTarget{0, 29});
    CHECK(target("C") == GoToTarget{std::nullopt, 2});
    CHECK(target("AD") == GoToTarget{std::nullopt, 29});

    // Header names, exactly or ignoring case, win over letters.
    CHECK(target("Name", names) == GoToTarget{std::nullopt, 1});
    CHECK(target("total SALES", names) == GoToTarget{std::nullopt, 3});
    CHECK(target("id", names) == GoToTarget{std::nullopt, 0});
    CHECK(target("B", names) == GoToTarget{std::nullopt, 2});
    CHECK(target("b", names) == GoToTarget{std::nullopt, 2});
    CHECK(target("C", names) == GoToTarget{std::nullopt, 2});
    CHECK(target("A5", names) == GoToTarget{4, 0});

    CHECK(error("") == "Enter a row number, a cell (B12 or R12C2) or a column");
    CHECK(error("0") == "Rows and columns are numbered from 1");
    CHECK(error("B0") == "Rows and columns are numbered from 1");
    CHECK(error("R0C1") == "Rows and columns are numbered from 1");
    CHECK(error("101") == "There are only 100 rows");
    CHECK(error("AE") == "There are only 30 columns");
    CHECK(error("Price", names) == "No row, cell or column \"Price\"");
    CHECK(error("1.5") == "No row, cell or column \"1.5\"");
    CHECK(error("99999999999999999999999") == "The number is too large");
    // While rows are still being counted, any row is accepted.
    CHECK(parseGoTo("5000", {}, 30, {512, RowCount::Kind::AtLeast}).target == GoToTarget{4999, std::nullopt});
}

TEST_CASE("HiddenColumns", "[hide]")
{
    HiddenColumns hidden;
    CHECK(hidden.empty());
    CHECK(hidden.hide(1, 2, 5) == 2);
    CHECK(hidden.contains(1));
    CHECK(hidden.contains(2));
    CHECK_FALSE(hidden.contains(3));
    CHECK(hidden.size() == 2);
    // Already hidden columns are not counted again; one column stays visible.
    CHECK(hidden.hide(0, 4, 5) == 2);
    CHECK(hidden.size() == 4);
    CHECK_FALSE(hidden.contains(4));
    CHECK(hidden.hidesAll(4, 4, 5));
    hidden.show(0);
    CHECK_FALSE(hidden.hidesAll(4, 4, 5));
    CHECK(hidden.hidesAll(0, 4, 5));
    // Columns past the end are ignored.
    CHECK(hidden.hide(7, 9, 5) == 0);

    // 0 visible, 1 2 3 hidden, 4 visible.
    CHECK(hidden.visibleIn(0, 4) == 2);
    CHECK(hidden.visibleIn(1, 3) == 0);
    CHECK(hidden.visibleIn(3, 6) == 3);
    CHECK(hidden.visibleIn(4, 2) == 0);
    CHECK(hidden.nearestVisible(0, 1, 5) == 0);
    CHECK(hidden.nearestVisible(2, 1, 5) == 4);
    CHECK(hidden.nearestVisible(2, -1, 5) == 0);
    CHECK(hidden.nearestVisible(9, 1, 5) == 4);
    hidden.show(4);
    hidden.hide(4, 4, 5);
    CHECK(hidden.contains(4));
    // At the end, it turns back.
    CHECK(hidden.nearestVisible(3, 1, 5) == 0);
    hidden.clear();
    CHECK(hidden.empty());
    CHECK(hidden.nearestVisible(3, 1, 5) == 3);
}

TEST_CASE("fileUri percent-encodes the path", "[regression]")
{
    // A ',' split the dbus-send array; '#', '%' and '?' broke the URI.
    CHECK(fileUri("/data/a b/c,d#e%f?.csv") == "file:///data/a%20b/c%2Cd%23e%25f%3F.csv");
    CHECK(fileUri("/tmp/x-1_2.~csv") == "file:///tmp/x-1_2.~csv");
    CHECK(fileUri("/表.csv") == "file:///%E8%A1%A8.csv");
}

TEST_CASE("parseTsv reads what spreadsheets copy")
{
    using Rows = std::vector<std::vector<std::string>>;
    CHECK(parseTsv("a\tb\n1\t2\n") == Rows{{"a", "b"}, {"1", "2"}});
    CHECK(parseTsv("a\tb\r\n1\t2\r\n") == Rows{{"a", "b"}, {"1", "2"}});
    CHECK(parseTsv("a\tb\n1\t2") == Rows{{"a", "b"}, {"1", "2"}});
    CHECK(parseTsv("x\n") == Rows{{"x"}});
    CHECK(parseTsv("a\t\n\tb\n") == Rows{{"a", ""}, {"", "b"}});
    // Quoted fields keep tabs, line breaks and quotes.
    CHECK(parseTsv("\"two\nlines\"\t\"a\"\"b\"\n") == Rows{{"two\nlines", "a\"b"}});
    // One cell as copied from one cell, line breaks and all.
    CHECK(parseTsv("one\nvalue") == Rows{{"one\nvalue"}});
    CHECK(parseTsv("\"quoted\"") == Rows{{"\"quoted\""}});
    CHECK(parseTsv("") == Rows{{""}});
}

TEST_CASE("pastedCells fills the selection from its top-left cell")
{
    using Cells = std::vector<TableModel::CellText>;
    auto cells = [](const Cells &list)
    {
        std::vector<std::tuple<size_t, size_t, std::string>> result;
        for (const auto &cell : list)
            result.emplace_back(cell.row, cell.column, cell.text);
        return result;
    };
    using Expected = std::vector<std::tuple<size_t, size_t, std::string>>;
    const std::vector<std::vector<std::string>> block = {{"a", "b"}, {"c", "d"}};
    CHECK(cells(pastedCells(block, 1, 1, 1, 1, 10, 5)) ==
          Expected{{1, 1, "a"}, {1, 2, "b"}, {2, 1, "c"}, {2, 2, "d"}});
    // Cut at the last row and column.
    CHECK(cells(pastedCells(block, 9, 4, 9, 4, 10, 5)) == Expected{{9, 4, "a"}});
    // Hidden columns are skipped.
    HiddenColumns hidden;
    hidden.hide(2, 2, 5);
    CHECK(cells(pastedCells(block, 0, 1, 0, 1, 10, 5, &hidden)) ==
          Expected{{0, 1, "a"}, {0, 3, "b"}, {1, 1, "c"}, {1, 3, "d"}});
    // One value fills the selection.
    CHECK(cells(pastedCells({{"x"}}, 0, 0, 1, 2, 10, 5, &hidden)) ==
          Expected{{0, 0, "x"}, {0, 1, "x"}, {1, 0, "x"}, {1, 1, "x"}});
    CHECK(pastedCells({}, 0, 0, 0, 0, 10, 5).empty());
}

TEST_CASE("formatCellAs: numbers with thousands separators and fixed decimals")
{
    using S = std::string;
    ColumnFormat thousands{.thousands = true};
    CHECK(formatCellAs(int64_t{1234567}, thousands) == "1,234,567");
    CHECK(formatCellAs(int64_t{-1234}, thousands) == "-1,234");
    CHECK(formatCellAs(int64_t{123}, thousands) == "123");
    CHECK(formatCellAs(1234567.25, thousands) == "1,234,567.25");
    CHECK(formatCellAs(1e20, thousands) == "1e+20");
    CHECK(formatCellAs(S(" +1234567.891 "), thousands) == "1,234,567.891");
    CHECK(formatCellAs(S("Tokyo"), thousands) == "Tokyo");
    CHECK(formatCellAs(true, thousands) == "TRUE");
    CHECK(formatCellAs(std::monostate{}, thousands).empty());

    ColumnFormat two{.decimals = 2};
    CHECK(formatCellAs(int64_t{5}, two) == "5.00");
    CHECK(formatCellAs(2.345678, two) == "2.35");
    CHECK(formatCellAs(S("1e3"), two) == "1000.00");
    CHECK(formatCellAs(-0.001, two) == "0.00");
    CHECK(formatCellAs(std::numeric_limits<double>::infinity(), two) == "inf");
    // Integers too large for a double keep their digits.
    CHECK(formatCellAs(int64_t{9007199254740993}, two) == "9007199254740993.00");
    CHECK(formatCellAs(2.5, ColumnFormat{.decimals = 0}) == "2");

    ColumnFormat both{.thousands = true, .decimals = 1};
    CHECK(formatCellAs(1234567.25, both) == "1,234,567.2");
    CHECK(formatCellAs(S("2024-01-31"), both) == "2024-01-31");
}

TEST_CASE("formatCellAs: date styles")
{
    using S = std::string;
    const Date date{19753}; // 2024-01-31
    CHECK(formatCellAs(date, {}) == "2024-01-31");
    CHECK(formatCellAs(date, {.date = DateStyle::Slash}) == "2024/01/31");
    CHECK(formatCellAs(date, {.date = DateStyle::MonthDayYear}) == "01/31/2024");
    CHECK(formatCellAs(date, {.date = DateStyle::DayMonthYear}) == "31/01/2024");
    CHECK(formatCellAs(date, {.date = DateStyle::Japanese}) == "2024年1月31日");
    CHECK(formatCellAs(date, {.date = DateStyle::MonthName}) == "Jan 31, 2024");
    for (const DateStyle style : DATE_STYLES)
        CHECK(dateStyleExample(style) == formatCellAs(date, {.date = style}));

    // The time of day stays as it is.
    const DateTime noon{(19753LL * 86400 + 12 * 3600 + 5) * 1000000 + 250000};
    CHECK(formatCellAs(noon, {.date = DateStyle::Slash}) == "2024/01/31 12:00:05.250");
    const DateTime before_1970{-1000000}; // 1969-12-31 23:59:59
    CHECK(formatCellAs(before_1970, {.date = DateStyle::Japanese}) == "1969年12月31日 23:59:59");
    // ISO text too; other text and numbers are left alone.
    CHECK(formatCellAs(S("2024-01-31"), {.date = DateStyle::MonthDayYear}) == "01/31/2024");
    CHECK(formatCellAs(S("2024-01-31T08:30"), {.date = DateStyle::Slash}) == "2024/01/31 08:30:00");
    CHECK(formatCellAs(S("31 Jan"), {.date = DateStyle::Slash}) == "31 Jan");
    CHECK(formatCellAs(int64_t{2024}, {.date = DateStyle::Slash}) == "2024");
}

TEST_CASE("ColumnFormats and cellDisplayText with a format")
{
    ColumnFormats formats;
    CHECK(formats.empty());
    CHECK(formats.find(2) == nullptr);
    formats.set(2, {.thousands = true});
    REQUIRE(formats.find(2) != nullptr);
    CHECK(formats.get(2).thousands);
    CHECK(formats.get(3).isDefault());
    formats.set(2, {});
    CHECK(formats.empty());

    const ColumnFormat format{.thousands = true};
    CHECK(cellDisplayText(int64_t{1234}, 256, &format) == "1,234");
    CHECK(cellDisplayText(int64_t{1234}) == "1234");
    CHECK(cellDisplayText(int64_t{1234567}, 3, &format) == "1,2…");
}

TEST_CASE("formatCell: numbers of everyday size without an exponent")
{
    CHECK(formatCell(500000.0) == "500000");
    CHECK(formatCell(1e6) == "1000000");
    CHECK(formatCell(0.0001) == "0.0001");
    CHECK(formatCell(1234.5) == "1234.5");
    CHECK(formatCell(-2e10) == "-20000000000");
    CHECK(formatCell(1e20) == "1e+20");
    CHECK(formatCell(1.5e-7) == "1.5e-07");
}
