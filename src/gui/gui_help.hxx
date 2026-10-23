#ifndef _GUI_HELP_HXX_
#define _GUI_HELP_HXX_

// Shows the user manual (docs/, built into the program) in a window of its
// own (Help > User Manual): in Japanese if that is the system's language,
// else in English. The pages are written to a temporary directory the
// first time, for Fl_Help_View to load, and removed again at exit.
void showHelpWindow();

#endif // _GUI_HELP_HXX_
