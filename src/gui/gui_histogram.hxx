#ifndef _GUI_HISTOGRAM_HXX_
#define _GUI_HISTOGRAM_HXX_

#include <FL/Fl_Menu_Item.H>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "table.hxx"

class Fl_Box;
class Fl_Button;
class Fl_Choice;
class Fl_Double_Window;
class Fl_Widget;

// Non-modal window with the histogram of one column of a TableWindow's
// current sheet (TableModel::startHistogram()): bars of numbers or dates, or
// for other columns the most frequent values. Pointing at a bar shows its
// range and count; double-clicking it filters the sheet to its rows. Bars
// can be stacked by another column ("Stack by"), in a color per value, and
// a part of a bar filtered to. Like
// GuiStatsPane it does not subscribe to the model: the owner calls update()
// after events, and setModel() when the sheet changes.
class HistogramWindow
{
public:
    HistogramWindow();
    ~HistogramWindow();
    HistogramWindow(const HistogramWindow &) = delete;
    HistogramWindow &operator=(const HistogramWindow &) = delete;

    // Shows the window with the column's histogram.
    void show(std::shared_ptr<TableModel> model, const std::string &sheet_name, size_t column);
    // Another sheet (or the same after a reopen): the same column, if it has one.
    void setModel(std::shared_ptr<TableModel> model, const std::string &sheet_name);
    bool shown() const;
    // Picks up the model's progress.
    void update();

private:
    class Chart;

    std::unique_ptr<Fl_Double_Window> window_;
    Fl_Choice *column_choice_ = nullptr;
    Fl_Choice *group_choice_ = nullptr; // "(none)", then the columns
    Fl_Box *status_ = nullptr;
    Fl_Button *save_button_ = nullptr;
    Chart *chart_ = nullptr;
    Fl_Box *detail_ = nullptr;
    std::shared_ptr<TableModel> model_;
    HistogramState state_;
    size_t column_ = 0;
    std::optional<size_t> group_column_;
    // After a sheet switch: once its columns are known, drop a histogram of
    // a column it does not have.
    bool check_column_ = false;
    std::optional<size_t> hovered_;
    std::optional<size_t> hovered_group_; // the part of a stacked bar pointed at
    std::string title_;
    // Shared by the column choice; the labels must outlive the items.
    std::vector<std::string> column_labels_;
    std::vector<Fl_Menu_Item> column_menu_;
    std::vector<Fl_Menu_Item> group_menu_;

    void start(size_t column, std::optional<size_t> group_column);
    void rebuildColumnMenu();
    void setTitle(const std::string &sheet_name);
    void showDetail();
    // A bar was double-clicked: a bin (a part of it, of a stacked bar), or
    // one of the most frequent values.
    void filterBy(size_t index, std::optional<size_t> group);
    static void deferredUpdate(void *data);
    static void columnCallback(Fl_Widget *, void *data);
    static void groupCallback(Fl_Widget *, void *data);
    static void saveCallback(Fl_Widget *, void *data);
    static void closeCallback(Fl_Widget *, void *data);
};

#endif // _GUI_HISTOGRAM_HXX_
