#ifndef _GUI_SQL_HXX_
#define _GUI_SQL_HXX_

#include <FL/Fl_Text_Display.H>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gui_theme.hxx"
#include "sql_history.hxx"
#include "sql_text.hxx"

class Fl_Box;
class Fl_Button;
class Fl_Double_Window;
class Fl_Text_Buffer;
class Fl_Widget;

// Non-modal editor for SQL run against one TableWindow's file. Run (or
// Ctrl+Enter) passes the text to the callback; the owner reports back with
// setRunning() and setStatus(). SQL that is run goes into the history shared
// by all windows (History button, Ctrl/Alt+Up and Down); Tab or Ctrl+Space
// completes names from setSchema() and keywords.
class SqlWindow
{
public:
    using RunCallback = std::function<void(const std::string &sql)>;

    explicit SqlWindow(RunCallback run);
    ~SqlWindow();
    SqlWindow(const SqlWindow &) = delete;
    SqlWindow &operator=(const SqlWindow &) = delete;

    void show(const std::string &file_name);
    void setFileName(const std::string &file_name);
    bool shown() const;
    void setRunning(bool running);
    void setStatus(const std::string &message, bool error = false);
    // The tables and columns to complete, of the file shown.
    void setSchema(std::vector<SqlTable> schema) { schema_ = std::move(schema); }

private:
    class Editor;

    std::unique_ptr<Fl_Double_Window> window_;
    std::unique_ptr<Fl_Text_Buffer> buffer_;
    // One style letter per byte of buffer_, for keyword / string / ... colors.
    std::unique_ptr<Fl_Text_Buffer> style_buffer_;
    std::array<Fl_Text_Display::Style_Table_Entry, 5> styles_{};
    Editor *editor_ = nullptr;
    Fl_Button *run_button_ = nullptr;
    Fl_Button *history_button_ = nullptr;
    Fl_Box *status_ = nullptr;
    RunCallback run_;
    std::string title_;
    std::string status_text_;
    ChangingTooltip status_tooltip_;
    std::vector<SqlTable> schema_;
    SqlHistoryCursor history_cursor_;
    bool replacing_text_ = false; // history text, not an edit by hand

    void run();
    void setText(const std::string &sql);
    // -1: older, +1: newer.
    void recallHistory(int direction);
    void showHistoryMenu();
    // False if there is nothing to complete (Tab then inserts a tab).
    bool complete();
    void restyle();
    static void modifiedCallback(int pos, int inserted, int deleted, int restyled, const char *deleted_text, void *data);
    static void runCallback(Fl_Widget *, void *data);
    static void historyCallback(Fl_Widget *, void *data);
    static void closeCallback(Fl_Widget *, void *data);
};

#endif // _GUI_SQL_HXX_
