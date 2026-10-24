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

    AppendResult readAppended(TableBook &book)
    {
        std::promise<AppendResult> promise;
        auto future = promise.get_future();
        book.readAppended([&promise](AppendResult result)
                          { promise.set_value(result); });
        return future.get();
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
