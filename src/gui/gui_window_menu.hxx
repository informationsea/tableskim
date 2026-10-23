#ifndef _GUI_WINDOW_MENU_HXX_
#define _GUI_WINDOW_MENU_HXX_

#include <FL/Fl_Menu_Item.H>

class Fl_Window;
class TableWindow;

// The Window menu lists every open TableWindow. All menu bars share the
// static menu array generated from gui.fl, so its "Window" item points
// (FL_SUBMENU_POINTER) to one array that is rebuilt whenever windows change.
// Having our own "Window" item also stops FLTK from adding its macOS-only one,
// which is lost when a second Fl_Sys_Menu_Bar replaces the first.

// Call once before the first TableWindow is created.
void initWindowMenu(Fl_Menu_Item *window_item);

// Rebuilds the menu on the next event loop iteration. Call it after a window
// is shown, hidden, focused or renamed.
void requestWindowMenuUpdate();

// Releases closed windows after the current widget/menu callback has returned.
void requestClosedWindowCleanup();

// The TableWindow whose main window is `window`, or nullptr. Other windows
// (Cell Detail, About, ...) keep their own objects in user_data(), so it
// cannot be cast to TableWindow.
TableWindow *tableWindowOf(const Fl_Window *window);

// The frontmost TableWindow, skipping dialogs; nullptr if there is none.
TableWindow *frontTableWindow();

#endif
