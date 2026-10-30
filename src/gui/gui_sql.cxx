#include "gui_sql.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <cstdlib>

#include "gui_table_view.hxx"
#include "gui_theme.hxx"
#include "sql_text.hxx"

namespace
{
    const int WINDOW_WIDTH = 560;
    const int WINDOW_HEIGHT = 280;
    const int MARGIN = 8;
    const int BUTTON_WIDTH = 80;
    const int BUTTON_HEIGHT = 25;
    const int FONT_SIZE = 14;
    const int HISTORY_BUTTON_WIDTH = 90;
    // Items in the history and completion menus.
    const size_t MAX_MENU_ITEMS = 30;
    // Characters of an entry shown in the history menu.
    const size_t HISTORY_LABEL_LENGTH = 60;

    // Shared by every SQL window; the file keeps other processes' entries.
    SqlHistory &history()
    {
        static SqlHistory shared;
        return shared;
    }

    // One line: whitespace runs become one space, long text ends in "...".
    std::string historyLabel(const std::string &sql)
    {
        std::string line;
        size_t characters = 0;
        bool space = false;
        for (size_t i = 0; i < sql.size(); ++i)
        {
            const char c = sql[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                space = !line.empty();
                continue;
            }
            // Count characters, not UTF-8 continuation bytes.
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            {
                if (characters + (space ? 1 : 0) >= HISTORY_LABEL_LENGTH)
                    return escapeLabel(line + "...", true);
                if (space)
                {
                    line += ' ';
                    ++characters;
                }
                ++characters;
                space = false;
            }
            line += c;
        }
        return escapeLabel(line, true);
    }

    // Style letters, in the order of SqlTokenKind and of SqlWindow::styles_.
    char styleLetter(SqlTokenKind kind)
    {
        return static_cast<char>('A' + static_cast<int>(kind));
    }

    bool isDark(Fl_Color color)
    {
        uchar r, g, b;
        Fl::get_color(color, r, g, b);
        return 299 * r + 587 * g + 114 * b < 128000;
    }
} // namespace

// Ctrl+Enter (Command+Enter on macOS) runs the SQL instead of adding a line;
// Ctrl+Up / Down or Alt+Up / Down walk the history; Tab or Ctrl+Space completes.
class SqlWindow::Editor : public Fl_Text_Editor
{
private:
    SqlWindow *owner_;

public:
    Editor(int x, int y, int w, int h, SqlWindow *owner) : Fl_Text_Editor(x, y, w, h), owner_(owner) {}

    int handle(int event) override
    {
        if (event == FL_KEYBOARD)
        {
            const int key = Fl::event_key();
            if ((key == FL_Enter || key == FL_KP_Enter) && Fl::event_state(FL_COMMAND))
            {
                owner_->run();
                return 1;
            }
            // Cmd on macOS, as the manual says; Ctrl there is often taken
            // by Mission Control, and Alt (Option) works everywhere.
            if ((key == FL_Up || key == FL_Down) && Fl::event_state(FL_COMMAND | FL_CTRL | FL_ALT))
            {
                owner_->recallHistory(key == FL_Up ? -1 : 1);
                return 1;
            }
            const bool tab = key == FL_Tab && !Fl::event_state(FL_SHIFT | FL_CTRL | FL_ALT | FL_META);
            if ((tab || (key == ' ' && Fl::event_state(FL_CTRL))) && owner_->complete())
                return 1;
            if (key == ' ' && Fl::event_state(FL_CTRL))
                return 1; // nothing to complete; no space either
        }
        return Fl_Text_Editor::handle(event);
    }
};

SqlWindow::SqlWindow(RunCallback run) : run_(std::move(run))
{
    window_ = std::make_unique<Fl_Double_Window>(WINDOW_WIDTH, WINDOW_HEIGHT, "SQL");
    buffer_ = std::make_unique<Fl_Text_Buffer>();
    const int bottom = WINDOW_HEIGHT - MARGIN - BUTTON_HEIGHT;
    editor_ = new Editor(MARGIN, MARGIN, WINDOW_WIDTH - 2 * MARGIN, bottom - 2 * MARGIN, this);
    editor_->buffer(buffer_.get());
    editor_->textfont(FL_COURIER);
    editor_->textsize(FONT_SIZE);
    style_buffer_ = std::make_unique<Fl_Text_Buffer>();
    editor_->highlight_data(style_buffer_.get(), styles_.data(), static_cast<int>(styles_.size()), 'A', nullptr, nullptr);
    buffer_->add_modify_callback(modifiedCallback, this);

    auto *row = new Fl_Group(MARGIN, bottom, WINDOW_WIDTH - 2 * MARGIN, BUTTON_HEIGHT);
    const int close_x = WINDOW_WIDTH - MARGIN - BUTTON_WIDTH;
    const int run_x = close_x - MARGIN - BUTTON_WIDTH;
    history_button_ = new Fl_Button(MARGIN, bottom, HISTORY_BUTTON_WIDTH, BUTTON_HEIGHT, "History");
    history_button_->tooltip("SQL run before (Ctrl+Up / Ctrl+Down)");
    history_button_->callback(historyCallback, this);
    const int status_x = MARGIN + HISTORY_BUTTON_WIDTH + MARGIN;
    status_ = new Fl_Box(status_x, bottom, run_x - MARGIN - status_x, BUTTON_HEIGHT);
    status_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    run_button_ = new Fl_Button(run_x, bottom, BUTTON_WIDTH, BUTTON_HEIGHT, "Run");
    run_button_->tooltip("Run the SQL (Ctrl+Enter). A single SELECT is shown as a sheet.");
    run_button_->callback(runCallback, this);
    auto *close = new Fl_Button(close_x, bottom, BUTTON_WIDTH, BUTTON_HEIGHT, "Close");
    close->callback(closeCallback, this);
    row->resizable(status_);
    row->end();

    window_->resizable(editor_);
    window_->size_range(300, 150);
    window_->end();
}

SqlWindow::~SqlWindow()
{
    buffer_->remove_modify_callback(modifiedCallback, this);
    // The editor unregisters callbacks from its buffer when destroyed.
    window_.reset();
}

void SqlWindow::show(const std::string &file_name)
{
    setFileName(file_name);
    // The theme may have changed since the last time.
    restyle();
    window_->show();
    editor_->take_focus();
}

void SqlWindow::setFileName(const std::string &file_name)
{
    title_ = "SQL - " + file_name;
    window_->copy_label(title_.c_str());
}

bool SqlWindow::shown() const
{
    return window_->shown();
}

void SqlWindow::setRunning(bool running)
{
    if (running)
    {
        run_button_->deactivate();
        setStatus("Running...");
    }
    else
        run_button_->activate();
}

void SqlWindow::setStatus(const std::string &message, bool error)
{
    status_text_ = escapeLabel(message);
    status_->label(status_text_.c_str());
    status_->labelcolor(error ? FL_RED : FL_FOREGROUND_COLOR);
    status_tooltip_.set(status_, status_text_);
    status_->redraw();
}

void SqlWindow::run()
{
    if (!run_button_->active())
        return;
    char *text = buffer_->text();
    const std::string sql = text ? text : "";
    free(text);
    if (sql.find_first_not_of(" \t\r\n") == std::string::npos)
        return;
    history().add(sql);
    history_cursor_.reset();
    run_(sql);
}

// Statements are short: the whole text is styled again after every edit.
// Only colors change: other fonts (bold) have other metrics and misalign lines.
void SqlWindow::restyle()
{
    const bool dark = isDark(editor_->color());
    const Fl_Color plain = editor_->textcolor();
    const Fl_Color keyword = dark ? fl_rgb_color(110, 170, 255) : fl_rgb_color(0, 70, 190);
    const Fl_Color string = dark ? fl_rgb_color(230, 160, 110) : fl_rgb_color(170, 60, 0);
    const Fl_Color number = dark ? fl_rgb_color(150, 210, 150) : fl_rgb_color(0, 120, 60);
    const Fl_Color comment = dark ? fl_rgb_color(140, 140, 140) : fl_rgb_color(120, 120, 120);
    styles_[static_cast<int>(SqlTokenKind::Plain)] = {plain, FL_COURIER, FONT_SIZE, 0, 0};
    styles_[static_cast<int>(SqlTokenKind::Keyword)] = {keyword, FL_COURIER, FONT_SIZE, 0, 0};
    styles_[static_cast<int>(SqlTokenKind::String)] = {string, FL_COURIER, FONT_SIZE, 0, 0};
    styles_[static_cast<int>(SqlTokenKind::Number)] = {number, FL_COURIER, FONT_SIZE, 0, 0};
    styles_[static_cast<int>(SqlTokenKind::Comment)] = {comment, FL_COURIER, FONT_SIZE, 0, 0};

    char *text = buffer_->text();
    const std::string sql = text ? text : "";
    free(text);
    std::string styles(sql.size(), styleLetter(SqlTokenKind::Plain));
    for (const auto &token : tokenizeSql(sql))
        std::fill(styles.begin() + static_cast<std::ptrdiff_t>(token.begin),
                  styles.begin() + static_cast<std::ptrdiff_t>(token.end), styleLetter(token.kind));
    style_buffer_->text(styles.c_str());
    editor_->redraw();
}

void SqlWindow::modifiedCallback(int, int inserted, int deleted, int, const char *, void *data)
{
    auto *self = static_cast<SqlWindow *>(data);
    if (inserted > 0 || deleted > 0)
    {
        // Typing makes the text a new draft.
        if (!self->replacing_text_)
            self->history_cursor_.reset();
        self->restyle();
    }
}

void SqlWindow::setText(const std::string &sql)
{
    replacing_text_ = true;
    buffer_->text(sql.c_str());
    replacing_text_ = false;
    editor_->insert_position(buffer_->length());
    editor_->show_insert_position();
}

void SqlWindow::recallHistory(int direction)
{
    if (!history_cursor_.walking())
        history().reload(); // other windows may have run SQL
    std::optional<std::string> sql;
    if (direction < 0)
    {
        char *text = buffer_->text();
        const std::string current = text ? text : "";
        free(text);
        sql = history_cursor_.previous(history().entries(), current);
    }
    else
        sql = history_cursor_.next(history().entries());
    if (sql)
        setText(*sql);
    else
        fl_beep();
}

void SqlWindow::showHistoryMenu()
{
    history().reload();
    const auto &entries = history().entries();
    std::vector<std::string> labels;
    for (auto it = entries.rbegin(); it != entries.rend() && labels.size() < MAX_MENU_ITEMS; ++it)
        labels.push_back(historyLabel(*it));
    std::vector<Fl_Menu_Item> items;
    if (labels.empty())
        items.push_back({"No SQL has been run yet", 0, nullptr, nullptr, FL_MENU_INACTIVE});
    for (size_t i = 0; i < labels.size(); ++i)
        items.push_back({labels[i].c_str(), 0, nullptr, reinterpret_cast<void *>(static_cast<intptr_t>(i + 1)), 0});
    items.push_back({nullptr});
    const Fl_Menu_Item *picked;
    {
        NestedLoopGuard guard;
        picked = items.front().pulldown(history_button_->x(), history_button_->y() + history_button_->h(),
                                        history_button_->w(), 0);
    }
    const auto index = picked ? reinterpret_cast<intptr_t>(picked->user_data()) : 0;
    if (index <= 0 || static_cast<size_t>(index) > entries.size())
        return;
    setText(entries[entries.size() - static_cast<size_t>(index)]);
    history_cursor_.reset();
    editor_->take_focus();
}

bool SqlWindow::complete()
{
    char *text = buffer_->text();
    const std::string sql = text ? text : "";
    free(text);
    const int cursor = editor_->insert_position();
    const SqlCompletion completion = completeSql(sql, static_cast<size_t>(cursor), schema_);
    if (completion.candidates.empty())
        return false;
    auto replace = [&](const std::string &with)
    {
        buffer_->replace(static_cast<int>(completion.begin), static_cast<int>(completion.end), with.c_str());
        editor_->insert_position(static_cast<int>(completion.begin + with.size()));
        editor_->show_insert_position();
    };
    if (completion.candidates.size() == 1)
    {
        replace(completion.candidates.front());
        return true;
    }
    // Typing on narrows them down; else a menu to choose from.
    const std::string common = commonCompletionPrefix(completion.candidates);
    if (common.size() > completion.end - completion.begin)
    {
        replace(common);
        return true;
    }
    std::vector<std::string> labels;
    for (const auto &candidate : completion.candidates)
    {
        if (labels.size() == MAX_MENU_ITEMS)
            break;
        labels.push_back(escapeLabel(candidate, true));
    }
    std::vector<Fl_Menu_Item> items;
    for (size_t i = 0; i < labels.size(); ++i)
        items.push_back({labels[i].c_str(), 0, nullptr, reinterpret_cast<void *>(static_cast<intptr_t>(i + 1)), 0});
    items.push_back({nullptr});
    int x = 0, y = 0;
    editor_->position_to_xy(cursor, &x, &y);
    const Fl_Menu_Item *picked;
    {
        NestedLoopGuard guard;
        picked = items.front().popup(x, y + editor_->textsize() + 4);
    }
    const auto index = picked ? reinterpret_cast<intptr_t>(picked->user_data()) : 0;
    if (index > 0)
        replace(completion.candidates[static_cast<size_t>(index) - 1]);
    return true;
}

void SqlWindow::historyCallback(Fl_Widget *, void *data)
{
    static_cast<SqlWindow *>(data)->showHistoryMenu();
}

void SqlWindow::runCallback(Fl_Widget *, void *data)
{
    static_cast<SqlWindow *>(data)->run();
}

void SqlWindow::closeCallback(Fl_Widget *, void *data)
{
    static_cast<SqlWindow *>(data)->window_->hide();
}
