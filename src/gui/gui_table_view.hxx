#ifndef _GUI_TABLE_VIEW_HXX_
#define _GUI_TABLE_VIEW_HXX_

#include <FL/Fl_Table.H>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "table.hxx"
#include "cell_format.hxx"
#include "gui_theme.hxx"

// Popup menus and alerts run a nested event loop that also fires timeouts.
// While one is on the stack, Auto Revert must not reopen the file, since that
// deletes the views whose methods are still running.
class NestedLoopGuard
{
    static inline int depth_ = 0;

public:
    NestedLoopGuard() { ++depth_; }
    ~NestedLoopGuard() { --depth_; }
    NestedLoopGuard(const NestedLoopGuard &) = delete;
    NestedLoopGuard &operator=(const NestedLoopGuard &) = delete;
    static bool active() { return depth_ > 0; }
    // Runs the action now, or once no nested loop (nor a menu, which holds
    // the grab) is left on the stack. For commands that reopen the file or
    // delete a window: the macOS menu bar works during alerts and popups.
    static void defer(std::function<void()> action);
};

class GuiTableView : public Fl_Table
{
public:
    // Top-left visible cell, and the selected cell (-1: none).
    struct ScrollPosition
    {
        int row = 0;
        int col = 0;
        int cursor_row = -1;
        int cursor_col = -1;
    };

private:
    // Shared with pending Fl::awake() callbacks so that they can tell
    // whether the view still exists (both are touched on the main thread only).
    struct AwakeToken
    {
        GuiTableView *view = nullptr;
        std::atomic<bool> pending{false};
    };

    std::shared_ptr<TableModel> model_;
    Subscription subscription_;
    std::shared_ptr<AwakeToken> token_;
    size_t sized_columns_ = 0;
    bool error_reported_ = false;
    // A memory checkpoint waits for another view's dialog.
    bool ask_again_ = false;
    bool use_monospace_font_ = false;
    // Rows of sequential sources appear as they are read, so a restored
    // position is approached step by step until it exists.
    std::optional<ScrollPosition> pending_scroll_;
    // A jumpTo() row that has not been read yet: the view scrolls to the end
    // until it exists, then selects it.
    std::optional<ScrollPosition> pending_jump_;
    // Hidden and frozen columns keep their index with a width of 0 in the
    // scrolled area; their widths are kept here. Frozen columns (the first
    // frozen_) are drawn in the row header, right of the row numbers.
    HiddenColumns hidden_;
    int frozen_ = 0;
    std::map<int, int> saved_widths_;
    int row_number_width_ = 0;
    bool frozen_drag_ = false; // a selection started on a frozen cell
    // The model's applied view, for the header icons.
    ViewSpec view_;
    // Sort and filter to restore once the model can apply them (after a reopen).
    std::optional<ViewSpec> pending_view_;
    // The model's search, for highlighting its hits.
    SearchState search_;
    // Column whose sort icon was pressed with the left button; releasing on
    // the same icon sorts.
    int header_push_col_ = -1;
    // Last top-left selected cell reported to the selection callback.
    struct SelectedCell
    {
        int row = -1;
        int col = -1;
        CellState state = CellState::OutOfRange;
        bool operator==(const SelectedCell &) const = default;
    };
    SelectedCell reported_selection_;
    std::function<void()> selection_callback_;
    std::function<void()> update_callback_;
    std::function<void(size_t)> filter_column_callback_;
    std::function<void(size_t)> stats_column_callback_;
    std::function<void(size_t)> histogram_column_callback_;
    std::function<void(size_t)> scatter_column_callback_;
    std::function<void()> cell_activate_callback_;
    std::function<void()> columns_changed_callback_;
    // Editing a cell (in edit mode): an input over it.
    class CellEditor;
    CellEditor *editor_ = nullptr;
    int edit_row_ = -1;
    int edit_col_ = -1;

    static void awakeHandler(void *data);
    void applyFont();
    void syncWithModel();
    void askToContinueReading();
    void autosizeColumns();
    void applyPendingScroll();
    void applyPendingJump();
    void skipHiddenColumn(int direction);
    void columnsChanged();
    bool collapsed(int col) const { return col < frozen_ || hidden_.contains(static_cast<size_t>(col)); }
    void setColumnWidth(int col, int width);
    void collapseColumn(int col);
    void expandColumn(int col);
    void updateRowHeaderWidth();
    int frozenColumnAt(int x, int *right = nullptr); // not const: Fl_Table::cols() is not
    int handleFrozen(int event);
    SortIndicator sortIndicator(int col) const;
    void drawDataCell(int R, int C, int X, int Y, int W, int H);
    void applyPendingView();
    void clampSelection();
    void scrollTo(int row, int col);
    SelectedCell topLeftSelectedCell();
    void checkSelectionChange(bool force = false);
    void showSortMenu(int col);
    void showCellMenu(int row, int col, TableContext context);
    void setFilter(std::optional<Filter> filter);
    bool onSortIcon(int col);
    void cycleSort(int col);
    bool handleEditKey();
    void placeEditor();
    void pasteText(const std::string &text);
    void selectCell(int row, int col);

public:
    GuiTableView(int x, int y, int w, int h, const char *label = 0);
    virtual ~GuiTableView() override;

    void setModel(std::shared_ptr<TableModel> model);
    const std::shared_ptr<TableModel> &model() const { return model_; }

    ScrollPosition scrollPosition() const { return {toprow, leftcol, current_row, current_col}; }
    // What a reopen should restore: a restore still waiting for rows (or for
    // the model) wins over the current state, so that two quick reopens in a
    // row keep it.
    ScrollPosition restorableScrollPosition() const { return pending_scroll_.value_or(scrollPosition()); }
    ViewSpec restorableView() const;
    void restoreScrollPosition(ScrollPosition position);
    // Applies the sort and filter of a previous model of the same sheet once
    // this model can (e.g. after all rows of a CSV file are read).
    void restoreView(ViewSpec spec);

    void set_use_monospace_font(bool use);
    virtual int handle(int event) override;
    virtual void draw() override;
    virtual void draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H) override;
    TsvCopyResult selectedCellsToTsv();
    // Copies the selection to the clipboard as TSV, telling the user about
    // rows that could not be copied because they are still loading.
    void copySelection();

    // Full text of the top-left cell of the selection; empty while it is
    // loading or when nothing is selected.
    std::string topLeftSelectedCellText();
    // Called on the main thread when the top-left selected cell changes or
    // finishes loading.
    void setSelectionCallback(std::function<void()> callback) { selection_callback_ = std::move(callback); }
    // Called on the main thread after the view caught up with model events
    // (loaded rows, search progress, ...).
    void setUpdateCallback(std::function<void()> callback) { update_callback_ = std::move(callback); }
    // Called for "Filter by This Column…" in the context menus.
    void setFilterColumnCallback(std::function<void(size_t)> callback) { filter_column_callback_ = std::move(callback); }
    // Called for "Column Statistics" in the context menus.
    void setStatsColumnCallback(std::function<void(size_t)> callback) { stats_column_callback_ = std::move(callback); }
    // Called with the column whose histogram is asked for (context menus).
    void setHistogramColumnCallback(std::function<void(size_t)> callback)
    {
        histogram_column_callback_ = std::move(callback);
    }
    // Called with the column to plot as Y (context menus).
    void setScatterColumnCallback(std::function<void(size_t)> callback)
    {
        scatter_column_callback_ = std::move(callback);
    }
    // Called when a cell is double-clicked (after it was selected) and not
    // edited.
    void setCellActivateCallback(std::function<void()> callback) { cell_activate_callback_ = std::move(callback); }
    void activateCell()
    {
        if (cell_activate_callback_)
            cell_activate_callback_();
    }
    // A double click edits the cell in edit mode, else activates it.
    void cellDoubleClicked(int row, int col);

    // Editing (only in edit mode, see TableBook::setEditing()). F2, Enter,
    // typing and double clicks edit the cell under the cursor, Delete and
    // Backspace clear the selected cells.
    // Edits the cell, starting with `typed` instead of its value if given.
    void editCell(int row, int col, const std::optional<std::string> &typed = std::nullopt);
    bool editingCell() const;
    // Ends the editing of a cell, keeping what was typed or not, and moves
    // the selection by `rows` and `cols` from it.
    void finishEditing(bool commit, int rows = 0, int cols = 0);
    void clearSelectedCells();
    // Pastes the clipboard (TSV) into the selection.
    void paste();
    void undo();
    void redo();

    // Hides the columns [first, last], unless that would hide every column;
    // the selection moves to a visible column.
    void hideColumns(int first, int last);
    void showColumn(int col);
    void showAllColumns();
    const HiddenColumns &hiddenColumns() const { return hidden_; }
    // Hides the columns hidden in a previous view of the same sheet (after a reopen).
    void restoreHiddenColumns(const HiddenColumns &hidden);
    // Keeps the first `count` columns in place when scrolling sideways; 0 unfreezes.
    void freezeColumns(int count);
    int frozenColumns() const { return frozen_; }
    // Whether the sheet has more rows than the table can show (about 89
    // million); only the first ones are shown.
    bool rowsCut() const;
    // Called after columns are hidden, shown, frozen or unfrozen (e.g. to update menus).
    void setColumnsChangedCallback(std::function<void()> callback) { columns_changed_callback_ = std::move(callback); }

    // Selects the cell (view row, column) and scrolls it into view. A hidden
    // column is shown. A row
    // beyond those read so far is selected once it is read (or the last row,
    // if there are fewer).
    void jumpTo(int row, int col);
    // Picks up the model's search hits. Model events do this too, but
    // starting or cancelling a search emits none.
    void updateSearchHits();
};

#endif // _GUI_TABLE_VIEW_HXX_
