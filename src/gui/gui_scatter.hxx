#ifndef _GUI_SCATTER_HXX_
#define _GUI_SCATTER_HXX_

#include <FL/Fl_Menu_Item.H>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "table.hxx"

class Fl_Box;
class Fl_Check_Button;
class Fl_Choice;
class Fl_Double_Window;
class Fl_Widget;

// Non-modal window plotting two columns of a TableWindow's current sheet
// against each other (TableModel::startScatter()): a scatter plot of a sample
// of the rows, or a 2D density of all of them. Pointing at a point or a cell
// shows its values; clicking a point goes to its row, double-clicking a cell
// or dragging a rectangle filters the sheet. Pearson's correlation and the
// least-squares line of all the rows plotted are shown above the plot. Like
// HistogramWindow it does not subscribe to the model: the owner calls
// update() after events, and setModel() when the sheet changes.
class ScatterWindow
{
public:
    // Called with the model, a view row and a column when a point is clicked.
    using RowCallback = std::function<void(const std::shared_ptr<TableModel> &model, size_t view_row, size_t column)>;

    ScatterWindow();
    ~ScatterWindow();
    ScatterWindow(const ScatterWindow &) = delete;
    ScatterWindow &operator=(const ScatterWindow &) = delete;

    // Shows the window with the plot of the columns.
    void show(std::shared_ptr<TableModel> model, const std::string &sheet_name, size_t x_column, size_t y_column);
    // Another sheet (or the same after a reopen): the same columns, if it has them.
    void setModel(std::shared_ptr<TableModel> model, const std::string &sheet_name);
    bool shown() const;
    // Picks up the model's progress.
    void update();
    void setRowCallback(RowCallback callback) { row_callback_ = std::move(callback); }

private:
    class Chart;

    std::unique_ptr<Fl_Double_Window> window_;
    Fl_Choice *x_choice_ = nullptr;
    Fl_Choice *y_choice_ = nullptr;
    Fl_Choice *mode_choice_ = nullptr;
    Fl_Check_Button *log_button_ = nullptr;
    Fl_Box *status_ = nullptr;
    Fl_Box *stats_ = nullptr;
    Fl_Check_Button *fit_button_ = nullptr;
    Chart *chart_ = nullptr;
    Fl_Box *detail_ = nullptr;
    std::shared_ptr<TableModel> model_;
    ScatterState state_;
    size_t x_column_ = 0;
    size_t y_column_ = 1;
    // After a sheet switch: once its columns are known, drop a plot of
    // columns it does not have.
    bool check_columns_ = false;
    bool density_ = false;
    std::optional<size_t> hovered_point_;                // index into the points
    std::optional<std::pair<size_t, size_t>> hovered_cell_; // x and y bins
    std::string title_;
    RowCallback row_callback_;
    // Shared by the column choices; the labels must outlive the items.
    std::vector<std::string> column_labels_;
    std::vector<Fl_Menu_Item> column_menu_;

    void start(size_t x_column, size_t y_column);
    void rebuildColumnMenu();
    void setTitle(const std::string &sheet_name);
    void showDetail();
    void goToPoint(size_t index);
    void filterCell(size_t x_bin, size_t y_bin);
    // A rectangle in coordinates (see histogramCoordinate()).
    void filterRange(double x_low, double x_high, double y_low, double y_high);
    void addFilterConditions(std::vector<FilterCondition> conditions);
    static void deferredUpdate(void *data);
    static void columnCallback(Fl_Widget *, void *data);
    static void modeCallback(Fl_Widget *, void *data);
    static void closeCallback(Fl_Widget *, void *data);
};

#endif // _GUI_SCATTER_HXX_
