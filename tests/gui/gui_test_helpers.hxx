#ifndef _GUI_TEST_HELPERS_HXX_
#define _GUI_TEST_HELPERS_HXX_

// Helpers for tests that drive the GUI (tableskim-gui-tests, Linux under
// Xvfb). Everything runs on the main thread, which turns the FLTK loop
// while it waits, so that Fl::awake() callbacks, timeouts and redraws run.

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <FL/Fl.H>
#include <FL/Fl_Window.H>

#include "gui.h"
#include "gui_table_view.hxx"

namespace gui_test
{
    using namespace std::chrono_literals;

    // Turns the event loop for about `duration`.
    void pump(std::chrono::milliseconds duration = 50ms);

    // Turns the event loop until `condition` holds; false after `timeout`.
    bool waitFor(const std::function<bool()> &condition, std::chrono::milliseconds timeout = 15s);

    // Answers the next modal window (a dialog or an alert) once it is shown,
    // from inside its own loop: `answer` gets the window and returns true
    // once it has dealt with it (else it is asked again on the next turn).
    // Destroying the object stops answering.
    class ModalAnswer
    {
    public:
        explicit ModalAnswer(std::function<bool(Fl_Window &)> answer);
        ~ModalAnswer();
        ModalAnswer(const ModalAnswer &) = delete;
        ModalAnswer &operator=(const ModalAnswer &) = delete;
        bool answered() const { return answered_; }
        // The label of the window's message, if it is an alert (fl_alert(),
        // fl_choice(), ...): the text of its first box with a label.
        static std::string message(Fl_Window &window);

    private:
        static void check(void *data);
        std::function<bool(Fl_Window &)> answer_;
        bool answered_ = false;
    };

    // Presses the button labelled `label` in `window` (searched
    // recursively). False if there is none.
    bool pressButton(Fl_Window &window, const std::string &label);

    // A TableWindow, shown and registered as main() does, closed and
    // deleted (without asking about unsaved changes) when destroyed.
    class Window
    {
    public:
        Window();
        explicit Window(const std::filesystem::path &path);
        // With View > First Row chosen before the file is opened.
        Window(const std::filesystem::path &path, OpenOptions::Header first_row);
        ~Window();
        Window(const Window &) = delete;
        Window &operator=(const Window &) = delete;

        TableWindow &operator*() const { return *window_; }
        TableWindow *operator->() const { return window_; }

        // The tabs' views, in order, and their labels.
        std::vector<GuiTableView *> views() const;
        std::vector<std::string> sheetLabels() const;
        GuiTableView *currentView() const;
        // Waits until a view's model has read its rows (an exact count).
        bool waitForRows(GuiTableView *view, size_t rows) const;

    private:
        TableWindow *window_ = nullptr;
    };

    // A directory removed with its files at the end of a test.
    class TempDir
    {
    public:
        TempDir();
        ~TempDir();
        const std::filesystem::path &path() const { return path_; }
        std::filesystem::path write(const std::string &name, const std::string &text) const;

    private:
        std::filesystem::path path_;
    };

    std::string readFile(const std::filesystem::path &path);
} // namespace gui_test

#endif // _GUI_TEST_HELPERS_HXX_
