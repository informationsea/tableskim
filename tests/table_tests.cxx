#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    struct FakeStats
    {
        std::atomic<int> reads{0};
        std::atomic<int> counts{0};
        std::mutex mutex;
        std::set<size_t> read_rows;
        std::optional<ViewSpec> applied;
        bool pause_first_view = false;
        std::atomic<int> view_applies{0};
        std::atomic<bool> view_entered{false};
        std::atomic<bool> release_view{false};
        // Pushed-down filters fail when applied, or only when rows are read.
        std::atomic<bool> fail_filter_apply{false};
        std::atomic<bool> fail_filtered_reads{false};
        // columnValueCounts(): supported, how often it ran, failing halfway.
        std::atomic<bool> value_counts{false};
        std::atomic<int> value_count_calls{0};
        std::atomic<bool> fail_value_counts{false};
        // countRowsSlow() cannot count, as for a source without a cheap count.
        std::atomic<bool> no_count{false};
    };

    // Random-access source whose cell (r, c) holds r * 10 + c.
    class FakeSheet : public SheetSource
    {
    private:
        size_t rows_;
        size_t cols_;
        bool pushdown_;
        std::shared_ptr<FakeStats> stats_;
        bool descending_ = false;
        bool filtered_ = false;
        std::shared_ptr<const std::vector<TableRow>> values_;

    public:
        FakeSheet(size_t rows, size_t cols, bool pushdown, std::shared_ptr<FakeStats> stats,
                  std::shared_ptr<const std::vector<TableRow>> values)
            : rows_(rows), cols_(cols), pushdown_(pushdown), stats_(std::move(stats)), values_(std::move(values)) {}

        SourceCapabilities capabilities() const override { return {.random_access = true, .query_pushdown = pushdown_}; }
        std::vector<ColumnInfo> columns() override
        {
            std::vector<ColumnInfo> result;
            for (size_t c = 0; c < cols_; ++c)
                result.push_back({"c" + std::to_string(c)});
            return result;
        }
        RowCount rowCount() override { return {}; }
        std::optional<size_t> countRowsSlow(std::stop_token) override
        {
            if (stats_->no_count)
                return std::nullopt;
            ++stats_->counts;
            // Pretend that a pushed-down filter keeps half of the rows.
            return filtered_ ? rows_ / 2 : rows_;
        }
        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token) override
        {
            ++stats_->reads;
            if (filtered_ && stats_->fail_filtered_reads)
                throw std::runtime_error("filtered read failed");
            {
                std::lock_guard lock(stats_->mutex);
                stats_->read_rows.insert(first_row);
            }
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            for (size_t r = first_row; r < std::min(rows_, first_row + max_rows); ++r)
            {
                const size_t source_row = descending_ ? rows_ - 1 - r : r;
                if (values_)
                {
                    block->rows.push_back((*values_)[source_row]);
                    continue;
                }
                TableRow row;
                for (size_t c = 0; c < cols_; ++c)
                    row.emplace_back(static_cast<int64_t>(source_row * 10 + c));
                block->rows.push_back(std::move(row));
            }
            return block;
        }
        std::optional<size_t> columnValueCounts(size_t column, const ValueCountSink &add,
                                                std::stop_token) override
        {
            if (!stats_->value_counts)
                return std::nullopt;
            ++stats_->value_count_calls;
            if (stats_->fail_value_counts)
            {
                add(CellValue{int64_t{-1}}, 1000);
                throw std::runtime_error("value counts failed");
            }
            // Every cell (r * 10 + c) is distinct; empty past the last column.
            if (column >= cols_)
            {
                add(CellValue{}, rows_);
                return 0;
            }
            for (size_t r = 0; r < rows_; ++r)
                add(CellValue{static_cast<int64_t>(r * 10 + column)}, 1);
            return rows_;
        }

        void applyView(const ViewSpec &spec) override
        {
            if (++stats_->view_applies == 1 && stats_->pause_first_view)
            {
                stats_->view_entered = true;
                waitFor([&] { return stats_->release_view.load(); }, std::chrono::seconds(2));
            }
            if (spec.filtered() && stats_->fail_filter_apply)
                throw std::runtime_error("filter failed");
            std::lock_guard lock(stats_->mutex);
            stats_->applied = spec;
            descending_ = !spec.sort.empty() && !spec.sort.front().ascending;
            filtered_ = spec.filtered();
        }
    };

    class FakeData : public DataSource
    {
    public:
        size_t rows = 100000;
        size_t cols = 3;
        bool pushdown = false;
        std::shared_ptr<FakeStats> stats = std::make_shared<FakeStats>();
        std::shared_ptr<const std::vector<TableRow>> values;

        std::vector<std::string> sheetNames() override { return {"a", "b"}; }
        std::unique_ptr<SheetSource> openSheet(const std::string &name) override
        {
            if (name != "a" && name != "b")
                return nullptr;
            return std::make_unique<FakeSheet>(values ? values->size() : rows, cols, pushdown, stats, values);
        }
    };
} // namespace

TEST_CASE("formatCell and helpers")
{
    CHECK(formatCell(CellValue{}) == "");
    CHECK(formatCell(CellValue{std::string("abc")}) == "abc");
    CHECK(formatCell(CellValue{int64_t{-42}}) == "-42");
    CHECK(formatCell(CellValue{1.5}) == "1.5");
    CHECK(formatCell(CellValue{true}) == "TRUE");
    CHECK(formatCell(CellValue{Date{0}}) == "1970-01-01");
    CHECK(formatCell(CellValue{Date{20366}}) == "2025-10-05");
    CHECK(formatCell(CellValue{DateTime{86400LL * 1000000 + 3723000001LL}}) == "1970-01-02 01:02:03.000001");
    CHECK(parseNumber("12.5") == 12.5);
    CHECK(parseNumber(" +3 ") == 3.0);
    CHECK_FALSE(parseNumber("12a").has_value());
    CHECK_FALSE(parseNumber("").has_value());
    CHECK(columnLetter(0) == "A");
    CHECK(columnLetter(25) == "Z");
    CHECK(columnLetter(26) == "AA");
    CHECK(columnLetter(701) == "ZZ");
    CHECK(columnLetter(702) == "AAA");
}

TEST_CASE("compareCells orders dates, date-times and numbers consistently")
{
    constexpr int64_t day = 86400LL * 1000000;
    const CellValue date{Date{19723}};                  // 2024-01-01
    const CellValue midnight{DateTime{19723 * day}};    // 2024-01-01 00:00
    const CellValue noon{DateTime{19723 * day + day / 2}};
    const CellValue number{int64_t{20000}};
    CHECK(compareCells(date, midnight) == 0);
    // A date-time counts in days against numbers, like a date.
    CHECK(compareCells(date, number) < 0);
    CHECK(compareCells(midnight, number) < 0);
    CHECK(compareCells(noon, CellValue{19723.25}) > 0);
    CHECK(compareCells(noon, CellValue{19723.75}) < 0);
    CHECK(compareCells(date, CellValue{19723.25}) < 0);
    CHECK(compareCells(noon, CellValue{std::string("text")}) < 0);
    // Any order of these sorts the same.
    std::vector<CellValue> cells{number, noon, CellValue{19723.25}, date, CellValue{std::string("x")}, midnight};
    std::vector<CellValue> reversed(cells.rbegin(), cells.rend());
    const auto less = [](const CellValue &a, const CellValue &b) { return compareCells(a, b) < 0; };
    std::stable_sort(cells.begin(), cells.end(), less);
    std::stable_sort(reversed.begin(), reversed.end(), less);
    for (size_t i = 0; i < cells.size(); ++i)
        CHECK(compareCells(cells[i], reversed[i]) == 0);
}

TEST_CASE("parseTemporal and parseBoolean", "[filter]")
{
    constexpr int64_t day = 86400LL * 1000000;
    const auto date = parseTemporal("1970-01-02");
    REQUIRE(date);
    CHECK(date->micros == day);
    CHECK_FALSE(date->has_time);
    CHECK(parseTemporal(" 1969-12-31 ")->micros == -day);
    const auto time = parseTemporal("1970-01-01T01:02:03.5");
    REQUIRE(time);
    CHECK(time->micros == 3723500000);
    CHECK(time->has_time);
    CHECK(parseTemporal("1970-01-01 00:01")->micros == 60000000);
    CHECK(parseTemporal("1970-01-01 00:00:00.0000019")->micros == 1);
    for (const char *invalid : {"", "2024-02-30", "2024-13-01", "2024-1-5", "2024/01/05", "2024-01-05 24:00",
                                "2024-01-05 10", "2024-01-05 10:00:00.", "2024-01-05x", "20240105", "abc"})
    {
        INFO(invalid);
        CHECK_FALSE(parseTemporal(invalid));
    }
    CHECK(parseTemporal("2024-02-29"));

    CHECK(parseBoolean("TRUE") == true);
    CHECK(parseBoolean(" yes ") == true);
    CHECK(parseBoolean("No") == false);
    CHECK(parseBoolean("0") == false);
    CHECK_FALSE(parseBoolean("maybe"));
    CHECK_FALSE(parseBoolean(""));
}

TEST_CASE("filters compare by the cell's type", "[filter]")
{
    auto matches = [](const CellValue &cell, FilterOp op, const std::string &value)
    { return cellMatchesCondition(cell, {0, op, value}); };
    constexpr int64_t day = 86400LL * 1000000;
    const CellValue noon = DateTime{19723 * day + day / 2}; // 2024-01-01 12:00:00
    const CellValue date = Date{19723};                     // 2024-01-01

    // A date value covers its whole day.
    CHECK(matches(noon, FilterOp::Equals, "2024-01-01"));
    CHECK_FALSE(matches(noon, FilterOp::NotEquals, "2024-01-01"));
    CHECK(matches(noon, FilterOp::LessEqual, "2024-01-01"));
    CHECK_FALSE(matches(noon, FilterOp::Less, "2024-01-01"));
    CHECK(matches(noon, FilterOp::GreaterEqual, "2024-01-01"));
    CHECK_FALSE(matches(noon, FilterOp::Greater, "2024-01-01"));
    CHECK(matches(noon, FilterOp::Greater, "2023-12-31"));
    CHECK(matches(noon, FilterOp::Less, "2024-01-02"));
    // A date-time value is one instant.
    CHECK(matches(noon, FilterOp::Equals, "2024-01-01 12:00"));
    CHECK(matches(noon, FilterOp::Greater, "2024-01-01T11:59:59.999999"));
    CHECK_FALSE(matches(noon, FilterOp::Equals, "2024-01-01 12:00:00.000001"));
    CHECK(matches(date, FilterOp::Equals, "2024-01-01"));
    CHECK(matches(date, FilterOp::Less, "2024-01-01 00:00:01"));
    // Text holding a date is a date, too.
    CHECK(matches(CellValue{std::string("2024-01-01 08:30")}, FilterOp::Equals, "2024-01-01"));
    CHECK(matches(CellValue{std::string("2024-1-1")}, FilterOp::Greater, "2024-01-01")); // text: "2024-1-1" > "2024-01-01"
    // Values that are no dates compare as before.
    CHECK(matches(date, FilterOp::Equals, "2024-01-01"));
    CHECK(matches(date, FilterOp::Contains, "01-01"));
    CHECK_FALSE(matches(CellValue{}, FilterOp::Equals, "2024-01-01"));
    CHECK(matches(CellValue{}, FilterOp::Less, "2024-01-01"));

    // Booleans against TRUE / FALSE, yes / no, 1 / 0.
    CHECK(matches(CellValue{true}, FilterOp::Equals, "true"));
    CHECK(matches(CellValue{true}, FilterOp::Equals, "Yes"));
    CHECK(matches(CellValue{false}, FilterOp::NotEquals, "TRUE"));
    CHECK(matches(CellValue{false}, FilterOp::Less, "true"));
    CHECK_FALSE(matches(CellValue{false}, FilterOp::Equals, "maybe"));
    CHECK(matches(CellValue{std::string("TRUE")}, FilterOp::Equals, "TRUE")); // text stays text
    CHECK_FALSE(matches(CellValue{std::string("TRUE")}, FilterOp::Equals, "true"));
}

TEST_CASE("regular expression filters", "[filter]")
{
    auto matches = [](const CellValue &cell, const std::string &pattern)
    { return cellMatchesCondition(cell, {0, FilterOp::Matches, pattern}); };
    CHECK(matches(CellValue{std::string("Apple pie")}, "^apple"));
    CHECK(matches(CellValue{std::string("Apple pie")}, "PIE$"));
    CHECK_FALSE(matches(CellValue{std::string("Apple pie")}, "(?-i)PIE"));
    CHECK(matches(CellValue{int64_t{2024}}, "^\\d{4}$"));
    CHECK(matches(CellValue{Date{19723}}, "-01-01$"));
    CHECK(matches(CellValue{std::string("日本語")}, "^日.語$"));
    CHECK(matches(CellValue{}, "^$"));
    CHECK_FALSE(matches(CellValue{std::string("x")}, "^$"));
    CHECK(cellMatchesCondition(CellValue{std::string("x")}, {0, FilterOp::NotMatches, "y"}));
    // An invalid pattern matches nothing, and filterError() says why.
    CHECK_FALSE(matches(CellValue{std::string("(")}, "("));
    CHECK_FALSE(cellMatchesCondition(CellValue{std::string("(")}, {0, FilterOp::NotMatches, "("}));
    Filter filter{.conditions = {{0, FilterOp::Contains, "("}, {1, FilterOp::Matches, "a+"}}};
    CHECK_FALSE(filterError(filter));
    filter.conditions.push_back({2, FilterOp::NotMatches, "(a"});
    const auto error = filterError(filter);
    REQUIRE(error);
    CHECK(error->find("Invalid regular expression \"(a\"") == 0);
}

TEST_CASE("random access source loads only what is requested")
{
    auto data = std::make_unique<FakeData>();
    auto stats = data->stats;
    TableModel::Options options;
    options.block_rows = 100;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    REQUIRE(model);
    CHECK(book.sheet("missing") == nullptr);

    REQUIRE(waitFor([&]
                    { return model->state() == ModelState::Ready; }));
    CHECK(model->columnName(1) == "c1");

    // Jump to the middle: blocks near the start are never read.
    model->requestRange(50000, 50020);
    CHECK(cellText(*model, 50010, 2) == "500102");
    CHECK(waitFor([&]
                  { return model->rowCount() == RowCount{100000, RowCount::Kind::Exact}; }));
    CHECK(stats->counts == 1);
    {
        std::lock_guard lock(stats->mutex);
        CHECK_FALSE(stats->read_rows.contains(10000));
    }
    CHECK(model->peek(100000, 0).state == CellState::OutOfRange);
    CHECK(model->peek(50010, 7).state == CellState::Empty);
}

TEST_CASE("events are delivered and subscriptions stop them")
{
    auto data = std::make_unique<FakeData>();
    TableBook book(std::move(data));
    auto model = book.sheet("a");

    std::atomic<int> rows_loaded{0};
    auto subscription = model->subscribe([&](const TableEvent &event)
                                         {
        if (event.kind == TableEvent::Kind::RowsLoaded)
            ++rows_loaded; });
    // Not the first rows: the worker loads them without a request, maybe
    // before the subscription, and then requesting them emits nothing.
    model->requestRange(50000, 50010);
    REQUIRE(waitFor([&]
                    { return rows_loaded > 0; }));
    subscription.reset();
    const int seen = rows_loaded;
    model->requestRange(90000, 90010);
    waitForCell(*model, 90000, 0);
    CHECK(rows_loaded == seen);
}

TEST_CASE("a listener can unsubscribe itself")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 0;
    TableBook book(std::move(data));
    auto model = book.sheet("a");
    REQUIRE(waitFor([&] { return model->rowCount() == RowCount{0, RowCount::Kind::Exact}; }));

    std::atomic<int> calls{0};
    Subscription subscription;
    subscription = model->subscribe([&](const TableEvent &event)
                                    {
        if (event.kind == TableEvent::Kind::ViewChanged)
        {
            ++calls;
            subscription.reset();
        } });

    model->setView({.sort = {{.column = 0}}});
    REQUIRE(waitFor([&] { return calls == 1; }, std::chrono::seconds(2)));
    model->setView({});
    REQUIRE(waitFor([&] { return model->view().empty(); }));
    CHECK(calls == 1);
}

TEST_CASE("readView on a model whose book is gone fails at once", "[regression]")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 100;
    auto book = std::make_unique<TableBook>(std::move(data), TableModel::Options{});
    auto model = book->sheet("a");
    REQUIRE(waitFor([&] { return model->state() == ModelState::Ready; }));
    // E.g. an export thread that got the model just before the file was reopened.
    book.reset();
    auto reader = model->readView();
    std::stop_source stop;
    std::jthread timeout([&stop](std::stop_token cancelled)
                         {
        for (int i = 0; i < 100 && !cancelled.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.request_stop(); });
    // Used to wait for rows that no worker would ever read.
    CHECK_THROWS_AS(reader->next(stop.get_token()), std::runtime_error);
}

TEST_CASE("LRU cache evicts blocks outside the viewport")
{
    auto data = std::make_unique<FakeData>();
    auto stats = data->stats;
    TableModel::Options options;
    options.block_rows = 100;
    options.cache_bytes = 64 * 1024; // only a handful of blocks fit
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");

    for (size_t row = 0; row < 20000; row += 1000)
    {
        model->requestRange(row, row + 10);
        REQUIRE(waitForCell(*model, row, 0).state == CellState::Ready);
    }
    // The first block was evicted and has to be read again.
    const int reads_before = stats->reads;
    model->requestRange(0, 10);
    CHECK(waitForCell(*model, 0, 0).state == CellState::Ready);
    CHECK(stats->reads > reads_before);
}

TEST_CASE("query pushdown sources apply the view themselves")
{
    auto data = std::make_unique<FakeData>();
    data->pushdown = true;
    auto stats = data->stats;
    TableBook book(std::move(data));
    auto model = book.sheet("a");
    REQUIRE(waitFor([&]
                    { return model->state() == ModelState::Ready; }));
    CHECK(model->viewCapabilities().sort);

    CHECK(cellText(*model, 0, 0) == "0");
    std::atomic<bool> changed{false};
    auto subscription = model->subscribe([&](const TableEvent &event)
                                         {
        if (event.kind == TableEvent::Kind::ViewChanged)
            changed = true; });
    model->setView({.sort = {{.column = 0, .ascending = false}}});
    REQUIRE(waitFor([&]
                    { return changed.load(); }));
    CHECK(cellText(*model, 0, 0) == "999990");
    std::lock_guard lock(stats->mutex);
    REQUIRE(stats->applied.has_value());
    CHECK_FALSE(stats->applied->sort.front().ascending);
}

// Without a row count from the source, rows that fill whole blocks end
// with an empty block; the in-memory view must not wait for a count.
TEST_CASE("in-memory sort before the row count of whole blocks is known")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 1000;
    auto stats = data->stats;
    stats->no_count = true;
    TableModel::Options options;
    options.block_rows = 100;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    REQUIRE(waitFor([&]
                    { return model->viewCapabilities().sort; }));

    model->setView({.sort = {{.column = 0, .ascending = false}}});
    CHECK(waitFor([&]
                  { return !model->view().sort.empty() && model->rowCount() == RowCount{1000, RowCount::Kind::Exact}; }));
    CHECK(cellText(*model, 0, 0) == "9990");
}

TEST_CASE("in-memory sort and filter on small random access tables")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 1000;
    TableModel::Options options;
    options.block_rows = 100;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    REQUIRE(waitFor([&]
                    { return model->viewCapabilities().sort; }));

    model->setView({.sort = {{.column = 0, .ascending = false}},
                    .filter = Filter{.conditions = {{.column = 0, .op = FilterOp::Less, .value = "500"}}}});
    REQUIRE(waitFor([&]
                    { return model->rowCount() == RowCount{50, RowCount::Kind::Exact}; }));
    CHECK(cellText(*model, 0, 0) == "490");
    CHECK(cellText(*model, 49, 0) == "0");
    CHECK(model->peek(50, 0).state == CellState::OutOfRange);

    model->setView({});
    REQUIRE(waitFor([&]
                    { return model->rowCount().value == 1000; }));
    CHECK(cellText(*model, 0, 0) == "0");
}

TEST_CASE("unfilteredRowCount keeps the row count before filtering")
{
    const Filter filter{.conditions = {{.column = 0, .op = FilterOp::Less, .value = "500"}}};

    SECTION("in memory")
    {
        auto data = std::make_unique<FakeData>();
        data->rows = 1000;
        TableBook book(std::move(data));
        auto model = book.sheet("a");
        REQUIRE(waitFor([&] { return model->viewCapabilities().filter; }));
        model->setView({.filter = filter});
        REQUIRE(waitFor([&] { return model->rowCount() == RowCount{50, RowCount::Kind::Exact}; }));
        CHECK(model->unfilteredRowCount() == RowCount{1000, RowCount::Kind::Exact});
    }

    SECTION("pushdown")
    {
        auto data = std::make_unique<FakeData>();
        data->pushdown = true;
        TableBook book(std::move(data));
        auto model = book.sheet("a");
        REQUIRE(waitFor([&] { return model->rowCount() == RowCount{100000, RowCount::Kind::Exact}; }));
        model->setView({.filter = filter});
        REQUIRE(waitFor([&] { return model->rowCount() == RowCount{50000, RowCount::Kind::Exact}; }));
        CHECK(model->unfilteredRowCount() == RowCount{100000, RowCount::Kind::Exact});

        // Another filter still knows the unfiltered count.
        model->setView({.sort = {{0, false}}, .filter = filter});
        REQUIRE(waitFor([&] { return !model->view().sort.empty(); }));
        CHECK(model->unfilteredRowCount() == RowCount{100000, RowCount::Kind::Exact});

        model->setView({});
        REQUIRE(waitFor([&] { return model->rowCount() == RowCount{100000, RowCount::Kind::Exact}; }));
        CHECK(model->unfilteredRowCount() == model->rowCount());
    }
}

TEST_CASE("search reports hits in view order")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 3000;
    TableModel::Options options;
    options.block_rows = 100;
    TableBook book(std::move(data), options);
    auto model = book.sheet("b");

    model->startSearch({.text = "2999", .column = std::nullopt});
    REQUIRE(waitFor([&]
                    { return model->searchState().done; }));
    const auto state = model->searchState();
    // Cells hold r * 10 + c (c < 3), so "2999" only occurs in 29990..29992 (row 2999).
    REQUIRE(state.hits.size() == 3);
    CHECK(state.hits[0] == CellPosition{2999, 0});
    CHECK(state.scanned_rows == 3000);

    model->cancelSearch();
    CHECK_FALSE(model->searchState().active);
}

TEST_CASE("a search restarted by a sort gets a new generation", "[regression]")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 3000;
    TableModel::Options options;
    options.block_rows = 100;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    REQUIRE(waitFor([&] { return model->rowCount().isExact(); }));

    model->startSearch({.text = "2999", .column = std::nullopt});
    REQUIRE(waitFor([&] { return model->searchState().done; }));
    const SearchState before = model->searchState();
    REQUIRE(before.hits.size() == 3);

    // The same hits in other rows: only the generation shows the restart.
    model->setView({.sort = {{0, false}}});
    REQUIRE(waitFor([&] { return !model->view().empty() && model->searchState().done; }));
    const SearchState after = model->searchState();
    CHECK(after.hits.size() == before.hits.size());
    CHECK(after.hits[0].row != before.hits[0].row);
    CHECK(after.generation != before.generation);
}

TEST_CASE("column statistics scan the view and restart with it", "[stats]")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 3000;
    TableModel::Options options;
    options.block_rows = 100;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    std::atomic<int> progress{0};
    auto subscription = model->subscribe([&](const TableEvent &event)
                                         {
        if (event.kind == TableEvent::Kind::StatsProgress)
            ++progress; });
    REQUIRE(waitFor([&] { return model->viewCapabilities().filter; }));

    // Cells hold r * 10 + c.
    model->startColumnStats(1);
    REQUIRE(waitFor([&] { return model->columnStatsState().done; }));
    const ColumnStatsState state = model->columnStatsState();
    CHECK(state.active);
    CHECK(state.column == 1);
    CHECK(state.scanned_rows == 3000);
    CHECK(progress > 0);
    CHECK(state.stats.rows == 3000);
    CHECK(state.stats.empty == 0);
    CHECK(state.stats.distinct == 3000);
    CHECK(state.stats.numbers == 3000);
    CHECK(state.stats.min == CellValue{int64_t{1}});
    CHECK(state.stats.max == CellValue{int64_t{29991}});
    CHECK(state.stats.mean == 14996.0);

    // Only the filtered rows count.
    model->setView({.filter = Filter{.conditions = {{0, FilterOp::Less, "500"}}}});
    REQUIRE(waitFor([&] { return model->view().filtered() && model->columnStatsState().done; }));
    const ColumnStatsState filtered = model->columnStatsState();
    CHECK(filtered.generation != state.generation);
    CHECK(filtered.column == 1);
    CHECK(filtered.scanned_rows == 50);
    CHECK(filtered.stats.rows == 50);
    CHECK(filtered.stats.max == CellValue{int64_t{491}});

    // Another column, then nothing.
    model->startColumnStats(7);
    REQUIRE(waitFor([&] { return model->columnStatsState().done; }));
    CHECK(model->columnStatsState().stats.empty == 50);
    model->cancelColumnStats();
    CHECK_FALSE(model->columnStatsState().active);
}

TEST_CASE("column statistics finish when the cache cannot hold a block", "[stats]")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 1000;
    data->pushdown = GENERATE(false, true);
    auto stats = data->stats;
    TableModel::Options options;
    options.block_rows = 100;
    options.cache_bytes = 1;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    model->startColumnStats(0);
    REQUIRE(waitFor([&] { return model->columnStatsState().done; }, std::chrono::seconds(2)));
    const auto result = model->columnStatsState();
    CHECK(result.scanned_rows == 1000);
    CHECK(result.stats.rows == 1000);
    CHECK(result.stats.max == CellValue{int64_t{9990}});
    CHECK(stats->reads <= 12);
}

TEST_CASE("pushdown sources count values themselves, or the rows are scanned", "[stats]")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 1000;
    data->pushdown = true;
    data->stats->value_counts = true;
    const bool fail = GENERATE(false, true);
    data->stats->fail_value_counts = fail;
    auto stats = data->stats;
    TableModel::Options options;
    options.block_rows = 100;
    options.stats_limits.max_distinct = 10;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    REQUIRE(waitFor([&] { return model->state() == ModelState::Ready; }));
    // Ready comes before any row is read: let the first block and the one
    // read ahead arrive, so that only the statistics can read more.
    REQUIRE(waitForCell(*model, 2 * options.block_rows - 1, 0).state == CellState::Ready);
    const int reads_before = stats->reads;

    model->startColumnStats(1);
    REQUIRE(waitFor([&] { return model->columnStatsState().done; }));
    const ColumnStatsState state = model->columnStatsState();
    CHECK(stats->value_count_calls == 1);
    CHECK(state.scanned_rows == 1000);
    // A failure leaves nothing of what was added before it.
    CHECK(state.stats.rows == 1000);
    CHECK(state.stats.min == CellValue{int64_t{1}});
    CHECK(state.stats.max == CellValue{int64_t{9991}});
    if (fail)
    {
        CHECK(stats->reads > reads_before);
        CHECK_FALSE(state.stats.distinct_exact);
    }
    else
    {
        CHECK(stats->reads == reads_before);
        // The source knows the exact count beyond the accumulator's limit.
        CHECK(state.stats.distinct == 1000);
        CHECK(state.stats.distinct_exact);
    }
}

TEST_CASE("column statistics and a search run side by side", "[stats]")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 2000;
    TableModel::Options options;
    options.block_rows = 100;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    model->startSearch({.text = "1999", .column = std::nullopt});
    model->startColumnStats(2);
    REQUIRE(waitFor([&] { return model->searchState().done && model->columnStatsState().done; }));
    CHECK(model->searchState().hits.size() == 3);
    CHECK(model->columnStatsState().stats.rows == 2000);
}

TEST_CASE("search with a regular expression")
{
    auto data = std::make_unique<FakeData>();
    data->cols = 2;
    data->values = std::make_shared<const std::vector<TableRow>>(std::vector<TableRow>{
        {std::string("Apple pie"), std::string("banana")},
        {std::string("apple"), int64_t{42}},
        {std::string("Cherry"), std::string("日本語テキスト")},
    });
    TableBook book(std::move(data));
    auto model = book.sheet("a");
    auto search = [&](SearchSpec spec)
    {
        model->startSearch(std::move(spec));
        REQUIRE(waitFor([&]
                        { return model->searchState().done; }));
        return model->searchState();
    };
    using Hits = std::vector<CellPosition>;

    // Anchors apply to the whole cell text; case-insensitive by default.
    CHECK(search({.text = "^apple$", .regex = true}).hits == Hits{{1, 0}});
    CHECK(search({.text = "^APPLE", .regex = true}).hits == Hits{{0, 0}, {1, 0}});
    CHECK(search({.text = "^APPLE", .case_sensitive = true, .regex = true}).hits.empty());
    // Numbers are matched as displayed.
    CHECK(search({.text = "^\\d+$", .regex = true}).hits == Hits{{1, 1}});
    // "." matches one UTF-8 character.
    CHECK(search({.text = "^日.語", .regex = true}).hits == Hits{{2, 1}});
    // Without regex, metacharacters are plain text.
    CHECK(search({.text = "^apple"}).hits.empty());

    const auto invalid = search({.text = "(", .regex = true});
    CHECK(invalid.active);
    CHECK(invalid.hits.empty());
    CHECK(invalid.error.find("Invalid regular expression") != std::string::npos);
    // The next search starts cleanly.
    const auto next = search({.text = "an+a", .regex = true});
    CHECK(next.error.empty());
    CHECK(next.hits == Hits{{0, 1}});
}

TEST_CASE("search finishes when the cache cannot hold a block", "[regression]")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 1000;
    auto stats = data->stats;
    TableModel::Options options;
    options.block_rows = 100;
    options.cache_bytes = 1;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    model->startSearch({.text = "999", .column = 0});
    REQUIRE(waitFor([&] { return model->searchState().done; }, std::chrono::seconds(2)));
    const auto result = model->searchState();
    CHECK(result.scanned_rows == 1000);
    REQUIRE(result.hits == std::vector<CellPosition>{{999, 0}});
    CHECK(stats->reads <= 12);
}

TEST_CASE("sort and filter finish with a cache smaller than the table", "[regression]")
{
    auto data = std::make_unique<FakeData>();
    data->rows = 1000;
    auto stats = data->stats;
    TableModel::Options options;
    options.block_rows = 100;
    options.cache_bytes = 1;
    TableBook book(std::move(data), options);
    auto model = book.sheet("a");
    REQUIRE(waitFor([&] { return model->viewCapabilities().sort; }));
    const ViewSpec spec{.sort = {{0, false}},
                        .filter = Filter{.conditions = {{0, FilterOp::GreaterEqual, "9500"}}}};
    model->setView(spec);
    REQUIRE(waitFor([&] { return model->view() == spec; }, std::chrono::seconds(2)));
    CHECK(model->rowCount() == RowCount{50, RowCount::Kind::Exact});
    CHECK(cellText(*model, 0, 0) == "9990");
    CHECK(cellText(*model, 49, 0) == "9500");
    CHECK(stats->reads <= 12);
    model->setView({});
    REQUIRE(waitFor([&] { return model->view().empty(); }));
    model->requestRange(500, 510);
    CHECK(cellText(*model, 500, 0) == "5000");
    CHECK(stats->reads > 10); // clearing the view restores eviction
}

TEST_CASE("a newer view survives an in-flight pushdown", "[regression]")
{
    auto data = std::make_unique<FakeData>();
    data->pushdown = true;
    auto stats = data->stats;
    stats->pause_first_view = true;
    TableBook book(std::move(data));
    auto model = book.sheet("a");
    model->setView({.sort = {{0, false}}});
    const bool entered = waitFor([&] { return stats->view_entered.load(); }, std::chrono::seconds(1));
    const ViewSpec latest{.sort = {{0, true}}};
    model->setView(latest);
    stats->release_view = true;
    REQUIRE(entered);
    REQUIRE(waitFor([&] { return model->view() == latest; }, std::chrono::seconds(2)));
    CHECK(stats->view_applies == 2);
    CHECK(cellText(*model, 0, 0) == "0");
}

TEST_CASE("requestedView reports a pending view until it is applied or fails")
{
    SECTION("pending pushdown")
    {
        auto data = std::make_unique<FakeData>();
        data->pushdown = true;
        auto stats = data->stats;
        stats->pause_first_view = true;
        TableBook book(std::move(data));
        auto model = book.sheet("a");
        const ViewSpec descending{.sort = {{0, false}}};
        model->setView(descending);
        REQUIRE(waitFor([&] { return stats->view_entered.load(); }, std::chrono::seconds(1)));
        CHECK(model->view() == ViewSpec{});
        CHECK(model->requestedView() == descending);
        stats->release_view = true;
        REQUIRE(waitFor([&] { return model->view() == descending; }, std::chrono::seconds(2)));
        CHECK(model->requestedView() == descending);
    }
    SECTION("any number of rows sorts in memory")
    {
        auto data = std::make_unique<FakeData>(); // 100000 rows, no pushdown
        TableBook book(std::move(data));
        auto model = book.sheet("a");
        REQUIRE(waitFor([&] { return model->state() == ModelState::Ready; }));
        CHECK(model->viewCapabilities().sort);
        const ViewSpec descending{.sort = {{0, false}}};
        model->setView(descending);
        REQUIRE(waitFor([&] { return model->view() == descending; }));
        CHECK(model->rowCount() == RowCount{100000, RowCount::Kind::Exact});
        CHECK(cellText(*model, 0, 0) == "999990"); // row * 10
    }
}

TEST_CASE("a failing pushed-down view is cleared instead of breaking the sheet", "[regression]")
{
    auto data = std::make_unique<FakeData>();
    data->pushdown = true;
    auto stats = data->stats;
    const bool fail_on_read = GENERATE(false, true);
    (fail_on_read ? stats->fail_filtered_reads : stats->fail_filter_apply) = true;
    TableBook book(std::move(data));
    auto model = book.sheet("a");
    REQUIRE(waitFor([&] { return model->state() == ModelState::Ready; }));
    CHECK(cellText(*model, 0, 0) == "0");

    const Filter filter{.conditions = {{.column = 0, .op = FilterOp::Equals, .value = "10"}}};
    model->setView({.filter = filter});
    std::optional<std::string> error;
    REQUIRE(waitFor([&]
                    {
        if (!error)
            error = model->takeViewError();
        return error && model->requestedView() == ViewSpec{} && model->view() == ViewSpec{}; }));
    CHECK(model->state() == ModelState::Ready);
    CHECK_FALSE(model->takeViewError().has_value());
    CHECK(cellText(*model, 0, 0) == "0");

    // The sheet still accepts other views.
    model->setView({.sort = {{0, false}}});
    REQUIRE(waitFor([&] { return model->view().sort.size() == 1; }));
    CHECK(cellText(*model, 0, 0) == "999990");
    CHECK(model->state() == ModelState::Ready);
}

TEST_CASE("sort and filter preserve large integer precision", "[regression]")
{
    auto data = std::make_unique<FakeData>();
    data->cols = 2;
    data->values = std::make_shared<const std::vector<TableRow>>(std::vector<TableRow>{
        {int64_t{9007199254740993LL}, std::string("high")},
        {int64_t{9007199254740992LL}, std::string("low")},
        {std::numeric_limits<int64_t>::max(), std::string("max")},
        {std::numeric_limits<int64_t>::min(), std::string("min")},
        {std::string("9007199254740993"), std::string("high-text")},
        {std::string(" +9007199254740992 "), std::string("low-text")},
        {9223372036854775808.0, std::string("double-above-max")},
        {-9223372036854775808.0, std::string("double-at-min")},
        {1.5, std::string("fraction")},
        {int64_t{1}, std::string("one")},
    });
    TableBook book(std::move(data));
    auto model = book.sheet("a");
    REQUIRE(waitFor([&] { return model->viewCapabilities().sort; }));
    const ViewSpec sorted{.sort = {{0, true}}};
    model->setView(sorted);
    REQUIRE(waitFor([&] { return model->view() == sorted; }));
    const std::vector<std::string> expected{"min", "double-at-min", "one", "fraction", "low", "low-text",
                                            "high", "high-text", "max", "double-above-max"};
    for (size_t r = 0; r < expected.size(); ++r)
        CHECK(cellText(*model, r, 1) == expected[r]);

    for (const auto op : {FilterOp::Equals, FilterOp::Less, FilterOp::Greater})
    {
        const ViewSpec filtered{.sort = {{0, true}},
                                .filter = Filter{.conditions = {{0, op, "9007199254740992"}}}};
        model->setView(filtered);
        REQUIRE(waitFor([&] { return model->view() == filtered; }));
        if (op == FilterOp::Equals)
        {
            CHECK(model->rowCount().value == 2);
            CHECK(cellText(*model, 0, 1) == "low");
            CHECK(cellText(*model, 1, 1) == "low-text");
        }
        else
        {
            CHECK(model->rowCount().value == 4);
            CHECK(cellText(*model, 0, 1) == (op == FilterOp::Less ? "min" : "high"));
        }
    }
}

TEST_CASE("sort compares dates and date-times chronologically", "[regression]")
{
    auto data = std::make_unique<FakeData>();
    data->cols = 2;
    data->values = std::make_shared<const std::vector<TableRow>>(std::vector<TableRow>{
        {Date{1}, std::string("next-day")},
        {DateTime{1}, std::string("after-midnight")},
        {DateTime{-1}, std::string("before-midnight")},
        {Date{0}, std::string("epoch-date")},
        {DateTime{0}, std::string("epoch-date-time")},
    });
    TableBook book(std::move(data));
    auto model = book.sheet("a");
    REQUIRE(waitFor([&] { return model->viewCapabilities().sort; }));

    const ViewSpec sorted{.sort = {{0, true}}};
    model->setView(sorted);
    REQUIRE(waitFor([&] { return model->view() == sorted; }));
    const std::vector<std::string> expected{
        "before-midnight", "epoch-date", "epoch-date-time", "after-midnight", "next-day"};
    for (size_t row = 0; row < expected.size(); ++row)
        CHECK(cellText(*model, row, 1) == expected[row]);
}

TEST_CASE("in-memory sort orders as compareCells() does")
{
    // Every kind of cell, ties included, in both directions and by two keys.
    const std::vector<CellValue> kinds{
        CellValue{}, std::string(), std::string("b"), std::string("a"), std::string("B"), std::string(" 2 "),
        std::string("+3"), std::string("2.5"), std::string("1e400"), std::string("nan"), int64_t{2}, int64_t{-7},
        std::numeric_limits<int64_t>::max(), 2.0, 2.5, -0.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), true, false, Date{0}, Date{1}, Date{-1}, DateTime{0},
        DateTime{1}, DateTime{-1}, DateTime{86400LL * 1000000}, Date{2}, std::string("1970-01-01"),
    };
    std::vector<TableRow> values;
    for (size_t i = 0; i < kinds.size() * 3; ++i)
        values.push_back({kinds[(i * 7) % kinds.size()], kinds[(i * 5 + 3) % kinds.size()], int64_t(i)});
    auto data = std::make_unique<FakeData>();
    data->values = std::make_shared<const std::vector<TableRow>>(values);
    TableBook book(std::move(data));
    auto model = book.sheet("a");
    REQUIRE(waitFor([&] { return model->viewCapabilities().sort; }));

    auto isEmpty = [](const CellValue &value)
    {
        const auto *text = std::get_if<std::string>(&value);
        return std::holds_alternative<std::monostate>(value) || (text && text->empty());
    };
    for (const bool ascending : {true, false})
    {
        const ViewSpec spec{.sort = {{0, ascending}, {1, !ascending}}};
        std::vector<TableRow> expected = values;
        std::stable_sort(expected.begin(), expected.end(), [&](const TableRow &a, const TableRow &b)
                         {
            for (const SortKey &key : spec.sort)
            {
                const int cmp = compareCells(a[key.column], b[key.column]);
                if (cmp != 0)
                {
                    if (isEmpty(a[key.column]) || isEmpty(b[key.column]))
                        return isEmpty(b[key.column]);
                    return key.ascending ? cmp < 0 : cmp > 0;
                }
            }
            return false; });
        model->setView(spec);
        REQUIRE(waitFor([&] { return model->view() == spec; }));
        for (size_t r = 0; r < expected.size(); ++r)
        {
            INFO("row " << r);
            CHECK(cellText(*model, r, 2) == formatCell(expected[r][2]));
        }
    }
}

TEST_CASE("block size must be positive")
{
    TableModel::Options options;
    options.block_rows = 0;
    CHECK_THROWS_AS((TableBook(std::make_unique<FakeData>(), options)), std::invalid_argument);
}

TEST_CASE("destroying a book while loading does not hang")
{
    for (int i = 0; i < 20; ++i)
    {
        auto data = std::make_unique<FakeData>();
        auto book = std::make_unique<TableBook>(std::move(data), TableModel::Options{});
        auto model = book->sheet("a");
        model->requestRange(static_cast<size_t>(i) * 1000, static_cast<size_t>(i) * 1000 + 50);
        book.reset();
        // The model outlives the book and stays usable (without loading more).
        CHECK(model->peek(0, 0).state != CellState::Error);
    }
}

TEST_CASE("header heuristic")
{
    using S = std::string;
    const TableRow header{S("Name"), S("Value")};
    CHECK(looksLikeHeaderRow(header));
    CHECK_FALSE(looksLikeHeaderRow(TableRow{S("Name"), S("")}));
    CHECK_FALSE(looksLikeHeaderRow(TableRow{S("Name"), S("12")}));
    CHECK_FALSE(looksLikeHeaderRow(TableRow{S("A"), S("A")}));
    CHECK_FALSE(looksLikeHeaderRow(TableRow{S("Name"), int64_t{1}}));

    // A lone title above a wider table is not a header.
    const TableRow title{S("Quarterly report")};
    const TableRow wide{S("x"), S("y"), S("z")};
    const TableRow narrow{S("x")};
    CHECK_FALSE(looksLikeHeaderRow(title, &wide));
    CHECK(looksLikeHeaderRow(title, &narrow));
    CHECK(looksLikeHeaderRow(title));
    CHECK(looksLikeHeaderRow(header, &wide));
}

TEST_CASE("only text and Excel formats use the header option")
{
    const std::filesystem::path demo = TABLESKIM_DEMO_DATA_DIR;
    CHECK(TableBook::open(demo / "normal.csv")->usesHeaderOption());
    CHECK(TableBook::open(demo / "normal.csv.gz")->usesHeaderOption());
    CHECK(TableBook::open(demo / "normal.xlsx")->usesHeaderOption());
    CHECK_FALSE(TableBook::open(demo / "normal.parquet")->usesHeaderOption());
    CHECK_FALSE(TableBook::open(demo / "demo.duckdb")->usesHeaderOption());
    CHECK_FALSE(TableBook::open(demo / "demo.sqlite3")->usesHeaderOption());
    CHECK_FALSE(TableBook(std::make_unique<FakeData>()).usesHeaderOption());
}

TEST_CASE("searchHitIndex finds the hit at a position")
{
    SearchState state;
    state.active = true;
    state.done = true;
    state.hits = {{1, 2}, {3, 0}, {3, 4}};
    CHECK(searchHitIndex(state, {3, 0}) == 1u);
    CHECK(searchHitIndex(state, {1, 2}) == 0u);
    CHECK_FALSE(searchHitIndex(state, {3, 1}));
    CHECK_FALSE(searchHitIndex(state, {9, 9}));
    CHECK_FALSE(searchHitIndex(SearchState{}, {0, 0}));
}
