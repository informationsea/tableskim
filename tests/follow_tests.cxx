#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    class TempFile
    {
    public:
        explicit TempFile(const std::string &name) : path_(std::filesystem::temp_directory_path() / name) {}
        ~TempFile() { std::filesystem::remove(path_); }
        const std::filesystem::path &path() const { return path_; }
        void write(const std::string &text, bool append = false) const
        {
            std::ofstream out(path_, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
            out << text;
        }

    private:
        std::filesystem::path path_;
    };

    AppendResult readAppended(TableBook &book, bool *caught_up = nullptr)
    {
        std::promise<std::pair<AppendResult, bool>> promise;
        auto future = promise.get_future();
        book.readAppended([&promise](AppendResult result, bool up_to_date)
                          { promise.set_value({result, up_to_date}); });
        const auto [result, up_to_date] = future.get();
        if (caught_up)
            *caught_up = up_to_date;
        return result;
    }

    std::shared_ptr<TableModel> openRead(std::shared_ptr<TableBook> &book, const std::filesystem::path &path,
                                         size_t block_rows = 512)
    {
        OpenOptions options;
        options.header = OpenOptions::Header::Yes;
        TableModel::Options model_options;
        model_options.block_rows = block_rows;
        book = TableBook::open(path, options, BackendRegistry::builtin(), model_options);
        auto model = book->sheet(book->sheetNames().front());
        REQUIRE(waitFor([&]
                        { return model->state() == ModelState::Ready; }));
        model->requestRange(0, 1000);
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        return model;
    }

    size_t exactRows(TableModel &model, size_t expected)
    {
        waitFor([&]
                { return model.rowCount().isExact() && model.rowCount().value == expected; });
        return model.rowCount().value;
    }
} // namespace

TEST_CASE("readAppended: rows appended to a CSV file are added")
{
    TempFile file("tableskim-test-follow.csv");
    file.write("n,name\n1,a\n2,b\n3,c\n");
    std::shared_ptr<TableBook> book;
    auto model = openRead(book, file.path(), 2);
    CHECK(book->readsAppendedRows());
    CHECK(model->rowCount().value == 3);
    CHECK(readAppended(*book) == AppendResult::None);

    // Into the partly filled last block, and on into new ones.
    file.write("4,d\n5,e\n6,f,extra\n", true);
    CHECK(readAppended(*book) == AppendResult::Appended);
    CHECK(exactRows(*model, 6) == 6);
    CHECK(cellText(*model, 2, 1) == "c");
    CHECK(cellText(*model, 3, 1) == "d");
    CHECK(cellText(*model, 5, 2) == "extra");
    CHECK(model->columnCount() == 3);

    SECTION("a row being written waits for its line break")
    {
        file.write("7,g", true);
        CHECK(readAppended(*book) == AppendResult::None);
        file.write("h\n8,\"two\nlines\"\n", true);
        CHECK(readAppended(*book) == AppendResult::Appended);
        CHECK(exactRows(*model, 8) == 8);
        CHECK(cellText(*model, 6, 1) == "gh");
        CHECK(cellText(*model, 7, 1) == "two\nlines");
    }
    SECTION("a sorted and filtered sheet applies its view again")
    {
        const ViewSpec spec{.sort = {{0, false}}, .filter = Filter{Filter::Combine::And, {{0, FilterOp::Greater, "2"}}}};
        model->setView(spec);
        REQUIRE(waitFor([&]
                        { return model->view() == spec; }));
        CHECK(model->rowCount().value == 4);
        file.write("9,i\n", true);
        CHECK(readAppended(*book) == AppendResult::Appended);
        REQUIRE(waitFor([&]
                        { return model->rowCount().value == 5; }));
        CHECK(cellText(*model, 0, 1) == "i");
        CHECK(model->view() == spec);
    }
    SECTION("a file written anew is read again")
    {
        file.write("n,name\n1,x\n2,y\n3,z\n4,w\n5,v\n6,u\n7,t\n");
        CHECK(readAppended(*book) == AppendResult::Changed);
    }
    SECTION("a shorter file is read again")
    {
        file.write("n,name\n1,a\n");
        CHECK(readAppended(*book) == AppendResult::Changed);
    }
}

TEST_CASE("readAppended: a last row without a line break is read again with the rest")
{
    TempFile file("tableskim-test-follow-no-newline.csv");
    file.write("n\n1\n2");
    std::shared_ptr<TableBook> book;
    auto model = openRead(book, file.path());
    CHECK(model->rowCount().value == 2);
    file.write("3\n", true);
    CHECK(readAppended(*book) == AppendResult::Changed);
}

TEST_CASE("readAppended: VCF meta lines and Shift_JIS")
{
    SECTION("## lines are skipped")
    {
        TempFile file("tableskim-test-follow.vcf");
        file.write("##meta\n#CHROM\tPOS\nchr1\t1\n");
        std::shared_ptr<TableBook> book;
        auto model = openRead(book, file.path());
        CHECK(model->rowCount().value == 1);
        file.write("##more\nchr2\t2\n", true);
        CHECK(readAppended(*book) == AppendResult::Appended);
        CHECK(exactRows(*model, 2) == 2);
        CHECK(cellText(*model, 1, 0) == "chr2");
    }
    SECTION("Shift_JIS text is decoded")
    {
        TempFile file("tableskim-test-follow-cp932.csv");
        // "名前" and "東京" in CP932.
        file.write("id,\x96\xbc\x91\x4f\n1,a\n");
        OpenOptions options;
        options.header = OpenOptions::Header::Yes;
        options.encoding = TextEncoding::Cp932;
        auto book = TableBook::open(file.path(), options);
        auto model = book->sheet(book->sheetNames().front());
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        file.write("2,\x93\x8c\x8b\x9e\n", true);
        CHECK(readAppended(*book) == AppendResult::Appended);
        CHECK(exactRows(*model, 2) == 2);
        CHECK(cellText(*model, 1, 1) == "東京");
    }
}

TEST_CASE("readsAppendedRows: plain text files only")
{
    CHECK(TableBook::open(kDemoData / "normal.csv")->readsAppendedRows());
    CHECK_FALSE(TableBook::open(kDemoData / "normal.csv.gz")->readsAppendedRows());
    CHECK_FALSE(TableBook::open(kDemoData / "normal-utf16le-bom.tsv")->readsAppendedRows());
    CHECK_FALSE(TableBook::open(kDemoData / "normal.xlsx")->readsAppendedRows());
    CHECK_FALSE(TableBook::open(kDemoData / "normal.parquet")->readsAppendedRows());
}

TEST_CASE("readAppended: quotes only start a quoted field at its start")
{
    TempFile file("tableskim-test-follow-quotes.csv");
    file.write("n,name\n1,a\n");
    std::shared_ptr<TableBook> book;
    auto model = openRead(book, file.path());
    // A stray quote would otherwise hide every line break after it.
    file.write("2,12\" pizza\n3,b\n", true);
    CHECK(readAppended(*book) == AppendResult::Appended);
    CHECK(exactRows(*model, 3) == 3);
    CHECK(cellText(*model, 1, 1) == "12\" pizza");
    CHECK(cellText(*model, 2, 1) == "b");
    // A quoted field with a line break waits until it ends.
    file.write("4,\"two\n", true);
    CHECK(readAppended(*book) == AppendResult::None);
    file.write("lines\"\n", true);
    CHECK(readAppended(*book) == AppendResult::Appended);
    CHECK(exactRows(*model, 4) == 4);
    CHECK(cellText(*model, 3, 1) == "two\nlines");
}

TEST_CASE("readAppended: whether every byte has been read")
{
    TempFile file("tableskim-test-follow-caught-up.csv");
    file.write("n\n1\n");
    std::shared_ptr<TableBook> book;
    auto model = openRead(book, file.path());
    bool caught_up = false;
    CHECK(readAppended(*book, &caught_up) == AppendResult::None);
    CHECK(caught_up);
    file.write("2\n3", true);
    CHECK(readAppended(*book, &caught_up) == AppendResult::Appended);
    CHECK_FALSE(caught_up); // "3" has no line break yet
    file.write("\n", true);
    CHECK(readAppended(*book, &caught_up) == AppendResult::Appended);
    CHECK(caught_up);
    CHECK(exactRows(*model, 3) == 3);
}

TEST_CASE("readAppended: a file without rows takes its header from them")
{
    TempFile file("tableskim-test-follow-empty.csv");
    file.write("");
    std::shared_ptr<TableBook> book;
    auto model = openRead(book, file.path());
    CHECK(model->rowCount().value == 0);
    file.write("time,level\n12:00,INFO\n", true);
    CHECK(readAppended(*book) == AppendResult::Appended);
    CHECK(exactRows(*model, 1) == 1);
    REQUIRE(waitFor([&]
                    { return model->columnName(1) == "level"; }));
    CHECK(model->columnName(0) == "time");
    CHECK(cellText(*model, 0, 1) == "INFO");
}

TEST_CASE("readAppended: statistics, histograms and filters go on with the new rows")
{
    TempFile file("tableskim-test-follow-jobs.csv");
    std::string text = "n,kind\n";
    for (int i = 1; i <= 100; ++i)
        text += std::to_string(i) + "," + (i % 2 ? "odd" : "even") + "\n";
    file.write(text);
    std::shared_ptr<TableBook> book;
    auto model = openRead(book, file.path(), 8);

    SECTION("statistics and a histogram whose bins hold the new values")
    {
        model->startColumnStats(0);
        model->startHistogram(0, 10);
        REQUIRE(waitFor([&]
                        { return model->columnStatsState().done && model->histogramState().done; }));
        const uint64_t generation = model->histogramState().generation;
        file.write("50\n60,odd\n", true);
        CHECK(readAppended(*book) == AppendResult::Appended);
        REQUIRE(waitFor([&]
                        { return model->columnStatsState().done && model->columnStatsState().stats.rows == 102; }));
        REQUIRE(waitFor([&]
                        { return model->histogramState().done && model->histogramState().histogram.values == 102; }));
        CHECK(model->histogramState().generation == generation);

        // A value beyond the bins starts the histogram over.
        file.write("1000\n", true);
        CHECK(readAppended(*book) == AppendResult::Appended);
        REQUIRE(waitFor([&]
                        { return model->histogramState().done && model->histogramState().histogram.values == 103; }));
        CHECK(model->histogramState().generation != generation);
        CHECK(model->histogramState().histogram.edges.back() >= 1000);
    }
    SECTION("an unsorted filter takes the matching rows at its end")
    {
        const ViewSpec odd{.filter = Filter{Filter::Combine::And, {{1, FilterOp::Equals, "odd"}}}};
        model->setView(odd);
        REQUIRE(waitFor([&]
                        { return model->view() == odd; }));
        CHECK(model->rowCount().value == 50);
        file.write("101,odd\n102,even\n103,odd\n", true);
        CHECK(readAppended(*book) == AppendResult::Appended);
        REQUIRE(waitFor([&]
                        { return model->rowCount().value == 52; }));
        CHECK(cellText(*model, 51, 0) == "103");
        CHECK(model->view() == odd);
    }
}
