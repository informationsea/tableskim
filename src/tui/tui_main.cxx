#include "tui_main.hxx"

#include <cstdio>
#include <string>

#include <ftxui/component/app.hpp>

#include "tui_app.hxx"

#ifdef _WIN32
#include <windows.h>

namespace
{
    // TableSkim.exe is a GUI application (no console) in Release builds;
    // tableskim-tui.exe is the console program meant for the TUI. With
    // --tui, TableSkim.exe would share cmd.exe's console without the shell
    // waiting for it, so give the TUI a console of its own instead.
    void ensureConsole()
    {
        if (GetConsoleWindow() || !AllocConsole())
            return;
        FILE *stream = nullptr;
        freopen_s(&stream, "CONIN$", "r", stdin);
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
} // namespace
#endif

int runTui(const CommandLineOptions &options)
{
    // The TUI has no way to choose a file, so it needs exactly one.
    if (options.files.size() != 1)
    {
        std::fprintf(stderr, options.files.empty() ? "The terminal UI needs a file to open\n"
                                                   : "The terminal UI opens one file at a time\n");
        std::fprintf(stderr, "Usage: tableskim [OPTIONS] file\n");
        return 1;
    }
#ifdef _WIN32
    ensureConsole();
#endif

    // Log output would be drawn over the full-screen UI.
    std::string error;
    if (!configureLogging(options, false, &error))
    {
        std::fprintf(stderr, "Cannot open log file %s: %s\n", options.log_file.c_str(), error.c_str());
        return 1;
    }

    auto screen = ftxui::App::Fullscreen();
    // Destroyed before the screen: its views post to the screen's loop.
    TuiApp tui(screen);
    const std::string &file = options.files.front();
    if (options.encoding)
        tui.setTextEncoding(*options.encoding);
    if (options.header)
        tui.setFirstRow(*options.header ? OpenOptions::Header::Yes : OpenOptions::Header::No);
    if (!tui.open(file, &error))
    {
        std::fprintf(stderr, "Failed to open %s: %s\n", file.c_str(), error.c_str());
        return 1;
    }
#ifndef _WIN32
    // Bracketed paste: the terminal marks pasted text, so that its newlines
    // are not taken for Enter (see TuiApp::handlePasteEvent()).
    std::fputs("\x1b[?2004h", stdout);
    std::fflush(stdout);
#endif
    screen.Loop(tui.component());
#ifndef _WIN32
    std::fputs("\x1b[?2004l", stdout);
    std::fflush(stdout);
#endif
    return 0;
}
