#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

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
