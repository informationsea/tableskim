#include "cli_options.hxx"

#include <cstdlib>

#include <CLI/CLI.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
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
    std::string header = "auto";
    app.add_option("--header", header,
                   "Whether the first row of CSV / TSV files and Excel sheets holds the column names: auto "
                   "(default), yes or no")
        ->check(CLI::Validator(
            [](std::string &value) -> std::string
            {
                if (value == "auto" || value == "yes" || value == "no")
                    return {};
                return "Expected auto, yes or no: " + value;
            },
            "WHEN"));
    app.add_option("files", options.files, "Files to open (the terminal UI needs exactly one)")->expected(0, -1);

    auto *summary_group = app.add_option_group("Summary", "Print to standard output and exit, without a UI");
    summary_group->add_flag("--summary", options.summary,
                            "Print each sheet's columns, their types and statistics, and the first rows");
    summary_group->add_option("--head", options.head, "Print the columns and the first N rows (with --summary: "
                                                      "N rows instead of 10)")
        ->type_name("N");
    std::string format = "markdown";
    auto *format_option = summary_group->add_option("--format", format, "Output of --summary / --head: markdown or json")
                              ->check(CLI::Validator(
                                  [](std::string &value) -> std::string
                                  {
                                      if (value == "markdown" || value == "json")
                                          return {};
                                      return "Unknown format: " + value;
                                  },
                                  "FORMAT"));
    auto *sheet_option = summary_group->add_option("--sheet", options.sheets, "Only this sheet (may be repeated)")
                             ->type_name("NAME")
                             ->allow_extra_args(false);
    auto *scan_option = summary_group->add_option("--scan-rows", options.scan_rows,
                                                  "Compute the statistics over the first N rows only")
                            ->type_name("N");
    std::vector<std::string> arg_storage = args;
    std::vector<char *> argv;
    for (auto &arg : arg_storage)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    try
    {
        app.parse(static_cast<int>(arg_storage.size()), argv.data());
        if (!options.printsSummary())
        {
            for (const auto *option : {format_option, sheet_option, scan_option})
            {
                if (option->count() > 0)
                    throw CLI::ValidationError(option->get_name(), "needs --summary or --head");
            }
        }
        else if (options.files.empty())
        {
            throw CLI::ValidationError("files", "--summary and --head need a file");
        }
    }
    catch (const CLI::ParseError &e)
    {
        return app.exit(e);
    }
    options.encoding = encoding == "auto" ? std::nullopt : parseTextEncoding(encoding);
    options.summary_format = format == "json" ? SummaryFormat::Json : SummaryFormat::Markdown;
    options.header = header == "auto" ? std::nullopt : std::optional<bool>(header == "yes");
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
        else
        {
            spdlog::set_default_logger(spdlog::stderr_color_mt("tableskim"));
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
