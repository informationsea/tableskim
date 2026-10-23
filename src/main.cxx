#include "cli_options.hxx"
#include "tui_main.hxx"
#ifdef TABLESKIM_HAS_GUI
#include "gui_main.hxx"
#endif

// The single `tableskim` command: the GUI when a display is available,
// otherwise (or with --tui) the terminal UI. Built with
// -DTABLESKIM_BUILD_GUI=OFF, it is always the terminal UI.
int main(int argc, char *argv[])
{
    CommandLineOptions options;
    if (const auto exit_code = parseCommandLine(argc, argv, options))
        return *exit_code;

#ifdef TABLESKIM_HAS_GUI
    if (chooseFrontend(options, graphicalDisplayAvailable()) == Frontend::Gui)
        return runGui(options);
#endif
    return runTui(options);
}
