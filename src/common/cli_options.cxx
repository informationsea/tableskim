#include "cli_options.hxx"

#include <cstdlib>

#include <CLI/CLI.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include "file_util.hxx"
#include "tableskim_version.hxx"

namespace
{
    bool isSet(const EnvLookup &getenv, const char *name)
    {
        const char *value = getenv(name);
        return value && *value;
    }
} // namespace

std::optional<int> parseCommandLine(const std::vector<std::string> &args, CommandLineOptions &options)
{
    CLI::App app{"TableSkim - A simple table viewer"};
    app.set_version_flag("-V,--version", "TableSkim " TABLESKIM_VERSION_FULL, "Show version information and exit");
    app.add_flag("-v,--verbose", options.verbose, "Enable verbose mode");
    app.add_flag("-t,--tui", options.tui, "Use the terminal UI even if a graphical display is available");
    app.add_option("--log", options.log_file, "Write log messages to this file");
    std::string encoding = "auto";
    app.add_option("--encoding", encoding,
                   "Encoding of CSV / TSV files: auto, utf-8, utf-16le, utf-16be or cp932 (shift_jis)")
        ->check(CLI::Validator(
            [](std::string &value) -> std::string
            {
                if (value == "auto" || parseTextEncoding(value))
                    return {};
                return "Unknown encoding: " + value;
            },
            "ENCODING"));
    app.add_option("files", options.files, "Files to open (the terminal UI needs exactly one)")->expected(0, -1);
    std::vector<std::string> arg_storage = args;
    std::vector<char *> argv;
    for (auto &arg : arg_storage)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    try
    {
        app.parse(static_cast<int>(arg_storage.size()), argv.data());
    }
    catch (const CLI::ParseError &e)
    {
        return app.exit(e);
    }
    options.encoding = encoding == "auto" ? std::nullopt : parseTextEncoding(encoding);
    return std::nullopt;
}

std::optional<int> parseCommandLine(int argc, char *argv[], CommandLineOptions &options)
{
#ifdef _WIN32
    // App::ensure_utf8() would do the same, but ignores argc, which then no
    // longer matches the array it returns.
    (void)argc;
    (void)argv;
    return parseCommandLine(CLI::detail::compute_win32_argv(), options);
#else
    return parseCommandLine(std::vector<std::string>(argv, argv + argc), options);
#endif
}

bool graphicalDisplayAvailable(const EnvLookup &getenv)
{
#if defined(_WIN32)
    (void)getenv;
    return true;
#elif defined(__APPLE__)
    // A local session always has the window server; one over SSH cannot use it.
    return !isSet(getenv, "SSH_CONNECTION") && !isSet(getenv, "SSH_CLIENT") && !isSet(getenv, "SSH_TTY");
#else
    return isSet(getenv, "WAYLAND_DISPLAY") || isSet(getenv, "DISPLAY");
#endif
}

bool graphicalDisplayAvailable()
{
    return graphicalDisplayAvailable([](const char *name)
                                     { return std::getenv(name); });
}

Frontend chooseFrontend(const CommandLineOptions &options, bool display_available)
{
    return options.tui || !display_available ? Frontend::Tui : Frontend::Gui;
}

bool configureLogging(const CommandLineOptions &options, bool to_console, std::string *error)
{
    try
    {
        if (!options.log_file.empty())
        {
#ifdef _WIN32
            // spdlog opens narrow names in the ANSI code page.
            const auto log_path = utf8Path(options.log_file).wstring();
#else
            const auto &log_path = options.log_file;
#endif
            spdlog::set_default_logger(spdlog::basic_logger_mt("tableskim", log_path));
            // The log is read while the program is running (or after it was killed).
            spdlog::flush_on(spdlog::level::trace);
        }
        else if (!to_console)
        {
            spdlog::set_default_logger(spdlog::null_logger_mt("tableskim"));
        }
    }
    catch (const spdlog::spdlog_ex &e)
    {
        if (error)
            *error = e.what();
        return false;
    }
#ifdef TABLESKIM_DEBUG
    spdlog::set_level(spdlog::level::debug);
#else
    if (options.verbose)
    {
        spdlog::set_level(spdlog::level::debug);
        spdlog::info("Verbose mode enabled");
    }
#endif
    return true;
}
