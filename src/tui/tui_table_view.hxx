#ifndef _TUI_TABLE_VIEW_HXX_
#define _TUI_TABLE_VIEW_HXX_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include "cell_format.hxx"
#include "table.hxx"

// One sheet drawn as a grid with a cursor and a rectangular selection.
class TuiTableView : public ftxui::ComponentBase
{
public:
    // Runs a closure on the UI thread and redraws.
    using Poster = std::function<void(std::function<void()>)>;

    struct ScrollPosition
    {
        size_t top_row = 0;
        size_t left_col = 0;
        size_t cursor_row = 0;
        size_t cursor_col = 0;
    };

    // Inclusive cell range.
    struct Selection
    {
        size_t row_top = 0;
        size_t col_left = 0;
        size_t row_bottom = 0;
        size_t col_right = 0;
    };

private:
    // Shared with posted closures so that they can tell whether the view
    // still exists (both are touched on the UI thread only).
    struct UpdateToken
    {
        TuiTableView *view = nullptr;
        std::atomic<bool> pending{false};
    };

    // Screen position of a drawn column, for mouse hits.
    struct DrawnColumn
    {
        size_t column = 0;
        int x_min = 0;
        int x_max = 0;
    };

    std::shared_ptr<TableModel> model_;
    Poster poster_;
    Subscription subscription_;
    std::shared_ptr<UpdateToken> token_;

    std::vector<int> column_widths_;
    HiddenColumns hidden_; // skipped when drawing and moving the cursor
    size_t frozen_ = 0;    // leftmost columns that stay in place when scrolling
    size_t sized_columns_ = 0;
    bool error_reported_ = false;

    size_t cursor_row_ = 0;
    size_t cursor_col_ = 0;
    std::optional<std::pair<size_t, size_t>> anchor_; // other corner of the selection
    bool visual_ = false;                              // cursor moves extend the selection
    size_t top_row_ = 0;
    size_t left_col_ = 0;
    // Scrolling with the wheel moves the view away from the cursor; moving the
    // cursor brings it back into view.
    bool follow_cursor_ = true;
    std::optional<ScrollPosition> pending_scroll_;
    // The model's applied view, for the header indicators.
    ViewSpec view_;
    // Sort and filter to restore once the model can apply them (after a reopen).
    std::optional<ViewSpec> pending_view_;

    // Layout of the last frame.
    ftxui::Box box_;
    int body_rows_ = 0;
    int row_header_width_ = 0;
    std::vector<DrawnColumn> drawn_columns_;
    bool dragging_ = false;
    std::chrono::steady_clock::time_point last_click_time_{};
    std::pair<size_t, size_t> last_click_cell_{SIZE_MAX, SIZE_MAX};

    const SearchState *search_ = nullptr; // hits to highlight, owned by the app

    std::function<void(const std::string &)> error_callback_;
    std::function<void()> activate_callback_;
    std::function<void()> update_callback_;

    void sync();
    void autosizeColumns();
    void applyPendingScroll();
    void applyPendingView();
    void clampCursor();
    void scrollCursorIntoView(int body_width);
    int frozenWidth() const; // frozen columns drawn, with their separators
    void moveCursor(long long rows, long long cols, bool extend);
    void setCursor(size_t row, size_t col, bool extend);
    std::optional<std::pair<size_t, size_t>> cellAt(int x, int y) const;
    bool handleMouse(ftxui::Event &event);
    int columnWidth(size_t column) const;
    ftxui::Element renderGrid(const ftxui::Box &box);

public:
    TuiTableView(std::shared_ptr<TableModel> model, Poster poster);
    ~TuiTableView() override;

    const std::shared_ptr<TableModel> &model() const { return model_; }

    ftxui::Element OnRender() override;
    bool OnEvent(ftxui::Event event) override;
    bool Focusable() const override { return true; }

    ScrollPosition scrollPosition() const;
    void restoreScrollPosition(ScrollPosition position);
    // Applies the sort and filter of a previous model of the same sheet once
    // this model can (e.g. after all rows of a CSV file are read).
    void restoreView(ViewSpec spec);

    size_t cursorRow() const { return cursor_row_; }
    size_t cursorColumn() const { return cursor_col_; }
    Selection selection() const;
    bool hasRangeSelection() const { return anchor_.has_value(); }
    bool selecting() const { return visual_; }

    void widenColumn(int delta);
    // Moves the cursor to the cell (view row, column), clearing the selection.
    // A hidden column is shown. A row beyond those read so far is approached
    // as rows arrive (the last row if there are fewer).
    void jumpTo(size_t row, size_t col);
    // Hides the columns [first, last], unless that would hide every column;
    // the cursor moves to a visible column.
    void hideColumns(size_t first, size_t last);
    void showAllColumns() { hidden_.clear(); }
    const HiddenColumns &hiddenColumns() const { return hidden_; }
    void restoreHiddenColumns(const HiddenColumns &hidden) { hidden_ = hidden; }
    // Keeps the columns up to `column` in place when scrolling sideways; 0 unfreezes.
    void freezeColumns(size_t count);
    size_t frozenColumns() const { return frozen_; }
    // Sorts by the cursor column: ascending, then descending, then file order.
    // False if the model cannot sort yet (still opening).
    bool cycleSort();
    const std::vector<SortKey> &sortKeys() const { return view_.sort; }
    // The model's applied view as of the last update.
    const ViewSpec &appliedView() const { return view_; }

    // Called on the UI thread when the model fails to load.
    void setErrorCallback(std::function<void(const std::string &)> callback)
    {
        error_callback_ = std::move(callback);
        sync();
    }
    // Called on Enter or a double click on a cell.
    void setActivateCallback(std::function<void()> callback) { activate_callback_ = std::move(callback); }
    // Called on the UI thread after the view caught up with model events
    // (loaded rows, search progress, ...).
    void setUpdateCallback(std::function<void()> callback) { update_callback_ = std::move(callback); }
    // Search whose hits are highlighted (nullptr: none). It must stay valid
    // until the next frame is drawn.
    void setSearchState(const SearchState *state) { search_ = state; }
};

#endif // _TUI_TABLE_VIEW_HXX_
