#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "cli_options.hxx"

namespace
{
    std::optional<int> parse(std::vector<std::string> args, CommandLineOptions &options)
    {
        args.insert(args.begin(), "tableskim");
        return parseCommandLine(args, options);
    }

    EnvLookup fakeEnv(std::map<std::string, std::string> env)
    {
        return [env = std::move(env)](const char *name) -> const char *
        {
            const auto it = env.find(name);
            return it == env.end() ? nullptr : it->second.c_str();
        };
    }
} // namespace

TEST_CASE("configureLogging sends console logs to stderr", "[cli][regression]")
{
    struct RestoreLogger
    {
        std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
        ~RestoreLogger()
        {
            spdlog::drop("tableskim");
            spdlog::set_default_logger(previous);
        }
    } restore;
    CommandLineOptions options;
    options.summary = true;
    options.verbose = true;
    std::string error;
    REQUIRE(configureLogging(options, options.verbose, &error));
    const auto logger = spdlog::default_logger();
    REQUIRE(logger->sinks().size() == 1);
    CHECK(std::dynamic_pointer_cast<spdlog::sinks::stderr_color_sink_mt>(logger->sinks().front()));
}

TEST_CASE("parseCommandLine reads the shared options", "[cli]")
{
    CommandLineOptions options;

    SECTION("defaults")
    {
        REQUIRE_FALSE(parse({}, options));
        CHECK_FALSE(options.verbose);
        CHECK_FALSE(options.tui);
        CHECK(options.log_file.empty());
        CHECK(options.files.empty());
    }

    SECTION("all options")
    {
        REQUIRE_FALSE(parse({"-v", "--tui", "--log", "out.log", "a.csv", "b.xlsx"}, options));
        CHECK(options.verbose);
        CHECK(options.tui);
        CHECK(options.log_file == "out.log");
        CHECK(options.files == std::vector<std::string>{"a.csv", "b.xlsx"});
    }

    SECTION("short TUI flag")
    {
        REQUIRE_FALSE(parse({"-t", "a.csv"}, options));
        CHECK(options.tui);
        CHECK(options.files == std::vector<std::string>{"a.csv"});
    }

    SECTION("encoding")
    {
        REQUIRE_FALSE(parse({"a.csv"}, options));
        CHECK_FALSE(options.encoding);
        REQUIRE_FALSE(parse({"--encoding", "sjis", "a.csv"}, options));
        CHECK(options.encoding == TextEncoding::Cp932);
        REQUIRE_FALSE(parse({"--encoding", "auto", "a.csv"}, options));
        CHECK_FALSE(options.encoding);
        const auto exit_code = parse({"--encoding", "latin1", "a.csv"}, options);
        REQUIRE(exit_code);
        CHECK(*exit_code != 0);
    }

    SECTION("unknown option is an error")
    {
        const auto exit_code = parse({"--no-such-option"}, options);
        REQUIRE(exit_code);
        CHECK(*exit_code != 0);
    }
}

TEST_CASE("parseCommandLine reads the summary options", "[cli]")
{
    CommandLineOptions options;

    SECTION("defaults")
    {
        REQUIRE_FALSE(parse({"a.csv"}, options));
        CHECK_FALSE(options.printsSummary());
        CHECK_FALSE(options.head);
        CHECK(options.summary_format == SummaryFormat::Markdown);
        CHECK(options.sheets.empty());
        CHECK_FALSE(options.scan_rows);
        CHECK_FALSE(options.header);
    }

    SECTION("--summary")
    {
        REQUIRE_FALSE(parse({"--summary", "--format", "json", "--scan-rows", "1000", "--header", "no", "a.csv"},
                            options));
        CHECK(options.summary);
        CHECK(options.printsSummary());
        CHECK(options.summary_format == SummaryFormat::Json);
        CHECK(options.scan_rows == 1000u);
        CHECK(options.header == false);
        CHECK(options.files == std::vector<std::string>{"a.csv"});
    }

    SECTION("--head with sheets")
    {
        REQUIRE_FALSE(parse({"--head", "20", "--sheet", "A", "--sheet", "B", "a.xlsx", "b.xlsx"}, options));
        CHECK_FALSE(options.summary);
        CHECK(options.printsSummary());
        CHECK(options.head == 20u);
        CHECK(options.sheets == std::vector<std::string>{"A", "B"});
        CHECK(options.files == std::vector<std::string>{"a.xlsx", "b.xlsx"});
    }

    SECTION("errors")
    {
        for (const std::vector<std::string> &args : {
                 std::vector<std::string>{"--summary"},                        // no file
                 std::vector<std::string>{"--format", "json", "a.csv"},        // no --summary
                 std::vector<std::string>{"--sheet", "A", "a.csv"},            // no --summary
                 std::vector<std::string>{"--summary", "--format", "xml", "a.csv"},
                 std::vector<std::string>{"--summary", "--header", "maybe", "a.csv"},
                 std::vector<std::string>{"--head", "many", "a.csv"},
             })
        {
            CommandLineOptions fresh;
            const auto exit_code = parse(args, fresh);
            REQUIRE(exit_code);
            CHECK(*exit_code != 0);
        }
    }
}

TEST_CASE("chooseFrontend prefers the GUI unless forced or impossible", "[cli]")
{
    CommandLineOptions options;
    CHECK(chooseFrontend(options, true) == Frontend::Gui);
    CHECK(chooseFrontend(options, false) == Frontend::Tui);
    options.tui = true;
    CHECK(chooseFrontend(options, true) == Frontend::Tui);
    CHECK(chooseFrontend(options, false) == Frontend::Tui);
}

TEST_CASE("graphicalDisplayAvailable looks at the environment", "[cli]")
{
#if defined(_WIN32)
    CHECK(graphicalDisplayAvailable(fakeEnv({})));
    CHECK(graphicalDisplayAvailable(fakeEnv({{"SSH_CONNECTION", "1.2.3.4 5 6.7.8.9 22"}})));
#elif defined(__APPLE__)
    CHECK(graphicalDisplayAvailable(fakeEnv({})));
    CHECK_FALSE(graphicalDisplayAvailable(fakeEnv({{"SSH_CONNECTION", "1.2.3.4 5 6.7.8.9 22"}})));
    CHECK_FALSE(graphicalDisplayAvailable(fakeEnv({{"SSH_TTY", "/dev/ttys001"}})));
    CHECK(graphicalDisplayAvailable(fakeEnv({{"SSH_TTY", ""}})));
#else
    CHECK_FALSE(graphicalDisplayAvailable(fakeEnv({})));
    CHECK_FALSE(graphicalDisplayAvailable(fakeEnv({{"DISPLAY", ""}})));
    CHECK(graphicalDisplayAvailable(fakeEnv({{"DISPLAY", ":0"}})));
    CHECK(graphicalDisplayAvailable(fakeEnv({{"WAYLAND_DISPLAY", "wayland-0"}})));
    CHECK(graphicalDisplayAvailable(fakeEnv({{"WAYLAND_DISPLAY", "wayland-0"}, {"SSH_CONNECTION", "x"}})));
#endif
}
