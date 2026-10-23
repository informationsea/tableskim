#include "gui_window_menu.hxx"
#include "gui.h"

#include <FL/Fl.H>
#include <FL/Fl_Sys_Menu_Bar.H>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <string>
#include <vector>

TableWindow *tableWindowOf(const Fl_Window *window)
{
    for (const auto &table_window : table_windows)
    {
        if (table_window->main_window == window)
            return table_window.get();
    }
    return nullptr;
}

TableWindow *frontTableWindow()
{
    for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w))
    {
        if (TableWindow *table_window = tableWindowOf(w))
            return table_window;
    }
    return nullptr;
}

namespace
{
    void removeClosedWindows(void *)
    {
        // A closed window's methods may still be running below an alert
        // (e.g. File > Close chosen from the macOS menu bar meanwhile).
        NestedLoopGuard::defer([]
                               {
            std::vector<std::unique_ptr<TableWindow>> closed;
            for (auto &window : table_windows)
            {
                if (!window->main_window->shown())
                    closed.push_back(std::move(window));
            }
            // Window destruction may dispatch events that look up other windows.
            std::erase(table_windows, nullptr);
            closed.clear(); });
    }

    Fl_Menu_Item *window_item_ = nullptr;
    // Moving a vector keeps its elements in place, so the label pointers in
    // items_ stay valid as long as labels_ is replaced together with them.
    std::vector<std::string> labels_;
    std::vector<Fl_Menu_Item> items_;

    bool isTableWindow(const Fl_Window *window)
    {
        return tableWindowOf(window) != nullptr;
    }

    Fl_Window *frontTableMainWindow()
    {
        TableWindow *front = frontTableWindow();
        return front ? front->main_window : nullptr;
    }

    // Menus draw '&' as an underline marker and, except in the macOS system
    // menu, '@' as the start of a symbol.
    std::string escapeMenuLabel(const char *text)
    {
        std::string escaped;
        for (const char *p = text ? text : ""; *p; ++p)
        {
            if (*p == '&')
                escaped += '&';
#ifndef __APPLE__
            if (*p == '@')
                escaped += '@';
#endif
            escaped += *p;
        }
        return escaped;
    }

    // macOS calls items of an FL_SUBMENU_POINTER menu with a null widget, so
    // these callbacks rely on user_data only.
    void minimizeCallback(Fl_Widget *, void *)
    {
        if (Fl_Window *window = frontTableMainWindow())
            window->iconize();
    }

    void bringAllToFrontCallback(Fl_Widget *, void *)
    {
        Fl_Window *front = frontTableMainWindow();
        for (const auto &table_window : table_windows)
        {
            Fl_Window *window = table_window->main_window;
            if (window->shown() && window != front)
                window->show();
        }
        // Raised last so that it stays in front.
        if (front)
            front->show();
    }

    void activateWindowCallback(Fl_Widget *, void *data)
    {
        // The window may have been closed since the menu was built.
        auto *window = static_cast<Fl_Window *>(data);
        if (isTableWindow(window) && window->shown())
            window->show();
    }

    Fl_Menu_Item makeItem(const char *label, int shortcut, Fl_Callback *callback, void *data, int flags)
    {
        Fl_Menu_Item item{};
        item.label(label);
        item.shortcut(shortcut);
        item.callback(callback, data);
        item.flags = flags;
        return item;
    }

    void rebuildWindowMenu(void *)
    {
        if (!window_item_)
            return;
        // An open menu (which holds the grab) may be showing items_; replacing
        // it now would leave the menu drawing freed items. Some X11 window
        // managers send FL_FOCUS when a menu pops up, which lands here.
        if (Fl::grab())
        {
            Fl::remove_timeout(rebuildWindowMenu);
            Fl::add_timeout(0.1, rebuildWindowMenu);
            return;
        }

        std::vector<Fl_Window *> windows;
        for (const auto &table_window : table_windows)
        {
            if (table_window->main_window->shown())
                windows.push_back(table_window->main_window);
        }
        Fl_Window *front = frontTableMainWindow();

        std::vector<std::string> labels;
        labels.reserve(windows.size());
        for (Fl_Window *window : windows)
            labels.push_back(escapeMenuLabel(window->label()));

        const int inactive = windows.empty() ? FL_MENU_INACTIVE : 0;
        std::vector<Fl_Menu_Item> items;
        items.push_back(makeItem("Minimize", FL_COMMAND + 'm', minimizeCallback, nullptr, inactive));
        items.push_back(makeItem("Bring All to Front", 0, bringAllToFrontCallback, nullptr,
                                 inactive | (windows.empty() ? 0 : FL_MENU_DIVIDER)));
        for (size_t i = 0; i < windows.size(); ++i)
        {
            const int checked = windows[i] == front ? FL_MENU_VALUE : 0;
            items.push_back(makeItem(labels[i].c_str(), 0, activateWindowCallback, windows[i],
                                     FL_MENU_RADIO | checked));
        }
        items.push_back(Fl_Menu_Item{}); // terminator

        labels_ = std::move(labels);
        items_ = std::move(items);
        window_item_->user_data(items_.data());

        // The macOS system menu is a copy that must be regenerated; other
        // platforms read the items each time a menu pops up.
        if (fl_sys_menu_bar)
            fl_sys_menu_bar->update();
    }
} // namespace

void initWindowMenu(Fl_Menu_Item *window_item)
{
    window_item_ = window_item;
    window_item_->flags |= FL_SUBMENU_POINTER;
    rebuildWindowMenu(nullptr);
}

void requestWindowMenuUpdate()
{
    // Deferred, because the request may come from a callback of an item in
    // the array that is about to be replaced; this also merges bursts.
    Fl::remove_timeout(rebuildWindowMenu);
    Fl::add_timeout(0.0, rebuildWindowMenu);
}

void requestClosedWindowCleanup()
{
    // Deleting a window in its own callback would invalidate FLTK's dispatch state.
    Fl::remove_timeout(removeClosedWindows);
    Fl::add_timeout(0.0, removeClosedWindows);
}
