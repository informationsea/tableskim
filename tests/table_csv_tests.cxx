#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <vector>

#include "table.hxx"
#include "table_csv.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    std::shared_ptr<TableModel> firstSheet(TableBook &book)
    {
        REQUIRE_FALSE(book.sheetNames().empty());
        auto model = book.sheet(book.sheetNames().front());
        REQUIRE(model);
        REQUIRE(waitFor([&]
                        { return model->state() != ModelState::Opening; }));
        REQUIRE(model->state() == ModelState::Ready);
        return model;
    }

    std::filesystem::path tempFile(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / ("tableskim-test-" + name);
    }

    // About how much memory the 10000 rows of large.csv take once read, by
    // RowBlock::approxBytes(), from the rows read up to a first checkpoint.
    // It depends on the standard library (sizes of std::string and the
    // cells: about 3.7 MiB with libstdc++), so tests set checkpoints
    // relative to it.
    size_t largeCsvBytes()
    {
        TableModel::Options options;
        options.block_rows = 256;
        options.memory_checkpoints = {size_t{512} << 10}; // above what opening reads
        options.memory_checkpoint_interval = 0;
        auto book = TableBook::open(kDemoData / "large.csv", {}, BackendRegistry::builtin(), options);
        auto model = firstSheet(*book);
        // Sorting reads every row, up to the checkpoint.
        model->setView(ViewSpec{.sort = {{.column = 0, .ascending = false}}});
        REQUIRE(waitFor([&]
                        { return model->memoryCheckpoint().has_value(); }));
        const auto checkpoint = *model->memoryCheckpoint();
        REQUIRE(checkpoint.rows > 0);
        model->stopReading();
        return checkpoint.bytes / checkpoint.rows * 10000;
    }
} // namespace

TEST_CASE("normal.csv: header and exact row count")
{
    auto book = TableBook::open(kDemoData / "normal.csv");
    REQUIRE(book->sheetNames() == std::vector<std::string>{"normal.csv"});
    auto model = firstSheet(*book);

    CHECK(model->hasHeader());
    CHECK(model->columnCount() == 9);
    CHECK(model->columnName(0) == "ID");
    CHECK(model->columnName(5) == "Department");
    CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    CHECK(cellText(*model, 1, 5) == "人事部");
    REQUIRE(waitFor([&]
                    { return model->rowCount() == RowCount{54, RowCount::Kind::Exact}; }));
    CHECK(model->displayRowCount() == 54);
    CHECK(model->peek(54, 0).state == CellState::OutOfRange);
}

TEST_CASE("CSV: non-ASCII directories and filenames", "[regression]")
{
    const auto directory = std::filesystem::temp_directory_path() / std::filesystem::path(u8"tableskim-test-表-😀");
    std::filesystem::create_directories(directory);
    const auto path = directory / std::filesystem::path(u8"データ-😀.csv");
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.good());
        out << "name,value\nTokyo,123\nOsaka,456\n";
    }
    {
        auto book = TableBook::open(path);
        REQUIRE(book->sheetNames() == std::vector<std::string>{"データ-😀.csv"});
        auto model = firstSheet(*book);
        CHECK(model->columnName(0) == "name");
        CHECK(cellText(*model, 0, 0) == "Tokyo");
        CHECK(cellText(*model, 1, 1) == "456");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
}

TEST_CASE("ragged.csv: rows of different widths are kept")
{
    auto book = TableBook::open(kDemoData / "ragged.csv");
    auto model = firstSheet(*book);

    CHECK(cellText(*model, 0, 4) == "r1c5");
    CHECK(cellText(*model, 1, 2) == "r2c3");
    CHECK(waitForCell(*model, 1, 3).state == CellState::Empty);
    CHECK(cellText(*model, 2, 6) == "r3c7");
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->columnCount() >= 12);
    CHECK(model->columnName(4) == "E");
    CHECK(model->columnName(11) == "L");
}

TEST_CASE("large.csv: loads lazily and reaches an exact count on demand")
{
    TableModel::Options options;
    options.block_rows = 256;
    auto book = TableBook::open(kDemoData / "large.csv", {}, BackendRegistry::builtin(), options);
    auto model = firstSheet(*book);

    // The first peek is answered immediately, without waiting for I/O.
    model->requestRange(9000, 9020);
    CHECK(cellText(*model, 9000, 0) == "9001");
    CHECK(model->rowCount().value >= 9001);

    model->requestRange(9990, 10010);
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 10000);
    CHECK(cellText(*model, 9999, 0) == "10000");

    // Sorted in memory, however many rows there are.
    const auto capabilities = model->viewCapabilities();
    CHECK(capabilities.sort);
    CHECK(capabilities.filter);
    CHECK(capabilities.search);
    const ViewSpec descending{.sort = {{.column = 0, .ascending = false}}};
    model->setView(descending);
    REQUIRE(waitFor([&]
                    { return model->view() == descending; }));
    CHECK(model->rowCount().value == 10000);
    CHECK(cellText(*model, 0, 0) == "10000");
}

TEST_CASE("memory checkpoints: 100 MB, 500 MB, 1 GB, then every 2 GB")
{
    const TableModel::Options options;
    constexpr size_t MB = size_t{1} << 20;
    constexpr size_t GB = size_t{1} << 30;
    CHECK(TableModel::nextMemoryCheckpoint(options, 0) == 100 * MB);
    CHECK(TableModel::nextMemoryCheckpoint(options, 100 * MB) == 500 * MB);
    CHECK(TableModel::nextMemoryCheckpoint(options, 600 * MB) == GB);
    CHECK(TableModel::nextMemoryCheckpoint(options, GB + 1) == 2 * GB);
    CHECK(TableModel::nextMemoryCheckpoint(options, 2 * GB) == 4 * GB);
    CHECK(TableModel::nextMemoryCheckpoint(options, 5 * GB) == 6 * GB);

    TableModel::Options never;
    never.memory_checkpoints.clear();
    never.memory_checkpoint_interval = 0;
    CHECK(TableModel::nextMemoryCheckpoint(never, 0) == 0);
}

TEST_CASE("large.csv: reading pauses at each memory checkpoint")
{
    // Pauses at 20% and 40% of the file, then every 40% (80%); the next
    // one (120%) is past its end. All are above what opening reads.
    const size_t total = largeCsvBytes();
    TableModel::Options options;
    options.block_rows = 256;
    options.memory_checkpoints = {total / 5, total * 2 / 5};
    options.memory_checkpoint_interval = total * 2 / 5;
    const size_t thresholds[] = {total / 5, total * 2 / 5, total * 4 / 5};
    auto book = TableBook::open(kDemoData / "large.csv", {}, BackendRegistry::builtin(), options);
    auto model = firstSheet(*book);
    std::atomic<int> checkpoints{0};
    auto subscription = model->subscribe([&](const TableEvent &event)
                                         {
        if (event.kind == TableEvent::Kind::MemoryCheckpoint)
            ++checkpoints; });

    // Sorting reads every row, up to the first checkpoint.
    const ViewSpec descending{.sort = {{.column = 0, .ascending = false}}};
    model->setView(descending);
    REQUIRE(waitFor([&]
                    { return model->memoryCheckpoint().has_value(); }));
    const auto checkpoint = *model->memoryCheckpoint();
    CHECK(checkpoint.bytes >= options.memory_checkpoints[0]);
    CHECK(checkpoint.bytes < options.memory_checkpoints[1]);
    CHECK(checkpoint.rows > 0);
    CHECK(checkpoint.rows < 10000);
    CHECK(checkpoint.rows == model->rowCount().value);
    CHECK(model->view() == ViewSpec{});
    // Listeners hear of it just after it is set.
    CHECK(waitFor([&]
                  { return checkpoints == 1; }));

    SECTION("continue reading")
    {
        std::vector<size_t> sizes;
        while (!(model->view() == descending))
        {
            REQUIRE(waitFor([&]
                            { return model->memoryCheckpoint() || model->view() == descending; }));
            if (const auto paused = model->memoryCheckpoint())
                sizes.push_back(paused->bytes);
            model->continueReading();
        }
        // Each pause comes at the next checkpoint, a block past it at most.
        REQUIRE(sizes.size() == 3);
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            CHECK(sizes[i] >= thresholds[i]);
            CHECK(sizes[i] < thresholds[i] + (size_t{512} << 10));
        }
        CHECK(checkpoints == 3);
        CHECK(model->rowCount() == RowCount{10000, RowCount::Kind::Exact});
        CHECK(cellText(*model, 0, 0) == "10000");
        CHECK_FALSE(model->readingStopped());
    }
    SECTION("stop reading")
    {
        model->stopReading();
        REQUIRE(waitFor([&]
                        { return model->view() == descending; }));
        CHECK_FALSE(model->memoryCheckpoint());
        CHECK(model->readingStopped());
        // The table ends at the rows read.
        CHECK(model->rowCount() == RowCount{checkpoint.rows, RowCount::Kind::Exact});
        CHECK(cellText(*model, 0, 0) == std::to_string(checkpoint.rows));
        CHECK(checkpoints == 1);
    }
}

TEST_CASE("TSV and header options")
{
    const auto path = tempFile("sample.tsv");
    {
        std::ofstream out(path);
        out << "1\ta,b\t3\n4\t5\t6\n";
    }
    auto book = TableBook::open(path);
    auto model = firstSheet(*book);
    CHECK_FALSE(model->hasHeader()); // numeric first row
    CHECK(cellText(*model, 0, 1) == "a,b");
    CHECK(cellText(*model, 1, 2) == "6");

    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    auto book2 = TableBook::open(path, options);
    auto model2 = firstSheet(*book2);
    CHECK(model2->columnName(1) == "a,b");
    CHECK(cellText(*model2, 0, 0) == "4");
    REQUIRE(waitFor([&]
                    { return model2->rowCount().isExact(); }));
    CHECK(model2->rowCount().value == 1);
    // Windows cannot remove a file the CSV sources still have open.
    book.reset();
    book2.reset();
    std::filesystem::remove(path);
}

TEST_CASE("in-memory sort, filter and search on a small CSV")
{
    auto book = TableBook::open(kDemoData / "normal.csv");
    auto model = firstSheet(*book);
    REQUIRE(waitFor([&]
                    { return model->viewCapabilities().sort; }));

    // Age (column 3) descending, numeric-aware; the one empty Age goes last.
    model->setView({.sort = {{.column = 3, .ascending = false}}});
    REQUIRE(waitFor([&]
                    { return !model->view().sort.empty(); }));
    double previous = 1e9;
    for (size_t r = 0; r < 53; ++r)
    {
        const auto age = parseNumber(cellText(*model, r, 3));
        REQUIRE(age.has_value());
        CHECK(*age <= previous);
        previous = *age;
    }
    CHECK(cellText(*model, 53, 3) == "");

    model->setView({.filter = Filter{.conditions = {{.column = 5, .op = FilterOp::Equals, .value = "Sales"}}}});
    REQUIRE(waitFor([&]
                    { return model->view().filter.has_value(); }));
    const size_t n = model->rowCount().value;
    CHECK(n > 0);
    CHECK(n < 54);
    for (size_t r = 0; r < n; ++r)
        CHECK(cellText(*model, r, 5) == "Sales");

    model->startSearch({.text = "sales", .column = 5});
    REQUIRE(waitFor([&]
                    { return model->searchState().done; }));
    CHECK(model->searchState().hits.size() == n);
}

TEST_CASE("large generated CSV stays responsive")
{
    const auto path = tempFile("big.csv");
    const size_t total_rows = 200000;
    {
        std::ofstream out(path);
        out << "id,name,value\n";
        for (size_t i = 0; i < total_rows; ++i)
            out << i << ",name" << i << "," << (i * 7 % 1000) << "\n";
    }
    auto book = TableBook::open(path);
    auto model = firstSheet(*book);

    const auto start = std::chrono::steady_clock::now();
    const CellRef first = model->peek(0, 0);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50));
    CHECK((first.state == CellState::Loading || first.state == CellState::Ready));
    CHECK(cellText(*model, 0, 1) == "name0");
    // Only the beginning is read until something further is requested.
    CHECK_FALSE(model->rowCount().isExact());

    model->requestRange(total_rows - 10, total_rows);
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }, std::chrono::seconds(60)));
    CHECK(model->rowCount().value == total_rows);
    CHECK(cellText(*model, total_rows - 1, 1) == "name" + std::to_string(total_rows - 1));

    book.reset();
    std::filesystem::remove(path);
}

TEST_CASE("CSV: a title line is not a header; tiny files keep their rows")
{
    const auto path = tempFile("title.csv");
    {
        std::ofstream out(path);
        out << "Report\nname,value\na,1\n";
    }
    auto book = TableBook::open(path);
    auto model = firstSheet(*book);
    CHECK_FALSE(model->hasHeader());
    CHECK(cellText(*model, 0, 0) == "Report");
    CHECK(cellText(*model, 1, 0) == "name");
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 3);
    book.reset();

    // Header plus a single data row: both read-ahead rows must survive.
    {
        std::ofstream out(path);
        out << "name,value\na,1\n";
    }
    auto book2 = TableBook::open(path);
    auto model2 = firstSheet(*book2);
    CHECK(model2->hasHeader());
    CHECK(cellText(*model2, 0, 1) == "1");
    REQUIRE(waitFor([&]
                    { return model2->rowCount().isExact(); }));
    CHECK(model2->rowCount().value == 1);
    book2.reset();
    std::filesystem::remove(path);
}

TEST_CASE("CSV: an empty file opens as an empty table", "[regression]")
{
    const auto path = tempFile("empty.csv");
    std::ofstream(path).close();
    auto book = TableBook::open(path);
    auto model = firstSheet(*book);
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 0);
    CHECK(model->state() != ModelState::Error);
    book.reset();
    std::filesystem::remove(path);
}

TEST_CASE("Header::No keeps the first row as data (GUI default)")
{
    OpenOptions options;
    options.header = OpenOptions::Header::No;
    for (const char *file : {"normal.csv", "normal.xlsx"})
    {
        INFO(file);
        auto book = TableBook::open(kDemoData / file, options);
        auto model = firstSheet(*book);
        CHECK_FALSE(model->hasHeader());
        CHECK(model->columnName(0) == "A");
        CHECK(model->columnName(8) == "I");
        CHECK(cellText(*model, 0, 0) == "ID");
        CHECK(cellText(*model, 1, 1) == "Sakura Brown");
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        CHECK(model->rowCount().value == 55);
    }
}

TEST_CASE("compressed CSV/TSV files are decompressed transparently")
{
    for (const char *file : {"normal.csv.gz", "normal.csv.bz2", "normal.csv.xz", "normal.csv.zst"})
    {
        INFO(file);
        auto book = TableBook::open(kDemoData / file);
        REQUIRE(book->sheetNames() == std::vector<std::string>{file});
        auto model = firstSheet(*book);
        CHECK(model->hasHeader());
        CHECK(model->columnCount() == 9);
        CHECK(model->columnName(5) == "Department");
        CHECK(cellText(*model, 0, 1) == "Sakura Brown");
        CHECK(cellText(*model, 1, 5) == "人事部");
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        CHECK(model->rowCount().value == 54);
    }

    // The tab delimiter comes from the inner extension.
    auto book = TableBook::open(kDemoData / "large.tsv.gz");
    auto model = firstSheet(*book);
    CHECK(model->columnName(0) == "RowNo");
    model->requestRange(9990, 10000);
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 10000);
    CHECK(cellText(*model, 9999, 0) == "10000");
}

TEST_CASE("compressed CSV is detected by content, not by name")
{
    const auto path = tempFile("misnamed.csv");
    std::filesystem::copy_file(kDemoData / "normal.csv.zst", path, std::filesystem::copy_options::overwrite_existing);
    {
        auto book = TableBook::open(path);
        auto model = firstSheet(*book);
        CHECK(model->columnName(0) == "ID");
        CHECK(cellText(*model, 0, 1) == "Sakura Brown");
    }
    std::filesystem::remove(path);
}

TEST_CASE("CSV: sheet name is the UTF-8 file name")
{
    const std::string name = "tableskim-test-日本語.csv";
    const std::filesystem::path path = std::filesystem::temp_directory_path() / std::filesystem::path(std::u8string(name.begin(), name.end()));
    {
        std::ofstream out(path, std::ios::binary);
        out << "a,b\n1,2\n";
    }
    {
        auto book = TableBook::open(path);
        REQUIRE(book->sheetNames() == std::vector<std::string>{name});
        auto model = firstSheet(*book);
        CHECK(cellText(*model, 0, 0) == "1");
    }
    std::filesystem::remove(path);
}

TEST_CASE("VCF: lines starting with ## can be skipped")
{
    const auto path = tempFile("variants.vcf");
    {
        std::ofstream out(path, std::ios::binary);
        // Meta lines may hold quotes and tabs; one is longer than a read chunk.
        out << "##fileformat=VCFv4.2\n"
               "##INFO=<ID=DP,Number=1,Type=Integer,Description=\"Total \"\"Depth\">\n"
               "##comment=a\tb\n"
               "##long=" << std::string(100 * 1024, 'x') << "\n"
               "#CHROM\tPOS\tID\tREF\tALT\n"
               "1\t100\trs1\tA\t##G\n"
               "##dropped\tC\tT\r\n"
               "2\t200\t.\tC\tT";
    }

    SECTION("skipped by default")
    {
        OpenOptions options;
        options.header = OpenOptions::Header::Yes;
        auto book = TableBook::open(path, options);
        CHECK(book->isTextFormat());
        CHECK(book->skipsMetaLines());
        auto model = firstSheet(*book);
        REQUIRE(model->columnCount() == 5);
        CHECK(model->columnName(0) == "#CHROM");
        CHECK(model->columnName(4) == "ALT");
        CHECK(cellText(*model, 0, 2) == "rs1");
        // Only lines starting with ## are dropped.
        CHECK(cellText(*model, 0, 4) == "##G");
        CHECK(cellText(*model, 1, 0) == "2");
        CHECK(cellText(*model, 1, 4) == "T");
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        CHECK(model->rowCount().value == 2);
    }

    SECTION("kept")
    {
        OpenOptions options;
        options.header = OpenOptions::Header::No;
        options.skip_meta_lines = false;
        auto book = TableBook::open(path, options);
        CHECK_FALSE(book->skipsMetaLines());
        auto model = firstSheet(*book);
        CHECK(cellText(*model, 0, 0) == "##fileformat=VCFv4.2");
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        CHECK(model->rowCount().value == 8);
    }
    std::filesystem::remove(path);
}

TEST_CASE("skipping ## lines keeps lone # lines and works on compressed files")
{
    const auto path = tempFile("hash.csv");
    {
        std::ofstream out(path, std::ios::binary);
        out << "##meta\n#\na,b\n##\n#c,d";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::No;
    options.skip_meta_lines = true;
    {
        auto book = TableBook::open(path, options);
        auto model = firstSheet(*book);
        CHECK(cellText(*model, 0, 0) == "#");
        CHECK(cellText(*model, 1, 1) == "b");
        CHECK(cellText(*model, 2, 0) == "#c");
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        CHECK(model->rowCount().value == 3);
    }
    std::filesystem::remove(path);

    // Nothing to skip in the demo file, but it goes through both streams.
    auto book = TableBook::open(kDemoData / "normal.csv.gz", options);
    auto model = firstSheet(*book);
    CHECK(cellText(*model, 0, 0) == "ID");
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 55);
}

TEST_CASE("## lines are skipped by default only in VCF files")
{
    CHECK_FALSE(TableBook::open(kDemoData / "normal.csv")->skipsMetaLines());
    CHECK_FALSE(TableBook::open(kDemoData / "normal.csv.gz")->skipsMetaLines());
    for (const char *name : {"variants.vcf.gz", "variants.VCF.bgz"})
    {
        INFO(name);
        const auto path = tempFile(name);
        std::filesystem::copy_file(kDemoData / "normal.csv.gz", path, std::filesystem::copy_options::overwrite_existing);
        {
            auto book = TableBook::open(path);
            CHECK(book->skipsMetaLines());
            auto model = firstSheet(*book);
            // Tab-separated, so each CSV line is a single cell.
            CHECK(cellText(*model, 0, 0).starts_with("1,Sakura Brown,"));
        }
        std::filesystem::remove(path);
    }
}

TEST_CASE("only text formats use the ## option")
{
    CHECK_FALSE(TableBook::open(kDemoData / "normal.parquet")->isTextFormat());
    CHECK_FALSE(TableBook::open(kDemoData / "normal.xlsx")->isTextFormat());
    CHECK_FALSE(TableBook::open(kDemoData / "normal.parquet")->textEncoding());
    CHECK_FALSE(TableBook::open(kDemoData / "normal.xlsx")->textEncoding());
}

TEST_CASE("CSV: column statistics read the file in order", "[stats]")
{
    const auto path = tempFile("stats.csv");
    {
        std::ofstream out(path, std::ios::binary);
        out << "n,s\n1,x\n2\n3,y\n,x\n";
    }
    {
        OpenOptions options;
        options.header = OpenOptions::Header::Yes;
        auto book = TableBook::open(path, options);
        auto model = firstSheet(*book);
        model->startColumnStats(0);
        REQUIRE(waitFor([&] { return model->columnStatsState().done; }));
        const ColumnStats numbers = model->columnStatsState().stats;
        CHECK(numbers.rows == 4);
        CHECK(numbers.empty == 1);
        CHECK(numbers.numbers == 3);
        CHECK(numbers.mean == 2.0);

        // The short row has no cell in this column: it is empty.
        model->startColumnStats(1);
        REQUIRE(waitFor([&] { return model->columnStatsState().done; }));
        const ColumnStats text = model->columnStatsState().stats;
        CHECK(text.rows == 4);
        CHECK(text.empty == 1);
        CHECK(text.distinct == 2);
        REQUIRE(text.top.size() == 2);
        CHECK(text.top[0] == ValueCount{CellValue{std::string("x")}, 2});
        CHECK(text.top[1] == ValueCount{CellValue{std::string("y")}, 1});
    }
    std::filesystem::remove(path);
}

TEST_CASE("CSV/TSV: the text encoding is detected", "[encoding]")
{
    const std::pair<const char *, TextEncoding> files[] = {
        {"normal.csv", TextEncoding::Utf8},
        {"normal-utf8-bom.csv", TextEncoding::Utf8},
        {"normal-cp932.csv", TextEncoding::Cp932},
        {"normal-cp932.csv.gz", TextEncoding::Cp932},
        {"normal-utf16le-bom.tsv", TextEncoding::Utf16LE},
        {"normal-utf16le.csv", TextEncoding::Utf16LE},
        {"normal-utf16be.csv", TextEncoding::Utf16BE},
    };
    for (const auto &[file, encoding] : files)
    {
        INFO(file);
        auto book = TableBook::open(kDemoData / file);
        CHECK(book->textEncoding() == encoding);
        auto model = firstSheet(*book);
        // No BOM before the first column name.
        CHECK(model->hasHeader());
        REQUIRE(model->columnCount() == 9);
        CHECK(model->columnName(0) == "ID");
        CHECK(model->columnName(5) == "Department");
        CHECK(cellText(*model, 0, 1) == "Sakura Brown");
        CHECK(cellText(*model, 1, 5) == "人事部");
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        CHECK(model->rowCount().value == 54);
    }
}

TEST_CASE("cp932-sample.csv: separators inside characters and extensions", "[encoding]")
{
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    auto book = TableBook::open(kDemoData / "cp932-sample.csv", options);
    CHECK(book->textEncoding() == TextEncoding::Cp932);
    auto model = firstSheet(*book);
    REQUIRE(model->columnCount() == 3);
    CHECK(cellText(*model, 0, 1) == "表示,ソフト");
    CHECK(cellText(*model, 1, 1) == "ポイント|予定");
    CHECK(cellText(*model, 2, 1) == "①②③ ㈱ №");
    CHECK(cellText(*model, 3, 1) == "髙橋 﨑 纊");
    CHECK(cellText(*model, 4, 1) == "ｶﾀｶﾅ ﾃｽﾄ");
    CHECK(cellText(*model, 5, 1) == "～－￠￡￢∥");
    CHECK(cellText(*model, 6, 1) == "改行を\n含む");
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    CHECK(model->rowCount().value == 7);
}

TEST_CASE("CSV: OpenOptions::encoding overrides detection", "[encoding]")
{
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    options.encoding = TextEncoding::Cp932;
    {
        auto book = TableBook::open(kDemoData / "normal-cp932.csv", options);
        CHECK(book->textEncoding() == TextEncoding::Cp932);
        CHECK(cellText(*firstSheet(*book), 1, 5) == "人事部");
    }
    // UTF-8 read as CP932 is garbled, but every row is still there.
    {
        auto book = TableBook::open(kDemoData / "normal.csv", options);
        auto model = firstSheet(*book);
        CHECK(cellText(*model, 0, 1) == "Sakura Brown");
        CHECK(cellText(*model, 1, 5) != "人事部");
        REQUIRE(waitFor([&]
                        { return model->rowCount().isExact(); }));
        CHECK(model->rowCount().value == 54);
    }
    options.encoding = TextEncoding::Utf8;
    {
        auto book = TableBook::open(kDemoData / "normal-cp932.csv", options);
        CHECK(book->textEncoding() == TextEncoding::Utf8);
        CHECK(cellText(*firstSheet(*book), 1, 5) != "人事部");
    }
}

TEST_CASE("## lines are skipped after a BOM and in UTF-16", "[encoding]")
{
    const std::string text = "##meta,\"x\"\n#CHROM\tPOS\n1\t100\n";
    std::string utf16 = "\xFF\xFE";
    for (const char c : text)
        utf16 += std::string{c, '\0'};
    for (const auto &[name, bytes] : {std::pair<std::string, std::string>{"bom.vcf", "\xEF\xBB\xBF" + text},
                                      {"utf16.vcf", utf16}})
    {
        INFO(name);
        const auto path = tempFile(name);
        {
            std::ofstream out(path, std::ios::binary);
            out << bytes;
        }
        {
            OpenOptions options;
            options.header = OpenOptions::Header::Yes;
            auto book = TableBook::open(path, options);
            CHECK(book->skipsMetaLines());
            auto model = firstSheet(*book);
            CHECK(model->columnName(0) == "#CHROM");
            CHECK(cellText(*model, 0, 1) == "100");
        }
        std::filesystem::remove(path);
    }
}

TEST_CASE("UTF-16 files open as text whatever their extension", "[encoding]")
{
    const auto path = tempFile("utf16.dat");
    std::filesystem::copy_file(kDemoData / "normal-utf16be.csv", path, std::filesystem::copy_options::overwrite_existing);
    {
        auto book = TableBook::open(path);
        CHECK(book->textEncoding() == TextEncoding::Utf16BE);
        auto model = firstSheet(*book);
        CHECK(model->columnName(0) == "ID");
        CHECK(cellText(*model, 1, 5) == "人事部");
    }
    std::filesystem::remove(path);
}

TEST_CASE("CSV: truncating a file while reading does not invalidate queued rows", "[regression]")
{
    const auto path = tempFile("truncated-while-reading.csv");
    constexpr size_t total = 40000;
    const std::string payload(128, 'x');
    {
        std::ofstream out(path, std::ios::binary);
        out << "id,payload\n";
        for (size_t i = 0; i < total; ++i)
            out << i << ',' << payload << '\n';
    }
    {
        OpenOptions options;
        options.header = OpenOptions::Header::Yes;
        auto data = openCsvDataSource(path, options);
        auto sheet = data->openSheet(data->sheetNames().front());
        size_t rows = 0;
        while (rows < 10000)
            rows += sheet->read(rows, 512, {})->rows.size();
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            REQUIRE(out.good());
            out << "id,payload\n0,new\n";
        }
        bool ended = false;
        while (rows <= total)
        {
            const auto block = sheet->read(rows, 512, {});
            for (const auto &row : block->rows)
            {
                REQUIRE(row.size() == 2);
                CHECK(formatCell(row[1]) == payload);
            }
            rows += block->rows.size();
            if (block->rows.size() < 512)
            {
                ended = true;
                break;
            }
        }
        CHECK(ended);
        CHECK(rows <= total);
    }
    std::filesystem::remove(path);
}

// Hidden (run with "[.super-large]"): one million rows, xz-compressed.
TEST_CASE("super-large.csv.xz: reaches the last row", "[.super-large]")
{
    auto book = TableBook::open(kDemoData / "super-large.csv.xz");
    auto model = firstSheet(*book);
    CHECK(model->columnName(0) == "RowNo");
    model->requestRange(999990, 999999);
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }, std::chrono::minutes(5)));
    CHECK(model->rowCount().value == 1000000);
    CHECK(cellText(*model, 999999, 0) == "1000000");
    CHECK(cellText(*model, 999999, 2) == "epsilon");
}
