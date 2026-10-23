#ifndef _TUI_MAIN_HXX_
#define _TUI_MAIN_HXX_

#include "cli_options.hxx"

// Runs the terminal UI until the user quits; returns the exit code.
int runTui(const CommandLineOptions &options);

#endif // _TUI_MAIN_HXX_
