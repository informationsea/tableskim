#include "gui_test_helpers.hxx"

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>

#include "file_util.hxx"

namespace gui_test
{
    void pump(std::chrono::milliseconds duration)
    {
        const auto end = std::chrono::steady_clock::now() + duration;
        while (true)
        {
            const auto left = std::chrono::duration<double>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0)
                return;
            Fl::wait(std::min(left, 0.01));
        }
    }

    bool waitFor(const std::function<bool()> &condition, std::chrono::milliseconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() > end)
                return false;
            Fl::wait(0.01);
        }
        return true;
    }

    namespace
    {
        template <typename Visit>
        bool visitWidgets(Fl_Widget &widget, const Visit &visit)
        {
            if (visit(widget))
                return true;
            if (Fl_Group *group = widget.as_group())
            {
                for (int i = 0; i < group->children(); ++i)
                {
                    if (visitWidgets(*group->child(i), visit))
                        return true;
                }
            }
            return false;
        }
    } // namespace

    ModalAnswer::ModalAnswer(std::function<bool(Fl_Window &)> answer) : answer_(std::move(answer))
    {
        Fl::add_check(check, this);
    }

    ModalAnswer::~ModalAnswer()
    {
        Fl::remove_check(check, this);
    }

    void ModalAnswer::check(void *data)
    {
        auto *self = static_cast<ModalAnswer *>(data);
        Fl_Window *modal = Fl::modal();
        if (self->answered_ || !modal || !modal->shown())
            return;
        self->answered_ = self->answer_(*modal);
    }

    std::string ModalAnswer::message(Fl_Window &window)
    {
        // Alerts hold an icon box ("!", "?") and the message box.
        std::string longest;
        visitWidgets(window, [&](Fl_Widget &widget)
                     {
            if (dynamic_cast<Fl_Box *>(&widget) && widget.label() && std::string(widget.label()).size() > longest.size())
                longest = widget.label();
            return false; });
        return longest;
    }

    bool pressButton(Fl_Window &window, const std::string &label)
    {
        return visitWidgets(window, [&](Fl_Widget &widget)
                            {
            auto *button = dynamic_cast<Fl_Button *>(&widget);
            if (!button || !button->label())
                return false;
            std::string text = button->label();
            std::erase(text, '&'); // shortcut markers
            if (text != label)
                return false;
            button->do_callback();
            return true; });
    }

    Window::Window()
    {
        // As main() does: registered before show().
        table_windows.push_back(std::unique_ptr<TableWindow>(new TableWindow()));
        window_ = table_windows.back().get();
        window_->show();
        window_->sync_menu_items();
        pump();
    }

    Window::Window(const std::filesystem::path &path) : Window()
    {
        window_->open(utf8String(path));
        pump();
    }

    Window::Window(const std::filesystem::path &path, OpenOptions::Header first_row) : Window()
    {
        window_->set_first_row(first_row);
        window_->open(utf8String(path));
        pump();
    }

    Window::~Window()
    {
        window_->hide();
        // hide() has the window deleted once no nested loop is running.
        const TableWindow *window = window_;
        const auto registered = [window]
        {
            return std::any_of(table_windows.begin(), table_windows.end(),
                               [window](const auto &w) { return w.get() == window; });
        };
        waitFor([&] { return !registered(); }, 5s);
        std::erase_if(table_windows, [window](const auto &w) { return w.get() == window; });
        pump();
    }

    std::vector<GuiTableView *> Window::views() const
    {
        std::vector<GuiTableView *> views;
        for (int i = 0; i < window_->tab->children(); ++i)
        {
            if (auto *view = dynamic_cast<GuiTableView *>(window_->tab->child(i)))
                views.push_back(view);
        }
        return views;
    }

    std::vector<std::string> Window::sheetLabels() const
    {
        std::vector<std::string> labels;
        for (const GuiTableView *view : views())
            labels.emplace_back(view->label() ? view->label() : "");
        return labels;
    }

    GuiTableView *Window::currentView() const
    {
        return dynamic_cast<GuiTableView *>(window_->tab->value());
    }

    bool Window::waitForRows(GuiTableView *view, size_t rows) const
    {
        return waitFor([&]
                       {
            const auto model = view ? view->model() : nullptr;
            return model && model->rowCount().isExact() && model->rowCount().value == rows; });
    }

    TempDir::TempDir()
    {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() / ("tableskim-gui-test-" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }

    TempDir::~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::filesystem::path TempDir::write(const std::string &name, const std::string &text) const
    {
        const auto path = path_ / name;
        std::ofstream out(path, std::ios::binary);
        out << text;
        return path;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }
} // namespace gui_test
