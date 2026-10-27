#include <catch2/catch_test_macros.hpp>

#include <atomic>
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
