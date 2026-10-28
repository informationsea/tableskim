#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "test_helpers.hxx"
#include "web_json.hxx"
#include "web_session.hxx"

namespace
{
    const std::string DEMO = TABLESKIM_DEMO_DATA_DIR;

    // Sends one request and returns the parsed reply.
    class Client
    {
    public:
        explicit Client(WebSession &session) : session_(session) {}

        JsonValue call(const std::string &method, const std::string &params = "{}")
        {
            const int id = ++next_id_;
            const JsonValue reply = JsonValue::parse(
                session_.handle("{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\",\"params\":" + params + "}"));
            REQUIRE(reply["id"].number() == id);
            return reply;
        }

        // The result, failing the test on an error reply.
        JsonValue result(const std::string &method, const std::string &params = "{}")
        {
            const JsonValue reply = call(method, params);
            INFO(method << ": " << (reply["error"].string() ? *reply["error"].string() : ""));
            REQUIRE(reply["error"].isNull());
            return reply["result"];
        }

        // Rows [first, last] once they are loaded.
        JsonValue rows(int sheet, size_t first, size_t last)
        {
            JsonValue rows;
            const std::string params = "{\"sheet\":" + std::to_string(sheet) + ",\"first\":" + std::to_string(first) +
                                       ",\"last\":" + std::to_string(last) + "}";
            REQUIRE(waitFor([&]
                            { rows = result("rows", params); return !*rows["loading"].boolean(); }));
            return rows;
        }

        JsonValue readyInfo(int sheet)
        {
            JsonValue info;
            REQUIRE(waitFor([&]
                            { info = result("info", "{\"sheet\":" + std::to_string(sheet) + "}");
                              return *info["state"].string() != "opening" && !*info["pending"].boolean() &&
                                     *info["rowsExact"].boolean(); }));
            return info;
        }

    private:
        WebSession &session_;
        int next_id_ = 0;
    };

    std::string cell(const JsonValue &rows, size_t row, size_t column)
    {
        return *(*(*rows["rows"].array())[row].array())[column].string();
    }

    std::string path(const std::string &name)
    {
        // JSON-escaped; the demo directory has no quotes or backslashes on
        // the systems the tests run on, but Windows paths use backslashes.
        std::string escaped;
        for (const char c : DEMO + "/" + name)
        {
            if (c == '\\' || c == '"')
                escaped += '\\';
            escaped += c;
        }
        return escaped;
    }
}

TEST_CASE("JSON values round-trip through the reader and writer")
{
    JsonWriter writer;
    writer.beginObject()
        .key("text")
        .value("a \"quoted\"\ttab\n\x01 日本")
        .key("list")
        .beginArray()
        .value(1.5)
        .value(size_t{42})
        .value(true)
        .null()
        .endArray()
        .key("empty")
        .beginObject()
        .endObject()
        .endObject();
    const JsonValue value = JsonValue::parse(writer.text());
    CHECK(*value["text"].string() == "a \"quoted\"\ttab\n\x01 日本");
    const auto &list = *value["list"].array();
    REQUIRE(list.size() == 4);
    CHECK(list[0].number() == 1.5);
    CHECK(list[1].number() == 42);
    CHECK(list[2].boolean() == true);
    CHECK(list[3].isNull());
    CHECK(value["empty"].object()->empty());
    CHECK(value["missing"].isNull());

    CHECK(*JsonValue::parse(R"("é😀\ud800")").string() == "é😀\xef\xbf\xbd");
    CHECK_THROWS(JsonValue::parse("{\"a\":}"));
    CHECK_THROWS(JsonValue::parse("[1,2"));
    CHECK_THROWS(JsonValue::parse("1 2"));
}

TEST_CASE("WebSession opens a CSV file and pages through its rows")
{
    std::atomic<unsigned> events = 0;
    WebSession session([&](int, unsigned bits)
                       { events |= bits; });
    Client client(session);

    const JsonValue opened = client.result("open", "{\"path\":\"" + path("normal.csv") + "\"}");
    REQUIRE(opened["sheets"].array()->size() == 1);
    CHECK(*opened["usesHeaderOption"].boolean());
    CHECK(*opened["textEncoding"].string() == "UTF-8");

    const JsonValue sheet = client.result("sheet", "{\"name\":\"normal.csv\"}");
    const int id = static_cast<int>(*sheet["sheet"].number());
    const JsonValue info = client.readyInfo(id);
    CHECK(*info["rows"].number() == 54);
    CHECK(*info["rowsText"].string() == "54 rows");
    const auto &columns = *info["columns"].array();
    REQUIRE(columns.size() == 9);
    CHECK(*columns[1]["name"].string() == "Name");
    CHECK(*columns[0]["mark"].string() == "#");
    CHECK((events & WebSession::ROWS) != 0);

    const JsonValue rows = client.rows(id, 0, 2);
    REQUIRE(rows["rows"].array()->size() == 3);
    CHECK(cell(rows, 0, 1) == "Sakura Brown");
    CHECK(*(*rows["kinds"].array())[0].string() == "nssnssnss");

    SECTION("the rows stop at the end of the table")
    {
        const JsonValue tail = client.rows(id, 50, 60);
        CHECK(tail["rows"].array()->size() == 4);
        CHECK(*tail["end"].boolean());
    }

    SECTION("a column range")
    {
        const JsonValue part = client.result("rows", "{\"sheet\":" + std::to_string(id) +
                                                         ",\"first\":0,\"last\":0,\"firstColumn\":4,\"lastColumn\":5}");
        REQUIRE((*part["rows"].array())[0].array()->size() == 2);
        CHECK(cell(part, 0, 0) == "Osaka");
    }

    SECTION("sorting by a column header cycles")
    {
        client.result("sortCycle", "{\"sheet\":" + std::to_string(id) + ",\"column\":0}");
        client.result("sortCycle", "{\"sheet\":" + std::to_string(id) + ",\"column\":0}");
        const JsonValue sorted = client.readyInfo(id);
        const auto &sort = *sorted["view"]["sort"].array();
        REQUIRE(sort.size() == 1);
        CHECK(sort[0]["ascending"].boolean() == false);
        CHECK(cell(client.rows(id, 0, 0), 0, 0) == "54");
    }

    SECTION("filtering")
    {
        client.result("setView", "{\"sheet\":" + std::to_string(id) +
                                     ",\"sort\":[],\"filter\":{\"combine\":\"and\",\"conditions\":[{\"column\":4,\"op\":\"equals\",\"value\":\"Osaka\"}]}}");
        const JsonValue filtered = client.readyInfo(id);
        CHECK(*filtered["rows"].number() == 4);
        CHECK(*filtered["rowsText"].string() == "4 rows of 54");
        CHECK(*filtered["filterText"].string() == "City = \"Osaka\"");
        const JsonValue osaka = client.rows(id, 0, 3);
        for (size_t row = 0; row < 4; ++row)
            CHECK(cell(osaka, row, 4) == "Osaka");

        const JsonValue bad = client.call("setView", "{\"sheet\":" + std::to_string(id) +
                                                         ",\"filter\":{\"conditions\":[{\"column\":1,\"op\":\"matches\",\"value\":\"(\"}]}}");
        CHECK(bad["error"].string());
    }

    SECTION("searching")
    {
        client.result("search", "{\"sheet\":" + std::to_string(id) + ",\"text\":\"tokyo\"}");
        JsonValue state;
        REQUIRE(waitFor([&]
                        { state = client.result("searchState", "{\"sheet\":" + std::to_string(id) + "}");
                          return *state["done"].boolean(); }));
        CHECK(*state["hits"].number() == 7);
        const JsonValue hit = client.result("findHit", "{\"sheet\":" + std::to_string(id) +
                                                           ",\"row\":0,\"column\":0,\"forward\":true,\"includeFrom\":true}");
        REQUIRE(hit["index"].number() == 0);
        CHECK(cell(client.rows(id, static_cast<size_t>(*hit["row"].number()), static_cast<size_t>(*hit["row"].number())), 0,
                   static_cast<size_t>(*hit["column"].number())) == "Tokyo");
        const JsonValue back = client.result("findHit", "{\"sheet\":" + std::to_string(id) +
                                                            ",\"row\":0,\"column\":0,\"forward\":false}");
        CHECK(*back["wrapped"].boolean());
        CHECK(back["index"].number() == 6);
    }

    SECTION("copying")
    {
        client.rows(id, 0, 1);
        const JsonValue copied = client.result("copy", "{\"sheet\":" + std::to_string(id) +
                                                           ",\"top\":0,\"left\":0,\"bottom\":1,\"right\":1}");
        CHECK(*copied["text"].string() == "1\tSakura Brown\n2\tHanako Suzuki\n");
    }
}

TEST_CASE("WebSession reports errors without dropping the request id")
{
    WebSession session([](int, unsigned) {});
    Client client(session);
    CHECK(client.call("open", "{\"path\":\"" + path("no-such-file.csv") + "\"}")["error"].string());
    CHECK(client.call("info", "{\"sheet\":99}")["error"].string());
    CHECK(client.call("nonsense")["error"].string());
    CHECK(client.call("rows", "{\"sheet\":1,\"first\":-1}")["error"].string());
    const JsonValue garbage = JsonValue::parse(session.handle("{not json"));
    CHECK(garbage["id"].isNull());
    CHECK(garbage["error"].string());
}

TEST_CASE("WebSession shows each sheet of a workbook and reopens with header options")
{
    WebSession session([](int, unsigned) {});
    Client client(session);
    const JsonValue opened = client.result("open", "{\"path\":\"" + path("demo.sqlite3") + "\"}");
    const auto &sheets = *opened["sheets"].array();
    REQUIRE(sheets.size() > 1);
    CHECK_FALSE(*opened["usesHeaderOption"].boolean());
    const JsonValue first = client.result("sheet", "{\"name\":\"" + *sheets[0].string() + "\"}");
    const JsonValue second = client.result("sheet", "{\"name\":\"" + *sheets[1].string() + "\"}");
    CHECK(*first["sheet"].number() != *second["sheet"].number());
    // The same sheet again keeps its id.
    CHECK(*client.result("sheet", "{\"name\":\"" + *sheets[0].string() + "\"}")["sheet"].number() == *first["sheet"].number());
    client.readyInfo(static_cast<int>(*second["sheet"].number()));

    client.result("open", "{\"path\":\"" + path("normal.csv") + "\",\"header\":\"no\"}");
    // Sheets of the closed book are gone.
    CHECK(client.call("info", "{\"sheet\":" + std::to_string(static_cast<int>(*first["sheet"].number())) + "}")["error"].string());
    const int id = static_cast<int>(*client.result("sheet", "{\"name\":\"normal.csv\"}")["sheet"].number());
    CHECK(*client.readyInfo(id)["rows"].number() == 55);
    CHECK(cell(client.rows(id, 0, 0), 0, 1) == "Name");

    const JsonValue filter_ops = client.result("filterOps");
    const auto &ops = *filter_ops.array();
    CHECK(ops.size() == 14);
    CHECK(*ops[0]["op"].string() == "contains");
}

namespace
{
    std::string sheetParams(int sheet, const std::string &rest = "")
    {
        return "{\"sheet\":" + std::to_string(sheet) + (rest.empty() ? "" : "," + rest) + "}";
    }

    int openSheet(Client &client, const std::string &file, const std::string &name)
    {
        client.result("open", "{\"path\":\"" + path(file) + "\"}");
        const int id = static_cast<int>(*client.result("sheet", "{\"name\":\"" + name + "\"}")["sheet"].number());
        client.readyInfo(id);
        return id;
    }

    // The state of a background computation once it is done.
    JsonValue waitDone(Client &client, const std::string &method, const std::string &params)
    {
        JsonValue state;
        REQUIRE(waitFor([&]
                        { state = client.result(method, params); return state["done"].boolean() == true; }));
        return state;
    }

    std::string statsValue(const JsonValue &state, const std::string &label)
    {
        for (const JsonValue &line : *state["lines"].array())
        {
            if (*line["label"].string() == label)
                return *line["value"].string();
        }
        return "<none>";
    }

    // Applies the conditions a filter request returned as the view's filter.
    void applyConditions(Client &client, int sheet, const JsonValue &conditions)
    {
        JsonWriter json;
        json.beginObject().key("combine").value("and").key("conditions").beginArray();
        for (const JsonValue &condition : *conditions["conditions"].array())
        {
            json.beginObject()
                .key("column")
                .value(*condition["column"].number())
                .key("op")
                .value(*condition["op"].string())
                .key("value")
                .value(*condition["value"].string())
                .endObject();
        }
        json.endArray().endObject();
        client.result("setView", sheetParams(sheet, "\"sort\":[],\"filter\":" + json.text()));
    }

    std::string readFile(const std::string &file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
}

TEST_CASE("WebSession runs SQL and completes it")
{
    WebSession session([](int, unsigned) {});
    Client client(session);
    client.result("open", "{\"path\":\"" + path("demo.sqlite3") + "\"}");

    const JsonValue tables = client.result("sqlSchema");
    bool found = false;
    for (const JsonValue &table : *tables.array())
        found = found || *table["name"].string() == "normal";
    CHECK(found);
    const JsonValue completion = client.result("completeSql", "{\"sql\":\"SELECT * FROM nor\",\"cursor\":17}");
    CHECK(completion["begin"].number() == 14);
    CHECK(*(*completion["candidates"].array())[0].string() == "normal");

    const JsonValue ran =
        client.result("sql", "{\"sql\":\"SELECT City, count(*) AS n FROM normal GROUP BY City ORDER BY n DESC, City\"}");
    const std::string result_sheet = *ran["sheet"].string();
    bool listed = false;
    for (const JsonValue &name : *ran["sheets"].array())
        listed = listed || *name.string() == result_sheet;
    CHECK(listed);
    const int first = static_cast<int>(*client.result("sheet", "{\"name\":\"" + result_sheet + "\"}")["sheet"].number());
    client.readyInfo(first);
    const JsonValue rows = client.rows(first, 0, 0);
    CHECK(cell(rows, 0, 0) == "Fukuoka");
    CHECK(cell(rows, 0, 1) == "9");

    CHECK(client.call("sql", "{\"sql\":\"SELECT nonsense FROM nowhere\"}")["error"].string());

    // Another query replaces the result sheet: its old model is gone.
    client.result("sql", "{\"sql\":\"SELECT 1 AS one\"}");
    CHECK(client.call("info", sheetParams(first))["error"].string());
    const int second = static_cast<int>(*client.result("sheet", "{\"name\":\"" + result_sheet + "\"}")["sheet"].number());
    client.readyInfo(second);
    CHECK(cell(client.rows(second, 0, 0), 0, 0) == "1");
}

TEST_CASE("WebSession computes statistics, histograms and scatter plots")
{
    WebSession session([](int, unsigned) {});
    Client client(session);
    const int id = openSheet(client, "normal.csv", "normal.csv");

    SECTION("statistics")
    {
        client.result("startStats", sheetParams(id, "\"column\":3"));
        const JsonValue stats = waitDone(client, "statsState", sheetParams(id));
        CHECK(stats["column"].number() == 3);
        CHECK(statsValue(stats, "Rows") == "54");
        CHECK(statsValue(stats, "Min") == "20");
        CHECK_FALSE((*stats["top"].array()).empty());
    }

    SECTION("a histogram and the filter of a bin")
    {
        client.result("startHistogram", sheetParams(id, "\"column\":3,\"bins\":10"));
        const JsonValue state = waitDone(client, "histogramState", sheetParams(id));
        const JsonValue &histogram = state["histogram"];
        CHECK(*histogram["kind"].string() == "number");
        const auto &bins = *histogram["bins"].array();
        REQUIRE(bins.size() > 1);
        size_t total = 0;
        for (const JsonValue &bin : bins)
            total += static_cast<size_t>(*bin["count"].number());
        CHECK(total == 53);
        const auto first_count = static_cast<size_t>(*bins[0]["count"].number());
        applyConditions(client, id, client.result("histogramFilter", sheetParams(id, "\"bin\":0")));
        CHECK(*client.readyInfo(id)["rows"].number() == first_count);
    }

    SECTION("the most frequent values of a text column and the filter of one")
    {
        client.result("startHistogram", sheetParams(id, "\"column\":4"));
        const JsonValue state = waitDone(client, "histogramState", sheetParams(id));
        CHECK(*state["histogram"]["kind"].string() == "none");
        const auto &top = *state["histogram"]["top"].array();
        REQUIRE_FALSE(top.empty());
        const auto count = static_cast<size_t>(*top[0]["count"].number());
        applyConditions(client, id, client.result("histogramFilter", sheetParams(id, "\"bin\":0")));
        CHECK(*client.readyInfo(id)["rows"].number() == count);
        CHECK(client.call("histogramFilter", sheetParams(id, "\"bin\":99"))["error"].string());
    }

    SECTION("a scatter plot, a cell's filter and a categorical X")
    {
        client.result("startScatter", sheetParams(id, "\"x\":3,\"y\":6"));
        JsonValue plot = waitDone(client, "scatterState", sheetParams(id, "\"points\":true"));
        CHECK(*plot["plottable"].boolean());
        const auto pairs = static_cast<size_t>(*plot["pairs"].number());
        CHECK(pairs > 40);
        CHECK(plot["points"].array()->size() == 3 * pairs);
        {
            // Every pair is a point here: the line fits them.
            const auto &all = *plot["points"].array();
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t i = 0; i + 2 < all.size(); i += 3)
            {
                const double x = *all[i].number();
                const double y = *all[i + 1].number();
                sx += x;
                sy += y;
                sxx += x * x;
                sxy += x * y;
            }
            const double n = static_cast<double>(pairs);
            const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
            REQUIRE(plot["fit"]["slope"].number());
            CHECK(*plot["fit"]["slope"].number() == Catch::Approx(slope));
            CHECK(*plot["fit"]["intercept"].number() == Catch::Approx((sy - slope * sx) / n));
        }
        const auto y_bins = plot["yAxis"]["edges"].array()->size() - 1;
        const auto &counts = *plot["counts"].array();
        size_t total = 0;
        size_t busiest = 0;
        for (size_t i = 0; i < counts.size(); ++i)
        {
            total += static_cast<size_t>(*counts[i].number());
            if (*counts[i].number() > *counts[busiest].number())
                busiest = i;
        }
        CHECK(total == pairs);
        applyConditions(client, id,
                        client.result("scatterFilter", sheetParams(id, "\"xBin\":" + std::to_string(busiest / y_bins) +
                                                                           ",\"yBin\":" + std::to_string(busiest % y_bins))));
        CHECK(*client.readyInfo(id)["rows"].number() == *counts[busiest].number());

        client.result("setView", sheetParams(id, "\"sort\":[],\"filter\":null"));
        client.readyInfo(id);
        client.result("startScatter", sheetParams(id, "\"x\":3,\"y\":6"));
        plot = waitDone(client, "scatterState", sheetParams(id, "\"points\":true"));
        // A rectangle around every point keeps every pair.
        const auto &points = *plot["points"].array();
        double low[2] = {*points[0].number(), *points[1].number()};
        double high[2] = {low[0], low[1]};
        for (size_t i = 0; i + 2 < points.size(); i += 3)
        {
            for (size_t axis = 0; axis < 2; ++axis)
            {
                low[axis] = std::min(low[axis], *points[i + axis].number());
                high[axis] = std::max(high[axis], *points[i + axis].number());
            }
        }
        applyConditions(client, id,
                        client.result("scatterFilter", sheetParams(id, "\"xLow\":" + std::to_string(low[0] - 1) +
                                                                           ",\"xHigh\":" + std::to_string(high[0] + 1) +
                                                                           ",\"yLow\":" + std::to_string(low[1] - 1) +
                                                                           ",\"yHigh\":" + std::to_string(high[1] + 1))));
        CHECK(*client.readyInfo(id)["rows"].number() == pairs);

        client.result("setView", sheetParams(id, "\"sort\":[],\"filter\":null"));
        client.readyInfo(id);
        client.result("startScatter", sheetParams(id, "\"x\":4,\"y\":3"));
        plot = waitDone(client, "scatterState", sheetParams(id));
        CHECK_FALSE(plot["categories"].array()->empty());
        CHECK(plot["boxes"].array()->size() == plot["categories"].array()->size());
        CHECK(plot["points"].array()->empty()); // not asked for
        CHECK(plot["fit"].isNull()); // categories have no line
    }
}

TEST_CASE("WebSession exports a view and groups it as jobs")
{
    std::atomic<unsigned> session_events = 0;
    WebSession session([&](int sheet, unsigned bits)
                       {
                           if (sheet == WebSession::SESSION)
                               session_events |= bits;
                       });
    Client client(session);
    const int id = openSheet(client, "normal.csv", "normal.csv");
    client.result("setView", sheetParams(id, "\"sort\":[],\"filter\":{\"conditions\":[{\"column\":4,\"op\":\"equals\",\"value\":\"Osaka\"}]}"));
    client.readyInfo(id);

    SECTION("CSV without hidden columns")
    {
        client.result("startExport", sheetParams(id, "\"format\":\"csv\",\"hidden\":[1,2]"));
        const JsonValue job = waitDone(client, "jobState", "{}");
        CHECK(*job["error"].string() == "");
        CHECK(*job["fileName"].string() == "normal-export.csv");
        CHECK((session_events & WebSession::JOB) != 0);
        const std::string text = readFile(*job["file"].string());
        CHECK(text.starts_with("ID,Age,City,"));
        CHECK(std::count(text.begin(), text.end(), '\n') == 5);
        // A new job takes the old one's file away.
        client.result("startExport", sheetParams(id, "\"format\":\"excel\""));
        const JsonValue excel = waitDone(client, "jobState", "{}");
        CHECK_FALSE(std::filesystem::exists(*job["file"].string()));
        CHECK(readFile(*excel["file"].string()).starts_with("PK"));
        client.result("cancelJob");
        CHECK_FALSE(std::filesystem::exists(*excel["file"].string()));
    }

    SECTION("Group By")
    {
        client.result("setView", sheetParams(id, "\"sort\":[],\"filter\":null"));
        client.readyInfo(id);
        const JsonValue names = client.result("aggregates");
        CHECK(*(*names.array())[0].string() == "count");
        client.result("startGroupBy", sheetParams(id, "\"keys\":[4],\"values\":[3],\"aggregates\":[\"mean\"]"));
        const JsonValue job = waitDone(client, "jobState", "{}");
        CHECK(*job["error"].string() == "");
        REQUIRE(job["sheet"].string());
        const int grouped = static_cast<int>(*client.result("sheet", "{\"name\":\"" + *job["sheet"].string() + "\"}")["sheet"].number());
        const JsonValue info = client.readyInfo(grouped);
        CHECK(*info["rows"].number() == 12);
        CHECK(*(*info["columns"].array())[2]["name"].string() == "Age mean");
    }

    SECTION("only one job at a time")
    {
        client.result("startExport", sheetParams(id, "\"format\":\"parquet\""));
        const JsonValue second = client.call("startGroupBy", sheetParams(id, "\"keys\":[4]"));
        // The export may already be done, in which case the Group By starts.
        if (second["error"].string())
            CHECK(second["error"].string()->find("running") != std::string::npos);
        waitDone(client, "jobState", "{}");
    }
}

TEST_CASE("WebSession formats columns and goes to cells")
{
    WebSession session([](int, unsigned) {});
    Client client(session);
    const int id = openSheet(client, "normal.csv", "normal.csv");

    CHECK(client.result("dateStyles").array()->size() == 6);
    client.result("setFormat", sheetParams(id, "\"column\":6,\"thousands\":true,\"decimals\":0"));
    client.result("setFormat", sheetParams(id, "\"column\":7,\"date\":\"slash\""));
    const JsonValue rows = client.rows(id, 0, 0);
    CHECK(cell(rows, 0, 6) == "941,752");
    CHECK(cell(rows, 0, 7) == "1986/08/02");
    const JsonValue info = client.result("info", sheetParams(id));
    CHECK(*(*info["columns"].array())[6]["format"]["thousands"].boolean());
    CHECK(*(*info["columns"].array())[7]["format"]["date"].string() == "slash");
    CHECK(*(*info["columns"].array())[3]["kind"].string() == "number");
    // Copies keep the values.
    client.rows(id, 0, 0);
    CHECK(*client.result("copy", sheetParams(id, "\"top\":0,\"left\":6,\"bottom\":0,\"right\":6"))["text"].string() == "941751.65");

    const JsonValue city = client.result("cellFilter", sheetParams(id, "\"row\":0,\"column\":4,\"equal\":false"));
    const auto &conditions = *city["conditions"].array();
    REQUIRE(conditions.size() == 1);
    CHECK(*conditions[0]["op"].string() == "notEquals");
    CHECK(*conditions[0]["value"].string() == "Osaka");

    const JsonValue cell_b3 = client.result("goTo", sheetParams(id, "\"text\":\"B3\""));
    CHECK(cell_b3["row"].number() == 2);
    CHECK(cell_b3["column"].number() == 1);
    CHECK(client.result("goTo", sheetParams(id, "\"text\":\"City\""))["row"].isNull());
    CHECK(client.result("goTo", sheetParams(id, "\"text\":\"ZZZ999999\""))["error"].string());
}
