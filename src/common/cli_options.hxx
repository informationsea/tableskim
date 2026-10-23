#ifndef _CLI_OPTIONS_HXX_
#define _CLI_OPTIONS_HXX_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "text_encoding.hxx"

// Command-line options shared by the GUI and the TUI. The single `tableskim`
// executable parses them once and then starts one of the two frontends.
struct CommandLineOptions
{
    bool verbose = false;
    // Start the terminal UI even when a graphical display is available.
    bool tui = false;
    std::string log_file;           // UTF-8; empty: GUI logs to stderr, TUI nowhere
    // Encoding of the text files given here; unset: detected.
    std::optional<TextEncoding> encoding;
    std::vector<std::string> files; // UTF-8
};

// Parses UTF-8 arguments, args[0] being the program name. Returns the exit
// code when the program should stop right away (--help, --version or a usage
// error, which has already been printed).
std::optional<int> parseCommandLine(const std::vector<std::string> &args, CommandLineOptions &options);
// Parses main()'s arguments. On Windows argv is in the ANSI code page, so the
// process command line is read again as UTF-8 instead.
std::optional<int> parseCommandLine(int argc, char *argv[], CommandLineOptions &options);

enum class Frontend
{
    Gui,
    Tui,
};

// Returns the value of an environment variable, or nullptr if it is not set.
using EnvLookup = std::function<const char *(const char *)>;

// Whether a GUI can be shown: an X11 or Wayland display on Linux and other
// Unix systems, anything but an SSH session on macOS, always on Windows.
bool graphicalDisplayAvailable(const EnvLookup &getenv);
bool graphicalDisplayAvailable();

Frontend chooseFrontend(const CommandLineOptions &options, bool display_available);

// Sets up spdlog: a file when --log is given, else stderr (to_console) or
// nothing (the TUI, whose screen log output would corrupt). Returns false and
// fills *error if the log file cannot be opened.
bool configureLogging(const CommandLineOptions &options, bool to_console, std::string *error);

#endif // _CLI_OPTIONS_HXX_
