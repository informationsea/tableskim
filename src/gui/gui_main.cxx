#include <algorithm>
#include <cstdio>
#include <string>

#include "gui_main.hxx"
#include "gui.h"
#include "gui_fonts.hxx"
#include "gui_theme.hxx"
#include "gui_window_menu.hxx"
#include "tableskim_version.hxx"
#include <vector>
#include <memory>
#include <spdlog/spdlog.h>
#include <FL/platform.H>
#include <FL/Fl_PNG_Image.H>

#ifdef __APPLE__
namespace
{
    void openDocumentInWindow(const std::string &path)
    {
        // Cocoa may also report files given on the command line, which are
        // already open.
        for (const auto &table_window : table_windows)
        {
            if (table_window->opened_path() == path)
            {
                table_window->show();
                return;
            }
        }

        TableWindow *target = nullptr;
        for (const auto &table_window : table_windows)
        {
            if (table_window->opened_path().empty())
            {
                target = table_window.get();
                break;
            }
        }
        if (!target)
        {
            table_windows.push_back(std::unique_ptr<TableWindow>(new TableWindow()));
            target = table_windows.back().get();
            target->show();
        }
        target->open(path);
    }

    // Finder sends files opened with the app as "Open" Apple Events instead
    // of command-line arguments. They may arrive during an alert, below which
    // the views that opening replaces can still be in use.
    void openDocument(const char *path)
    {
        NestedLoopGuard::defer([path = std::string(path)]
                               { openDocumentInWindow(path); });
    }
} // namespace
#endif

int my_dispatch(int e, Fl_Window *w);

int runGui(const CommandLineOptions &options)
{
    std::string error;
    if (!configureLogging(options, true, &error))
    {
        std::fprintf(stderr, "Cannot open log file %s: %s\n", options.log_file.c_str(), error.c_str());
        return 1;
    }

    spdlog::debug("Open files: {}", options.files.empty() ? "None" : options.files[0]);

    Fl::lock(); // enable Fl::awake() from TableBook worker threads
#ifdef __APPLE__
    // Installed before the event loop starts so the file that launched the app is not missed.
    fl_open_callback(openDocument);
#endif
    fl_open_display();
    register_fonts();
    Fl_PNG_Image image("icon", TableWindow::icon, sizeof(TableWindow::icon));

#ifndef __APPLE__
    Fl_Window::default_icon(&image);
#endif
    // Window managers group windows by WM_CLASS (Wayland: app_id), which defaults to "FLTK".
    Fl_Window::default_xclass("TableSkim");
    setTheme(FLUENT_THEME);

    Fl::event_dispatch(my_dispatch);

    version_window = make_version_window();
    initWindowMenu(TableWindow::menuitem_window);
    // One window per file, and an empty one if no file is given.
    const size_t window_count = std::max<size_t>(options.files.size(), 1);
    for (size_t i = 0; i < window_count; ++i)
    {
        // Registered before show(): on macOS it sends FL_FOCUS right away.
        table_windows.push_back(std::unique_ptr<TableWindow>(new TableWindow()));
        table_windows.back()->show();
        if (i < options.files.size())
        {
            // Before any file is open this only records the choice.
            if (options.encoding)
                table_windows.back()->set_text_encoding(*options.encoding);
            if (options.header)
                table_windows.back()->set_first_row(*options.header ? OpenOptions::Header::Yes : OpenOptions::Header::No);
            table_windows.back()->open(options.files[i]);
        }
        else
        {
            // Greys out the commands that need a file, even if the window
            // manager sends no FL_FOCUS.
            table_windows.back()->sync_menu_items();
            requestWindowMenuUpdate();
        }
    }
    return Fl::run();
}

void setFltkScheme(Fl_Menu_ *, void *data)
{
    setTheme(static_cast<const char *>(data));
}

void quit(Fl_Menu_ *, void *)
{
    // Saving reopens files, which must wait until no menu or alert is up.
    NestedLoopGuard::defer([]
                           {
        spdlog::debug("Quitting...");
        // Each window with unsaved changes asks first; Cancel stops quitting.
        for (const auto &table_window : table_windows)
        {
            if (!table_window->confirm_unsaved())
                return;
        }
        for (const auto &table_window : table_windows)
        {
            table_window->hide();
        }
        // Fl::run() returns only once no window is shown, including the About
        // window.
        while (Fl_Window *window = Fl::first_window())
            window->hide(); });
}

int my_dispatch(int e, Fl_Window *w)
{
    if (w && e == FL_FOCUS)
    {
        // spdlog::debug("Focus event for window: {}", w->label());
        TableWindow *table_window = tableWindowOf(w);
        if (table_window)
        {
            table_window->sync_menu_items();
            requestWindowMenuUpdate();
            table_window->set_focused(true);
        }
        // table_window
    }
    else if (w && e == FL_UNFOCUS)
    {
        // spdlog::debug("Unfocus event for window: {}", w->label());
        if (auto *table_window = tableWindowOf(w))
            table_window->set_focused(false);
    }

    return Fl::handle_(e, w);
}