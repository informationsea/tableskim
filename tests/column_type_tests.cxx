#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>

#include "column_type.hxx"
#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    ColumnType typeOf(const std::vector<CellValue> &values)
    {
        ColumnTypeCounter counter;
        for (const auto &value : values)
            counter.add(value);
        return counter.type();
    }
} // namespace

TEST_CASE("ColumnTypeCounter: the kind of most values")
{
    using S = std::string;
    CHECK(typeOf({}).kind == ColumnKind::Unknown);
    CHECK(typeOf({S(""), std::monostate{}}).kind == ColumnKind::Unknown);

    const ColumnType integers = typeOf({S("1"), S(" 42 "), int64_t{-3}, 7.0, S("")});
    CHECK(integers.kind == ColumnKind::Number);
    CHECK(integers.integers);
    CHECK(integers.values == 4);
    const ColumnType decimals = typeOf({S("1"), S("2.0"), int64_t{3}});
    CHECK(decimals.kind == ColumnKind::Number);
    CHECK_FALSE(decimals.integers);

    CHECK(typeOf({S("2024-01-31"), Date{0}}).kind == ColumnKind::Date);
    CHECK(typeOf({S("2024-01-31"), S("2024-01-31 12:00")}).kind == ColumnKind::DateTime);
    CHECK(typeOf({DateTime{0}}).kind == ColumnKind::DateTime);
    CHECK(typeOf({true, S("FALSE"), S(" true ")}).kind == ColumnKind::Boolean);
    CHECK(typeOf({S("1"), S("0")}).kind == ColumnKind::Number);
    CHECK(typeOf({S("Tokyo"), S("1")}).kind == ColumnKind::Mixed);
    CHECK(typeOf({S("Tokyo"), S("Osaka")}).kind == ColumnKind::Text);

    // A stray "N/A" among numbers keeps it a number column.
    std::vector<CellValue> mostly(9, CellValue{S("5")});
    mostly.push_back(S("N/A"));
    CHECK(typeOf(mostly).kind == ColumnKind::Number);
    mostly.push_back(S("-"));
    CHECK(typeOf(mostly).kind == ColumnKind::Mixed);
}

TEST_CASE("columnKindMark and columnTypeText")
{
    CHECK(std::string(columnKindMark(ColumnKind::Number)) == "#");
    CHECK(std::string(columnKindMark(ColumnKind::Date)) == "@");
    CHECK(std::string(columnKindMark(ColumnKind::DateTime)) == "@");
    CHECK(std::string(columnKindMark(ColumnKind::Boolean)) == "?");
    CHECK(std::string(columnKindMark(ColumnKind::Text)).empty());
    CHECK(std::string(columnKindMark(ColumnKind::Mixed)).empty());
    CHECK(columnTypeText({ColumnKind::Number, true, 3}) == "Number (whole numbers)");
    CHECK(columnTypeText({ColumnKind::DateTime, false, 3}) == "Date and time");
}

TEST_CASE("TableModel::columnTypes: judged from the first rows read")
{
    const auto file = GENERATE(as<std::string>{}, "normal.csv", "normal.xlsx", "normal.parquet");
    INFO(file);
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    auto book = TableBook::open(kDemoData / file, options);
    auto model = book->sheet(book->sheetNames().front());
    REQUIRE(waitFor([&]
                    { return model->state() == ModelState::Ready; }));
    model->requestRange(0, 10);
    std::vector<ColumnType> types;
    REQUIRE(waitFor([&]
                    { types = model->columnTypes(); return types.size() == 9 && types[0].values > 0; }));
    // ID, Name, Email, Age, City, Department, Salary, Birthday, Active
    CHECK(types[0].kind == ColumnKind::Number);
    CHECK(types[0].integers);
    CHECK(types[1].kind == ColumnKind::Text);
    CHECK(types[3].kind == ColumnKind::Number);
    CHECK(types[4].kind == ColumnKind::Text);
    CHECK(types[6].kind == ColumnKind::Number);
    CHECK_FALSE(types[6].integers);
    CHECK((types[7].kind == ColumnKind::Date || types[7].kind == ColumnKind::DateTime));
    CHECK(types[8].kind == ColumnKind::Boolean);
}

TEST_CASE("TableModel::columnTypes: at most type_sample_rows rows")
{
    // Text after the first rows does not count.
    auto path = std::filesystem::temp_directory_path() / "tableskim-test-column-types.csv";
    {
        std::ofstream out(path, std::ios::binary);
        out << "n\n";
        for (int i = 0; i < 3000; ++i)
            out << (i < 1500 ? std::to_string(i) : std::string("text")) << "\n";
    }
    OpenOptions options;
    options.header = OpenOptions::Header::Yes;
    auto book = TableBook::open(path, options);
    auto model = book->sheet(book->sheetNames().front());
    REQUIRE(waitFor([&]
                    { return model->state() == ModelState::Ready; }));
    model->requestRange(0, 2999);
    REQUIRE(waitFor([&]
                    { return model->rowCount().isExact(); }));
    const auto types = model->columnTypes();
    REQUIRE(types.size() == 1);
    CHECK(types[0].kind == ColumnKind::Number);
    CHECK(types[0].values == 1000);
    book.reset();
    model.reset();
    std::filesystem::remove(path);
}
