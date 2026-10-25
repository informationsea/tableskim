// main() of tableskim-gui-tests: sets FLTK up as runGui() does, then runs
// the Catch2 tests on the main thread. Needs an X display (ctest runs it
// under xvfb-run).

#include <catch2/catch_session.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

#include <FL/Fl.H>

#include "gui.h"
#include "gui_fonts.hxx"
#include "gui_test_helpers.hxx"
#include "gui_theme.hxx"
#include "gui_window_menu.hxx"
// Last: X11 defines macros (None, ...) that clash with names above.
#include <FL/platform.H>

int my_dispatch(int e, Fl_Window *w);

namespace
{
    // A dialog that no test answers would stop the tests for good: it is
    // closed after a while, which the test then fails on (or times out).
    struct UnexpectedModal
    {
        Fl_Window *window = nullptr;
        std::chrono::steady_clock::time_point since;
    } unexpected_modal;

    void closeUnexpectedModal(void *)
    {
        Fl_Window *modal = Fl::modal();
        if (!modal || !modal->shown())
        {
            unexpected_modal.window = nullptr;
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (modal != unexpected_modal.window)
        {
            unexpected_modal = {modal, now};
            return;
        }
        if (now - unexpected_modal.since < std::chrono::seconds(10))
            return;
        std::fprintf(stderr, "Closing a modal window no test answered: %s\n",
                     gui_test::ModalAnswer::message(*modal).c_str());
        modal->hide();
        unexpected_modal.window = nullptr;
    }
} // namespace

int main(int argc, char *argv[])
{
    // The recent files and the SQL history go to a directory of their own.
    std::random_device random;
    const auto state = std::filesystem::temp_directory_path() / ("tableskim-gui-tests-" + std::to_string(random()));
    std::filesystem::create_directories(state);
    setenv("XDG_STATE_HOME", state.c_str(), 1);

    if (!std::getenv("DISPLAY"))
    {
        std::fprintf(stderr, "tableskim-gui-tests needs an X display; run it with xvfb-run -a\n");
        return 1;
    }
    // X11 even where FLTK could use Wayland.
    unsetenv("WAYLAND_DISPLAY");

    Fl::lock(); // Fl::awake() from TableBook worker threads
    fl_open_display();
    register_fonts();
    Fl_Window::default_xclass("TableSkim");
    setTheme(FLUENT_THEME);
    Fl::event_dispatch(my_dispatch);
    version_window = make_version_window();
    initWindowMenu(TableWindow::menuitem_window);
    Fl::add_check(closeUnexpectedModal);

    const int result = Catch::Session().run(argc, argv);

    table_windows.clear();
    std::error_code ec;
    std::filesystem::remove_all(state, ec);
    return result;
}
