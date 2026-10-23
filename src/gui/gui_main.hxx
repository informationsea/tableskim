#ifndef _GUI_MAIN_HXX_
#define _GUI_MAIN_HXX_

#include "cli_options.hxx"

// Runs the FLTK GUI until its last window is closed; returns the exit code.
int runGui(const CommandLineOptions &options);

#endif // _GUI_MAIN_HXX_
