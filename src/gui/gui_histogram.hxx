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
// range and count; double-clicking it filters the sheet to its rows. Like
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
    Fl_Box *status_ = nullptr;
    Fl_Button *save_button_ = nullptr;
    Chart *chart_ = nullptr;
    Fl_Box *detail_ = nullptr;
    std::shared_ptr<TableModel> model_;
    HistogramState state_;
    size_t column_ = 0;
    // After a sheet switch: once its columns are known, drop a histogram of
    // a column it does not have.
    bool check_column_ = false;
    std::optional<size_t> hovered_;
    std::string title_;
    // Shared by the column choice; the labels must outlive the items.
    std::vector<std::string> column_labels_;
    std::vector<Fl_Menu_Item> column_menu_;

    void start(size_t column);
    void rebuildColumnMenu();
    void setTitle(const std::string &sheet_name);
    void showDetail();
    // A bar was double-clicked: a bin, or one of the most frequent values.
    void filterBy(size_t index);
    static void deferredUpdate(void *data);
    static void columnCallback(Fl_Widget *, void *data);
    static void saveCallback(Fl_Widget *, void *data);
    static void closeCallback(Fl_Widget *, void *data);
};

#endif // _GUI_HISTOGRAM_HXX_
