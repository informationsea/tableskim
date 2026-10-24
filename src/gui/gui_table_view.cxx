#include "gui_table_view.hxx"
#include "spdlog/spdlog.h"

#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <climits>
#include <cstdint>

#include "cell_format.hxx"
#include "gui.h"
#include "gui_theme.hxx"

namespace
{
    const int FONT_SIZE = 16;
    const int ROW_HEIGHT = FONT_SIZE + 8;
    // Fl_Table adds up the row heights in an int, which more rows overflow.
    const int MAX_ROWS = INT_MAX / ROW_HEIGHT - 1;
    const int MIN_COLUMN_WIDTH = 40;
    // Room for the sort and filter icons, so that the header label is not cut.
    const int HEADER_PADDING = 12 + SORT_ICON_WIDTH + FILTER_ICON_WIDTH;
    const int MAX_COLUMN_WIDTH = 320;
    const size_t AUTOSIZE_SAMPLE_ROWS = 100;
    // Menu items whose quick filter value is longer are cut.
    const size_t MENU_VALUE_BYTES = 40;

    enum class MenuAction : intptr_t
    {
        None,
        Copy,
        SortAscending,
        SortDescending,
        NoSort,
        FilterEqual,
        FilterNotEqual,
        FilterColumn,
        ClearFilter,
        ColumnStats,
        Histogram,
        ScatterPlot,
        HideColumns,
        ShowAllColumns,
        FreezeColumns,
        UnfreezeColumns,
    };

    Fl_Menu_Item menuItem(const char *label, MenuAction action, int flags = 0, int shortcut = 0)
    {
        return {label, shortcut, nullptr, reinterpret_cast<void *>(static_cast<intptr_t>(action)), flags};
    }

    MenuAction menuAction(const Fl_Menu_Item *item)
    {
        return item ? static_cast<MenuAction>(reinterpret_cast<intptr_t>(item->user_data())) : MenuAction::None;
    }

    std::string quickFilterLabel(const FilterCondition &condition)
    {
        std::string label = std::string("Filter: ") + filterOpLabel(condition.op);
        if (filterOpTakesValue(condition.op))
            label += " \"" + cellDisplayText(CellValue{condition.value}, MENU_VALUE_BYTES) + "\"";
        return escapeLabel(label, true);
    }

    void gui_table_callback(Fl_Widget *w, void *userdata)
    {
        auto *self = static_cast<GuiTableView *>(w);
        if (!self)
        {
            spdlog::error("Failed to get GuiTableView instance from callback widget");
            return;
        }
        if (!self->model())
        {
            spdlog::error("GuiTableView has no model in callback");
            return;
        }

        const int R = self->callback_row();
        const int C = self->callback_col();

        switch (self->callback_context())
        {
        case Fl_Table::CONTEXT_CELL:
        {
            // spdlog::debug("Cell clicked: ({}, {}) {}", R + 1, C + 1, Fl::event_clicks());
            // The sidebar follows the selection, so it also shows cells that
            // are still loading once they arrive.
            if (Fl::event() == FL_PUSH && R >= 0 && C >= 0 && Fl::event_clicks() == 1)
                self->cellDoubleClicked(R, C);
            break;
        }
        case Fl_Table::CONTEXT_ROW_HEADER:
            // spdlog::debug("Row header clicked: {}", R + 1);
            break;
        case Fl_Table::CONTEXT_COL_HEADER:
            // spdlog::debug("Column header clicked: {}", C + 1);
            break;
        default:
            break;
        }
    }

    // Clearing more cells at once would take too much memory for undo.
    const size_t MAX_CLEARED_CELLS = 5000000;

    const double DEFER_POLL_INTERVAL = 0.1;
    std::vector<std::function<void()>> deferred_actions;

    bool insideNestedLoop()
    {
        return NestedLoopGuard::active() || Fl::grab();
    }

    void runDeferredActions(void *)
    {
        // Timeouts also fire inside nested loops.
        if (insideNestedLoop())
        {
            Fl::add_timeout(DEFER_POLL_INTERVAL, runDeferredActions);
            return;
        }
        // An action may defer more (e.g. from an alert it shows).
        const std::vector<std::function<void()>> actions = std::move(deferred_actions);
        deferred_actions.clear();
        for (const auto &action : actions)
            action();
    }
} // namespace

// Keys that end the editing of a cell; everything else is the input's.
class GuiTableView::CellEditor : public Fl_Input
{
private:
    GuiTableView *view_;

public:
    explicit CellEditor(GuiTableView *view) : Fl_Input(0, 0, 0, 0), view_(view)
    {
        // Values may have line breaks (Alt+Enter types one).
        type(FL_MULTILINE_INPUT);
        box(FL_BORDER_BOX);
        hide();
    }

    int handle(int event) override
    {
        if (event == FL_KEYBOARD)
        {
            const int key = Fl::event_key();
            const bool shift = Fl::event_state(FL_SHIFT) != 0;
            if (key == FL_Enter || key == FL_KP_Enter)
            {
                if (Fl::event_state(FL_ALT | FL_CTRL | FL_META))
                {
                    insert("\n");
                    return 1;
                }
                view_->finishEditing(true, shift ? -1 : 1, 0);
                return 1;
            }
            if (key == FL_Tab)
            {
                view_->finishEditing(true, 0, shift ? -1 : 1);
                return 1;
            }
            if (key == FL_Escape)
            {
                view_->finishEditing(false);
                return 1;
            }
        }
        const int handled = Fl_Input::handle(event);
        // Clicking elsewhere keeps what was typed.
        if (event == FL_UNFOCUS)
            view_->finishEditing(true);
        return handled;
    }
};

void NestedLoopGuard::defer(std::function<void()> action)
{
    if (!insideNestedLoop())
    {
        action();
        return;
    }
    deferred_actions.push_back(std::move(action));
    Fl::remove_timeout(runDeferredActions);
    Fl::add_timeout(DEFER_POLL_INTERVAL, runDeferredActions);
}

GuiTableView::GuiTableView(int x, int y, int w, int h, const char *label)
    : Fl_Table(x, y, w, h, label),
      token_(std::make_shared<AwakeToken>())
{
    token_->view = this;
    row_header(1);
    col_header(1);
    rows(0);
    cols(0);
    col_resize(1);
    row_height_all(ROW_HEIGHT);
    col_header_height(ROW_HEIGHT);
    color(FL_BACKGROUND2_COLOR);
    callback(gui_table_callback);
    // In the table's scroll group, which clips it to the cells.
    editor_ = new CellEditor(this);
    // Fl_Table's constructor leaves its internal scroll group open; without
    // end() the next widget (e.g. the next sheet's view) would be nested in it.
    end();
}

GuiTableView::~GuiTableView()
{
    subscription_.reset(); // waits for an in-flight listener call
    token_->view = nullptr;
}

bool GuiTableView::rowsCut() const
{
    return model_ && model_->displayRowCount() > static_cast<size_t>(MAX_ROWS);
}

void GuiTableView::setModel(std::shared_ptr<TableModel> model)
{
    finishEditing(false);
    subscription_.reset();
    if (model != model_)
    {
        // Another table (e.g. a new SQL result): columns of the old one stay
        // neither hidden nor frozen, and its pending sort/filter is dropped.
        // Autosizing below gives every column its width again.
        hidden_ = {};
        frozen_ = 0;
        saved_widths_.clear();
        pending_view_.reset();
        pending_jump_.reset();
        updateRowHeaderWidth();
    }
    model_ = std::move(model);
    sized_columns_ = 0;
    error_reported_ = false;
    if (model_)
    {
        // Called on the worker thread: just schedule one main-thread update.
        subscription_ = model_->subscribe([token = token_](const TableEvent &)
                                          {
            if (!token->pending.exchange(true))
                Fl::awake(&GuiTableView::awakeHandler, new std::shared_ptr<AwakeToken>(token)); });
    }
    syncWithModel();
}

void GuiTableView::awakeHandler(void *data)
{
    std::unique_ptr<std::shared_ptr<AwakeToken>> holder(static_cast<std::shared_ptr<AwakeToken> *>(data));
    AwakeToken &token = **holder;
    token.pending = false;
    if (token.view)
        token.view->syncWithModel();
}

void GuiTableView::updateSearchHits()
{
    search_ = model_ ? model_->searchState() : SearchState{};
    redraw();
}

// Reading CSV / TSV keeps every row in memory; each memory checkpoint asks
// whether to go on.
void GuiTableView::askToContinueReading()
{
    // The dialog runs a nested loop, in which this is called again. A model
    // reports a checkpoint once, so another view asks after the dialog.
    static bool asking = false;
    if (!model_)
        return;
    if (asking)
    {
        if (!ask_again_ && model_->memoryCheckpoint())
        {
            ask_again_ = true;
            NestedLoopGuard::defer([token = token_]
                                   {
                if (token->view)
                {
                    token->view->ask_again_ = false;
                    token->view->askToContinueReading();
                } });
        }
        return;
    }
    const auto checkpoint = model_->memoryCheckpoint();
    if (!checkpoint)
        return;
    asking = true;
    const std::shared_ptr<TableModel> model = model_;
    const std::string size = memorySizeText(checkpoint->bytes);
    int choice;
    {
        NestedLoopGuard guard;
        choice = fl_choice("The %zu rows of %s read so far take about %s of memory.\n"
                           "Continue reading the file?\n\n"
                           "Stop Reading shows only the rows read so far, as if the file ended there.",
                           "Stop Reading", "Continue", nullptr, checkpoint->rows, escapeLabel(model->name()).c_str(),
                           size.c_str());
    }
    asking = false;
    if (choice == 1)
        model->continueReading();
    else
        model->stopReading();
}

void GuiTableView::syncWithModel()
{
    if (!model_)
    {
        search_ = {};
        rows(0);
        cols(0);
        redraw();
        return;
    }

    if (model_->state() == ModelState::Error && !error_reported_)
    {
        error_reported_ = true;
        spdlog::error("Failed to load {}: {}", model_->name(), model_->errorMessage());
        NestedLoopGuard guard;
        fl_alert("Failed to load %s:\n%s", escapeLabel(model_->name()).c_str(),
                 escapeLabel(model_->errorMessage()).c_str());
    }
    if (const auto view_error = model_->takeViewError())
    {
        spdlog::error("Failed to sort or filter {}: {}", model_->name(), *view_error);
        NestedLoopGuard guard;
        fl_alert("Could not sort or filter %s, so the sort and filter were cleared:\n%s",
                 escapeLabel(model_->name()).c_str(), escapeLabel(*view_error).c_str());
    }

    askToContinueReading();
    applyPendingView();
    const ViewSpec view = model_->view();
    const bool view_changed = view != view_;
    view_ = view;
    const int new_rows = static_cast<int>(std::min<size_t>(model_->displayRowCount(), MAX_ROWS));
    const int new_cols = static_cast<int>(std::min<size_t>(model_->columnCount(), INT_MAX));
    if (new_rows != rows() || new_cols != cols())
    {
        const bool shrunk = new_rows < rows() || new_cols < cols();
        rows(new_rows);
        cols(new_cols);
        // Fl_Table keeps the selection when it shrinks (e.g. after filtering),
        // which would then cover rows that no longer exist.
        if (shrunk)
            clampSelection();
        if (editingCell() && (edit_row_ >= rows() || edit_col_ >= cols()))
            finishEditing(false);
    }

    applyFont();
    const int digits = static_cast<int>(std::to_string(std::max(new_rows, 1)).size());
    row_number_width_ = static_cast<int>(fl_width("0")) * digits + 16;
    updateRowHeaderWidth();

    autosizeColumns();
    applyPendingScroll();
    applyPendingJump();
    search_ = model_->searchState();
    // The same coordinates may hold a different value after sorting/filtering.
    checkSelectionChange(view_changed);
    redraw();
    if (update_callback_)
        update_callback_();
}

void GuiTableView::clampSelection()
{
    if (rows() == 0 || cols() == 0)
    {
        current_row = select_row = current_col = select_col = -1;
        return;
    }
    if (current_row < 0 || select_row < 0 || current_col < 0 || select_col < 0)
        return;
    if (std::min(current_row, select_row) >= rows() || std::min(current_col, select_col) >= cols())
    {
        // Entirely gone: select the nearest cell instead.
        current_row = select_row = std::min(current_row, rows() - 1);
        current_col = select_col = std::min(current_col, cols() - 1);
        return;
    }
    current_row = std::min(current_row, rows() - 1);
    select_row = std::min(select_row, rows() - 1);
    current_col = std::min(current_col, cols() - 1);
    select_col = std::min(select_col, cols() - 1);
}

void GuiTableView::restoreScrollPosition(ScrollPosition position)
{
    pending_scroll_ = position;
    applyPendingScroll();
}

ViewSpec GuiTableView::restorableView() const
{
    if (pending_view_)
        return *pending_view_;
    return model_ ? model_->requestedView() : ViewSpec{};
}

void GuiTableView::restoreView(ViewSpec spec)
{
    if (spec.empty())
        return;
    pending_view_ = std::move(spec);
    applyPendingView();
}

void GuiTableView::applyPendingView()
{
    if (!pending_view_ || !model_)
        return;
    if (model_->state() == ModelState::Error)
    {
        pending_view_.reset();
        return;
    }
    const ViewCapabilities capabilities = model_->viewCapabilities();
    if ((pending_view_->sort.empty() || capabilities.sort) && (!pending_view_->filtered() || capabilities.filter))
    {
        model_->setView(std::move(*pending_view_));
        pending_view_.reset();
    }
}

void GuiTableView::applyPendingScroll()
{
    if (!pending_scroll_)
        return;
    // Column widths are not final until the first block has been sized.
    if (cols() > 0 && sized_columns_ == 0)
        return;

    const ScrollPosition target = *pending_scroll_;
    scrollTo(std::min(target.row, rows() - 1), std::min(target.col, cols() - 1));
    const bool cursor_ready = target.cursor_row < 0 || (target.cursor_row < rows() && target.cursor_col < cols());
    if (target.cursor_row >= 0 && target.cursor_col >= 0 && cursor_ready)
    {
        set_selection(target.cursor_row, target.cursor_col, target.cursor_row, target.cursor_col);
        checkSelectionChange();
    }

    const bool rows_final = model_->rowCount().isExact() || model_->state() == ModelState::Error;
    if (rows_final || (target.row < rows() && target.col < cols() && cursor_ready))
        pending_scroll_.reset();
}

void GuiTableView::applyPendingJump()
{
    if (!pending_jump_ || !model_)
        return;
    const ScrollPosition target = *pending_jump_;
    const bool rows_final = model_->rowCount().isExact() || model_->state() == ModelState::Error;
    if (target.row < rows() || rows_final)
    {
        // Clamped by jumpTo() if the sheet has fewer rows.
        pending_jump_.reset();
        jumpTo(target.row, target.col);
        return;
    }
    // Showing the last rows read makes the model read the next ones.
    if (rows() > 0)
        scrollTo(rows() - 1, target.col);
}

void GuiTableView::scrollTo(int row, int col)
{
    // Unlike row_position()/col_position(), this does not skip the update
    // when asked for the same row again after more rows were added.
    if (row >= 0)
        vscrollbar->Fl_Slider::value(std::min(static_cast<double>(row_scroll_position(row)), vscrollbar->maximum()));
    if (col >= 0)
        hscrollbar->Fl_Slider::value(std::min(static_cast<double>(col_scroll_position(col)), hscrollbar->maximum()));
    table_scrolled();
}

void GuiTableView::hideColumns(int first, int last)
{
    if (first < 0 || last < first || hidden_.hide(static_cast<size_t>(first), static_cast<size_t>(last),
                                                  static_cast<size_t>(cols())) == 0)
        return;
    for (int c = first; c <= last && c < cols(); ++c)
    {
        if (hidden_.contains(static_cast<size_t>(c)))
            collapseColumn(c);
    }
    updateRowHeaderWidth();
    // The cursor moves to the next visible column (or the previous one at the end).
    int row_top, col_left, row_bottom, col_right;
    get_selection(row_top, col_left, row_bottom, col_right);
    const int row = std::max(row_top, 0);
    const int col = static_cast<int>(hidden_.nearestVisible(static_cast<size_t>(first), 1, static_cast<size_t>(cols())));
    set_selection(row, col, row, col);
    checkSelectionChange(true);
    columnsChanged();
}

void GuiTableView::showColumn(int col)
{
    if (col < 0 || !hidden_.contains(static_cast<size_t>(col)))
        return;
    hidden_.show(static_cast<size_t>(col));
    expandColumn(col);
    updateRowHeaderWidth();
    columnsChanged();
}

void GuiTableView::showAllColumns()
{
    if (hidden_.empty())
        return;
    HiddenColumns shown;
    std::swap(shown, hidden_);
    for (int c = 0; c < cols(); ++c)
    {
        if (shown.contains(static_cast<size_t>(c)))
            expandColumn(c);
    }
    updateRowHeaderWidth();
    columnsChanged();
}

void GuiTableView::freezeColumns(int count)
{
    count = std::max(count, 0);
    if (count == frozen_)
        return;
    const int old = frozen_;
    frozen_ = count;
    for (int c = 0; c < std::max(old, count) && c < cols(); ++c)
    {
        if (c < count)
            collapseColumn(c);
        else
            expandColumn(c);
    }
    updateRowHeaderWidth();
    columnsChanged();
}

// Columns are sized while rows arrive; a collapsed one keeps the width for later.
void GuiTableView::setColumnWidth(int col, int width)
{
    if (collapsed(col))
    {
        saved_widths_[col] = width;
        col_width(col, 0);
    }
    else
        col_width(col, width);
}

void GuiTableView::collapseColumn(int col)
{
    if (col_width(col) > 0)
    {
        saved_widths_[col] = col_width(col);
        col_width(col, 0);
    }
}

// Back in the scrolled area, unless it is still hidden or frozen.
void GuiTableView::expandColumn(int col)
{
    if (collapsed(col))
        return;
    if (const auto it = saved_widths_.find(col); it != saved_widths_.end())
    {
        if (col < cols())
            col_width(col, it->second);
        saved_widths_.erase(it);
    }
}

void GuiTableView::updateRowHeaderWidth()
{
    int width = row_number_width_;
    for (int c = 0; c < frozen_ && c < cols(); ++c)
    {
        if (hidden_.contains(static_cast<size_t>(c)))
            continue;
        if (const auto it = saved_widths_.find(c); it != saved_widths_.end())
            width += it->second;
    }
    if (width != row_header_width())
        row_header_width(width);
}

// The frozen column drawn at window x, or -1; `right` is its right edge.
int GuiTableView::frozenColumnAt(int x, int *right)
{
    int left = wix + row_number_width_;
    for (int c = 0; c < frozen_ && c < cols(); ++c)
    {
        const auto it = saved_widths_.find(c);
        if (hidden_.contains(static_cast<size_t>(c)) || it == saved_widths_.end())
            continue;
        if (x >= left && x < left + it->second)
        {
            if (right)
                *right = left + it->second;
            return c;
        }
        left += it->second;
    }
    return -1;
}

// Clicks and drags on frozen cells and their headers, which Fl_Table sees
// as its row header. Returns 1 if handled.
int GuiTableView::handleFrozen(int event)
{
    const int ex = Fl::event_x();
    const int ey = Fl::event_y();
    int R = -1, C = -1;
    ResizeFlag resize_flag;
    if (event == FL_PUSH)
    {
        int right = 0;
        const int col = frozenColumnAt(ex, &right);
        if (col < 0 || ey < wiy)
            return 0;
        pending_scroll_.reset();
        pending_jump_.reset();
        if (ey < tiy)
        {
            // The header of a frozen column.
            if (Fl::event_button() == FL_RIGHT_MOUSE)
                showSortMenu(col);
            else if (ex >= right - SORT_ICON_WIDTH)
                cycleSort(col);
            else if (rows() > 0)
            {
                set_selection(0, col, rows() - 1, col);
                take_focus();
                checkSelectionChange();
            }
            redraw();
            return 1;
        }
        if (cursor2rowcol(R, C, resize_flag) != CONTEXT_ROW_HEADER || R < 0)
            return 0;
        if (Fl::event_button() == FL_RIGHT_MOUSE)
        {
            showCellMenu(R, col, CONTEXT_CELL);
            redraw();
            return 1;
        }
        if (Fl::event_state(FL_SHIFT) && current_row >= 0 && current_col >= 0)
            set_selection(current_row, current_col, R, col);
        else
            set_selection(R, col, R, col);
        take_focus();
        frozen_drag_ = true;
        checkSelectionChange();
        redraw();
        if (Fl::event_clicks())
            cellDoubleClicked(R, col);
        return 1;
    }
    if (event == FL_DRAG && frozen_drag_)
    {
        const TableContext context = cursor2rowcol(R, C, resize_flag);
        int col = -1;
        if (context == CONTEXT_ROW_HEADER)
            col = frozenColumnAt(ex);
        else if (context == CONTEXT_CELL)
            col = C;
        if (R >= 0 && col >= 0 && current_row >= 0 && current_col >= 0)
        {
            set_selection(current_row, current_col, R, col);
            checkSelectionChange();
            redraw();
        }
        return 1;
    }
    if (event == FL_RELEASE && frozen_drag_)
    {
        frozen_drag_ = false;
        return 1;
    }
    return 0;
}

void GuiTableView::restoreHiddenColumns(const HiddenColumns &hidden)
{
    // Applied by autosizeColumns(), as the columns are sized.
    hidden_ = hidden;
    columnsChanged();
}

void GuiTableView::columnsChanged()
{
    redraw();
    if (columns_changed_callback_)
        columns_changed_callback_();
}

// After a key moved the selection onto a hidden column.
void GuiTableView::skipHiddenColumn(int direction)
{
    if (select_col < 0 || !hidden_.contains(static_cast<size_t>(select_col)))
        return;
    const int col = static_cast<int>(hidden_.nearestVisible(static_cast<size_t>(select_col), direction,
                                                             static_cast<size_t>(cols())));
    // Without Shift, the cursor moved with the selection's end.
    if (current_col == select_col)
        current_col = col;
    select_col = col;
    if (col >= frozen_ && (col < leftcol + 1 || col > rightcol - 1))
        col_position(col);
    redraw();
}

void GuiTableView::jumpTo(int row, int col)
{
    // The awake update announcing these rows may not have run yet.
    if (row >= rows() || col >= cols())
        syncWithModel();
    showColumn(col);
    if (row < 0 || col < 0 || col >= cols() || rows() == 0)
        return;
    if (row >= rows())
    {
        if (model_->rowCount().isExact() || model_->state() == ModelState::Error)
            row = rows() - 1;
        else
        {
            pending_scroll_.reset();
            pending_jump_ = ScrollPosition{row, col};
            applyPendingJump();
            return;
        }
    }
    pending_scroll_.reset();
    pending_jump_.reset();
    set_selection(row, col, row, col);
    // Like Fl_Table::move_cursor(), partly visible edge rows/columns are
    // scrolled to as well.
    const bool row_visible = row > toprow && row < botrow;
    const bool col_visible = col < frozen_ || (col > leftcol && col < rightcol);
    if (!row_visible || !col_visible)
        scrollTo(row_visible ? -1 : row, col_visible ? -1 : col);
    checkSelectionChange();
    redraw();
}

void GuiTableView::autosizeColumns()
{
    const size_t column_count = static_cast<size_t>(cols());
    if (sized_columns_ >= column_count)
        return;
    // Wait for the first block so that widths reflect real data.
    if (model_->peek(0, 0).state == CellState::Loading)
        return;

    applyFont();
    const ColumnWidthMetrics metrics{
        .measure = [](const std::string &text)
        { return static_cast<int>(fl_width(text.c_str())); },
        .header_padding = HEADER_PADDING,
        .cell_padding = 12,
        .min_width = MIN_COLUMN_WIDTH,
        .max_width = MAX_COLUMN_WIDTH,
        .sample_rows = AUTOSIZE_SAMPLE_ROWS,
    };
    for (size_t c = sized_columns_; c < column_count; ++c)
        setColumnWidth(static_cast<int>(c), fittingColumnWidth(*model_, c, metrics));
    sized_columns_ = column_count;
    updateRowHeaderWidth();
}

void GuiTableView::applyFont()
{
    fl_font(use_monospace_font_ ? FL_COURIER : FL_HELVETICA, FONT_SIZE);
}

void GuiTableView::set_use_monospace_font(bool use)
{
    use_monospace_font_ = use;
    redraw();
}

int GuiTableView::handle(int event)
{
    if (event == FL_PASTE)
    {
        if (model_ && model_->editMode())
            pasteText(std::string(Fl::event_text(), static_cast<size_t>(Fl::event_length())));
        return 1;
    }
    if (event == FL_KEYBOARD && !editingCell() && handleEditKey())
        return 1;
    if (frozen_ > 0 && model_ && (event == FL_PUSH || event == FL_DRAG || event == FL_RELEASE) && handleFrozen(event))
        return 1;
    if ((event == FL_PUSH || event == FL_RELEASE) && model_)
    {
        int R, C;
        ResizeFlag resize_flag;
        const TableContext context = cursor2rowcol(R, C, resize_flag);
        const bool on_header = context == CONTEXT_COL_HEADER && C >= 0 && resize_flag == RESIZE_NONE;
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && on_header)
        {
            showSortMenu(C);
            return 1;
        }
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && resize_flag == RESIZE_NONE &&
            ((context == CONTEXT_CELL && R >= 0 && C >= 0) || (context == CONTEXT_ROW_HEADER && R >= 0)))
        {
            pending_scroll_.reset();
            pending_jump_.reset();
            showCellMenu(R, C, context);
            return 1;
        }
        // Only the sort icon sorts (press and release on one icon); the
        // rest of the header selects the column as usual.
        if (event == FL_PUSH)
        {
            header_push_col_ = on_header && Fl::event_button() == FL_LEFT_MOUSE && onSortIcon(C) ? C : -1;
            if (header_push_col_ >= 0)
                return 1;
        }
        else if (header_push_col_ >= 0)
        {
            if (on_header && C == header_push_col_ && onSortIcon(C))
                cycleSort(C);
            header_push_col_ = -1;
            return 1;
        }
    }
    // A drag that started on a sort icon must not select.
    if (event == FL_DRAG && header_push_col_ >= 0)
        return 1;

    // Once the user scrolls or clicks, a pending restore must not move the view.
    // (toprow alone cannot tell: it also changes while rows are still arriving.)
    switch (event)
    {
    case FL_PUSH:
    case FL_MOUSEWHEEL:
    case FL_KEYBOARD:
        pending_scroll_.reset();
        pending_jump_.reset();
        break;
    default:
        break;
    }
    // Moving into a frozen column must not scroll the others back to the start.
    const double hscroll = hscrollbar->value();
    const int handled = Fl_Table::handle(event);
    // The border left of a column next to a collapsed one resizes the collapsed one.
    if ((event == FL_DRAG || event == FL_RELEASE) && !saved_widths_.empty())
    {
        for (const auto &entry : saved_widths_)
        {
            if (entry.first < cols() && collapsed(entry.first) && col_width(entry.first) != 0)
                col_width(entry.first, 0);
        }
    }
    if (event == FL_KEYBOARD && frozen_ > 0 && select_col >= 0 && select_col < frozen_ && hscrollbar->value() != hscroll)
    {
        hscrollbar->Fl_Slider::value(hscroll);
        table_scrolled();
    }
    if (event == FL_KEYBOARD && !hidden_.empty())
    {
        const int key = Fl::event_key();
        const bool back = key == FL_Left || key == FL_End || (key == FL_Tab && Fl::event_state(FL_SHIFT));
        skipHiddenColumn(back ? -1 : 1);
    }
    // Frozen cells are drawn with the row header, which Fl_Table redraws
    // only with the whole table.
    if (frozen_ > 0 && (event == FL_PUSH || event == FL_DRAG || event == FL_RELEASE || event == FL_KEYBOARD))
        redraw();
    checkSelectionChange();
    return handled;
}

void GuiTableView::draw()
{
    // Picks up theme changes made while this view existed.
    styleScrollbar(vscrollbar);
    styleScrollbar(hscrollbar);
    Fl_Table::draw();
    if (frozen_ <= 0 || !model_)
        return;
    // The headers of the frozen columns, in the corner above the row header.
    const int header_height = col_header() ? col_header_height() : 0;
    applyFont();
    fl_push_clip(wix, wiy, row_header_width(), wih);
    fl_rectf(wix, wiy, row_number_width_, header_height, color());
    int x = wix + row_number_width_;
    for (int c = 0; c < frozen_ && c < cols(); ++c)
    {
        const auto it = saved_widths_.find(c);
        if (hidden_.contains(static_cast<size_t>(c)) || it == saved_widths_.end())
            continue;
        if (header_height > 0)
            drawHeaderCell(model_->columnName(static_cast<size_t>(c)).c_str(), x, wiy, it->second, header_height,
                           col_header_color(), sortIndicator(c), filtersColumn(view_, static_cast<size_t>(c)));
        x += it->second;
    }
    // The edge of the frozen columns.
    fl_color(themeColors().header_line);
    fl_rectf(wix + row_header_width() - 2, wiy, 2, tiy - wiy + std::min(tih, table_h));
    fl_pop_clip();
}

SortIndicator GuiTableView::sortIndicator(int col) const
{
    const auto &keys = view_.sort;
    if (keys.empty() || keys.front().column != static_cast<size_t>(col))
        return SortIndicator::Unsorted;
    return keys.front().ascending ? SortIndicator::Ascending : SortIndicator::Descending;
}

void GuiTableView::draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H)
{
    switch (context)
    {
    case CONTEXT_STARTPAGE:
        applyFont();
        if (editingCell())
            placeEditor();
        if (model_ && rows() > 0)
            model_->requestRange(static_cast<size_t>(std::max(toprow, 0)), static_cast<size_t>(std::max(botrow, 0)));
        return;

    case CONTEXT_ROW_HEADER:
        if (frozen_ > 0 && model_)
        {
            // Row number, then the frozen cells.
            drawHeaderCell(std::to_string(R + 1).c_str(), X, Y, row_number_width_, H, row_header_color());
            int x = X + row_number_width_;
            for (int c = 0; c < frozen_ && c < cols(); ++c)
            {
                const auto it = saved_widths_.find(c);
                if (hidden_.contains(static_cast<size_t>(c)) || it == saved_widths_.end())
                    continue;
                drawDataCell(R, c, x, Y, it->second, H);
                x += it->second;
            }
            return;
        }
        [[fallthrough]];
    case CONTEXT_COL_HEADER:
    {
        if (W <= 0)
            return; // a hidden column
        const std::string label = !model_ ? std::string()
                                  : context == CONTEXT_ROW_HEADER
                                      ? std::to_string(R + 1)
                                      : model_->columnName(static_cast<size_t>(C));
        SortIndicator sort = SortIndicator::None;
        bool filtered = false;
        if (context == CONTEXT_COL_HEADER)
        {
            sort = sortIndicator(C);
            filtered = filtersColumn(view_, static_cast<size_t>(C));
        }
        drawHeaderCell(label.c_str(), X, Y, W, H, row_header_color(), sort, filtered);
        if (context == CONTEXT_COL_HEADER && !hidden_.empty())
        {
            // A mark where columns are hidden, as spreadsheets draw one.
            fl_color(FL_SELECTION_COLOR);
            if (C > 0 && hidden_.contains(static_cast<size_t>(C - 1)))
                fl_rectf(X, Y, 3, H);
            if (C + 1 < cols() && hidden_.contains(static_cast<size_t>(C + 1)))
                fl_rectf(X + W - 3, Y, 3, H);
        }
        return;
    }

    case CONTEXT_CELL:
        // The editor shows the cell being edited.
        if (W > 0 && !(editingCell() && R == edit_row_ && C == edit_col_)) // not hidden or frozen
            drawDataCell(R, C, X, Y, W, H);
        return;

    default:
        return;
    }
}

void GuiTableView::drawDataCell(int R, int C, int X, int Y, int W, int H)
{
    const CellRef cell = model_ ? model_->peek(static_cast<size_t>(R), static_cast<size_t>(C)) : CellRef{CellState::OutOfRange};
    const ThemeColors &colors = themeColors();
    fl_push_clip(X, Y, W, H);
    const bool hit = searchHitIndex(search_, {static_cast<size_t>(R), static_cast<size_t>(C)}).has_value();
    const bool modified = model_ && model_->cellModified(static_cast<size_t>(R), static_cast<size_t>(C));
    fl_color(is_selected(R, C) ? colors.cell_selection
             : hit             ? colors.cell_search_hit
             : modified        ? colors.cell_modified
                               : colors.cell_background);
    fl_rectf(X, Y, W, H);

    switch (cell.state)
    {
    case CellState::Ready:
        fl_color(colors.cell_text);
        // draw_symbols = 0: FLTK would otherwise treat "@..." (e.g. in e-mail
        // addresses) as a symbol name and drop it from the text.
        fl_draw(cellDisplayText(*cell.value).c_str(), X + 4, Y, W - 8, H,
                (isNumericCell(*cell.value) ? FL_ALIGN_RIGHT : FL_ALIGN_LEFT) | FL_ALIGN_CLIP, nullptr, 0);
        break;
    case CellState::Loading:
        fl_color(colors.cell_muted_text);
        fl_draw("…", X + 4, Y, W - 8, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
        break;
    case CellState::Error:
        fl_color(FL_RED);
        fl_draw("!", X + 4, Y, W - 8, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
        break;
    case CellState::Empty:
    case CellState::OutOfRange:
        break;
    }

    fl_color(colors.grid_line);
    fl_xyline(X, Y + H - 1, X + W - 1);
    fl_yxline(X + W - 1, Y, Y + H - 1);
    fl_pop_clip();
}

bool GuiTableView::onSortIcon(int col)
{
    int X, Y, W, H;
    if (find_cell(CONTEXT_COL_HEADER, 0, col, X, Y, W, H) != 0)
        return false;
    return Fl::event_x() >= X + W - SORT_ICON_WIDTH && Fl::event_x() < X + W;
}

void GuiTableView::cycleSort(int col)
{
    if (!model_->viewCapabilities().sort)
    {
        // The header menu explains why.
        fl_beep();
        return;
    }
    ViewSpec spec = model_->requestedView();
    spec.sort = nextSortCycle(spec.sort, static_cast<size_t>(col));
    model_->setView(std::move(spec));
}

// Sorts by one column at a time; "No Sort" restores the file order.
void GuiTableView::showSortMenu(int col)
{
    const size_t column = static_cast<size_t>(col);
    ViewSpec spec = model_->requestedView();
    const bool sorted_here = !spec.sort.empty() && spec.sort.front().column == column;
    const ViewCapabilities capabilities = model_->viewCapabilities();
    const int inactive = capabilities.sort ? 0 : FL_MENU_INACTIVE;

    std::vector<Fl_Menu_Item> items;
    if (inactive)
        items.push_back(menuItem("Still opening (cannot sort yet)", MenuAction::None,
                                 FL_MENU_INACTIVE | FL_MENU_DIVIDER));
    items.push_back(menuItem("Sort Ascending", MenuAction::SortAscending,
                             FL_MENU_RADIO | inactive | (sorted_here && spec.sort.front().ascending ? FL_MENU_VALUE : 0)));
    items.push_back(menuItem("Sort Descending", MenuAction::SortDescending,
                             FL_MENU_RADIO | inactive | (sorted_here && !spec.sort.front().ascending ? FL_MENU_VALUE : 0)));
    items.push_back(menuItem("No Sort", MenuAction::NoSort,
                             FL_MENU_RADIO | inactive | (spec.sort.empty() ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER));
    items.push_back(menuItem("Filter by This Column…", MenuAction::FilterColumn,
                             capabilities.filter && filter_column_callback_ ? 0 : FL_MENU_INACTIVE));
    items.push_back(menuItem("Clear Filter", MenuAction::ClearFilter,
                             (spec.filtered() ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER));
    items.push_back(menuItem("Column Statistics", MenuAction::ColumnStats, stats_column_callback_ ? 0 : FL_MENU_INACTIVE));
    items.push_back(menuItem("Histogram", MenuAction::Histogram, histogram_column_callback_ ? 0 : FL_MENU_INACTIVE));
    items.push_back(menuItem("Scatter Plot", MenuAction::ScatterPlot,
                             (scatter_column_callback_ ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER));
    // The selected columns, if this is one of them; else this column.
    int row_top, col_left, row_bottom, col_right;
    get_selection(row_top, col_left, row_bottom, col_right);
    if (col < col_left || col > col_right)
        col_left = col_right = col;
    const bool several = col_right > col_left;
    const bool all = hidden_.hidesAll(static_cast<size_t>(col_left), static_cast<size_t>(col_right),
                                      static_cast<size_t>(cols()));
    items.push_back(menuItem(several ? "Hide Columns" : "Hide Column", MenuAction::HideColumns, all ? FL_MENU_INACTIVE : 0));
    items.push_back(menuItem("Show All Columns", MenuAction::ShowAllColumns,
                             (hidden_.empty() ? FL_MENU_INACTIVE : 0) | FL_MENU_DIVIDER));
    // At least one column must be left to scroll.
    items.push_back(menuItem("Freeze Columns up to Here", MenuAction::FreezeColumns,
                             col + 1 < cols() && col + 1 != frozen_ ? 0 : FL_MENU_INACTIVE));
    items.push_back(menuItem("Unfreeze Columns", MenuAction::UnfreezeColumns, frozen_ > 0 ? 0 : FL_MENU_INACTIVE));
    items.push_back({nullptr});

    const std::shared_ptr<TableModel> model = model_;
    MenuAction action;
    {
        NestedLoopGuard guard;
        action = menuAction(items.front().popup(Fl::event_x(), Fl::event_y()));
    }
    // A new SQL result may have replaced the table meanwhile: its columns
    // are not the ones the menu was for.
    if (model_ != model)
        return;
    // Nor keep a view that changed meanwhile (e.g. from the Filter pane).
    spec = model_->requestedView();
    switch (action)
    {
    case MenuAction::ColumnStats:
        stats_column_callback_(column);
        return;
    case MenuAction::Histogram:
        histogram_column_callback_(column);
        return;
    case MenuAction::ScatterPlot:
        scatter_column_callback_(column);
        return;
    case MenuAction::HideColumns:
        hideColumns(col_left, col_right);
        return;
    case MenuAction::ShowAllColumns:
        showAllColumns();
        return;
    case MenuAction::FreezeColumns:
        freezeColumns(col + 1);
        return;
    case MenuAction::UnfreezeColumns:
        freezeColumns(0);
        return;
    case MenuAction::SortAscending:
        spec.sort = {{column, true}};
        break;
    case MenuAction::SortDescending:
        spec.sort = {{column, false}};
        break;
    case MenuAction::NoSort:
        spec.sort.clear();
        break;
    case MenuAction::FilterColumn:
        filter_column_callback_(column);
        return;
    case MenuAction::ClearFilter:
        setFilter(std::nullopt);
        return;
    default:
        return;
    }
    model_->setView(std::move(spec));
}

// Right-clicking outside the selection selects the clicked cell (or row)
// first, as spreadsheets do, so that the menu acts on what was clicked.
void GuiTableView::showCellMenu(int row, int col, TableContext context)
{
    const bool row_header = context == CONTEXT_ROW_HEADER;
    if (!is_selected(row, row_header ? 0 : col))
    {
        if (row_header)
            set_selection(row, 0, row, std::max(cols() - 1, 0));
        else
            set_selection(row, col, row, col);
        take_focus();
        checkSelectionChange();
    }

    const ViewCapabilities capabilities = model_->viewCapabilities();
    const ViewSpec spec = model_->requestedView();
    // Quick filters on the clicked cell; a ragged row's missing cell is empty.
    std::optional<CellValue> value;
    if (!row_header)
    {
        const CellRef cell = model_->peek(static_cast<size_t>(row), static_cast<size_t>(col));
        if (cell.state == CellState::Ready)
            value = *cell.value;
        else if (cell.state == CellState::Empty)
            value = CellValue{};
    }
    const size_t column = static_cast<size_t>(std::max(col, 0));
    const std::optional<FilterCondition> equal =
        value ? std::optional(cellFilterCondition(column, *value, true)) : std::nullopt;
    const std::optional<FilterCondition> not_equal =
        value ? std::optional(cellFilterCondition(column, *value, false)) : std::nullopt;
    const std::string equal_label = equal ? quickFilterLabel(*equal) : "Filter: = …";
    const std::string not_equal_label = not_equal ? quickFilterLabel(*not_equal) : "Filter: ≠ …";

    std::vector<Fl_Menu_Item> items;
    items.push_back(menuItem("Copy", MenuAction::Copy, FL_MENU_DIVIDER, FL_COMMAND + 'c'));
    if (!row_header)
    {
        if (!capabilities.filter)
            items.push_back(menuItem("Still opening (cannot filter yet)", MenuAction::None, FL_MENU_INACTIVE));
        const int quick = capabilities.filter && value ? 0 : FL_MENU_INACTIVE;
        items.push_back(menuItem(equal_label.c_str(), MenuAction::FilterEqual, quick));
        items.push_back(menuItem(not_equal_label.c_str(), MenuAction::FilterNotEqual, quick));
        items.push_back(menuItem("Filter by This Column…", MenuAction::FilterColumn,
                                 capabilities.filter && filter_column_callback_ ? 0 : FL_MENU_INACTIVE));
    }
    items.push_back(menuItem("Clear Filter", MenuAction::ClearFilter, spec.filtered() ? 0 : FL_MENU_INACTIVE));
    if (!row_header)
    {
        items.back().flags |= FL_MENU_DIVIDER;
        items.push_back(menuItem("Column Statistics", MenuAction::ColumnStats, stats_column_callback_ ? 0 : FL_MENU_INACTIVE));
        items.push_back(menuItem("Histogram", MenuAction::Histogram, histogram_column_callback_ ? 0 : FL_MENU_INACTIVE));
        items.push_back(menuItem("Scatter Plot", MenuAction::ScatterPlot, scatter_column_callback_ ? 0 : FL_MENU_INACTIVE));
    }
    items.push_back({nullptr});

    const std::shared_ptr<TableModel> model = model_;
    MenuAction action;
    {
        NestedLoopGuard guard;
        action = menuAction(items.front().popup(Fl::event_x(), Fl::event_y()));
    }
    // As in showSortMenu(): the menu was for this table only.
    if (model_ != model)
        return;
    switch (action)
    {
    case MenuAction::ColumnStats:
        stats_column_callback_(column);
        break;
    case MenuAction::Histogram:
        histogram_column_callback_(column);
        break;
    case MenuAction::ScatterPlot:
        scatter_column_callback_(column);
        break;
    case MenuAction::Copy:
        copySelection();
        break;
    case MenuAction::FilterEqual:
    case MenuAction::FilterNotEqual:
    {
        ViewSpec next = model_->requestedView();
        addFilterCondition(next, action == MenuAction::FilterEqual ? *equal : *not_equal);
        model_->setView(std::move(next));
        break;
    }
    case MenuAction::FilterColumn:
        filter_column_callback_(column);
        break;
    case MenuAction::ClearFilter:
        setFilter(std::nullopt);
        break;
    default:
        break;
    }
}

void GuiTableView::setFilter(std::optional<Filter> filter)
{
    ViewSpec spec = model_->requestedView();
    spec.filter = std::move(filter);
    model_->setView(std::move(spec));
}

void GuiTableView::copySelection()
{
    const TsvCopyResult result = selectedCellsToTsv();
    if (result.copied_rows > 0)
        Fl::copy(result.text.c_str(), result.text.length(), 2);
    if (result.skipped_rows > 0)
    {
        NestedLoopGuard guard;
        fl_message("Some selected data is not available yet.\n"
                   "Copied %zu row(s); skipped %zu row(s) containing unavailable cells.%s",
                   result.copied_rows, result.skipped_rows,
                   result.copied_rows == 0 ? "\nThe clipboard was not changed." : "");
    }
}

TsvCopyResult GuiTableView::selectedCellsToTsv()
{
    int row_top, row_bottom, col_left, col_right;
    this->get_selection(row_top, col_left, row_bottom, col_right);
    spdlog::debug("Selected cell range: ({}, {}) to ({}, {})", row_top, col_left, row_bottom, col_right);
    if (!model_ || row_top < 0 || col_left < 0)
        return {};
    return rangeToTsv(*model_, static_cast<size_t>(row_top), static_cast<size_t>(col_left),
                      static_cast<size_t>(row_bottom), static_cast<size_t>(col_right), &hidden_);
}

GuiTableView::SelectedCell GuiTableView::topLeftSelectedCell()
{
    int row_top, row_bottom, col_left, col_right;
    get_selection(row_top, col_left, row_bottom, col_right);
    if (!model_ || row_top < 0 || col_left < 0)
        return {};
    // E.g. Select All starting with a hidden column.
    col_left = static_cast<int>(hidden_.nearestVisible(static_cast<size_t>(col_left), 1, static_cast<size_t>(cols())));
    const CellRef cell = model_->peek(static_cast<size_t>(row_top), static_cast<size_t>(col_left));
    return {row_top, col_left, cell.state};
}

std::string GuiTableView::topLeftSelectedCellText()
{
    const SelectedCell selected = topLeftSelectedCell();
    if (selected.state != CellState::Ready)
        return {};
    const CellRef cell = model_->peek(static_cast<size_t>(selected.row), static_cast<size_t>(selected.col));
    return cell.state == CellState::Ready ? formatCell(*cell.value) : std::string();
}

void GuiTableView::checkSelectionChange(bool force)
{
    const SelectedCell selected = topLeftSelectedCell();
    if (!force && selected == reported_selection_)
        return;
    reported_selection_ = selected;
    if (selection_callback_)
        selection_callback_();
}

// ===========================================================================
// Editing
// ===========================================================================

void GuiTableView::cellDoubleClicked(int row, int col)
{
    if (model_ && model_->cellEditable(static_cast<size_t>(row), static_cast<size_t>(col)))
        editCell(row, col);
    else
        activateCell();
}

bool GuiTableView::editingCell() const
{
    return editor_ && editor_->visible();
}

// F2, Enter and typing edit the cell under the cursor; Delete and Backspace
// clear the selection. Outside edit mode, keys do what they did.
bool GuiTableView::handleEditKey()
{
    if (!model_ || !model_->editMode() || current_row < 0 || current_col < 0 ||
        Fl::event_state(FL_CTRL | FL_ALT | FL_META))
        return false;
    const int key = Fl::event_key();
    const auto row = static_cast<size_t>(current_row), col = static_cast<size_t>(current_col);
    if (key == FL_F + 2 || key == FL_Enter || key == FL_KP_Enter)
    {
        if (model_->cellEditable(row, col))
            editCell(current_row, current_col);
        else
            fl_beep();
        return true;
    }
    if (key == FL_Delete || key == FL_BackSpace)
    {
        clearSelectedCells();
        return true;
    }
    const char *text = Fl::event_text();
    if (Fl::event_length() > 0 && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7F)
    {
        if (model_->cellEditable(row, col))
            editCell(current_row, current_col, std::string(text, static_cast<size_t>(Fl::event_length())));
        else
            fl_beep();
        return true;
    }
    return false;
}

void GuiTableView::editCell(int row, int col, const std::optional<std::string> &typed)
{
    finishEditing(true);
    if (!model_ || row < 0 || col < 0 || !model_->cellEditable(static_cast<size_t>(row), static_cast<size_t>(col)))
    {
        fl_beep();
        return;
    }
    const CellRef cell = model_->peek(static_cast<size_t>(row), static_cast<size_t>(col));
    const std::string value = cell.state == CellState::Ready ? formatCell(*cell.value) : std::string();
    selectCell(row, col);
    if (col < frozen_)
    {
        // Frozen cells are drawn left of the scrolled cells, where the editor
        // would be cut off.
        const char *input;
        {
            NestedLoopGuard guard;
            input = fl_input("%s, row %d:", typed ? typed->c_str() : value.c_str(),
                             escapeLabel(model_->columnName(static_cast<size_t>(col))).c_str(), row + 1);
        }
        if (input && model_)
            model_->setCells({{static_cast<size_t>(row), static_cast<size_t>(col), input}});
        Fl::focus(this);
        return;
    }
    edit_row_ = row;
    edit_col_ = col;
    editor_->textfont(use_monospace_font_ ? FL_COURIER : FL_HELVETICA);
    editor_->textsize(FONT_SIZE);
    editor_->value(typed ? typed->c_str() : value.c_str());
    placeEditor();
    editor_->show();
    editor_->take_focus();
    editor_->insert_position(editor_->size());
    redraw();
}

void GuiTableView::placeEditor()
{
    int X, Y, W, H;
    if (find_cell(CONTEXT_CELL, edit_row_, edit_col_, X, Y, W, H) != 0)
        return;
    if (X != editor_->x() || Y != editor_->y() || W != editor_->w() || H != editor_->h())
    {
        editor_->resize(X, Y, W, H);
        editor_->redraw();
    }
}

void GuiTableView::finishEditing(bool commit, int rows_down, int cols_right)
{
    if (!editingCell())
        return;
    const std::string text = editor_->value();
    const int row = edit_row_, col = edit_col_;
    const bool had_focus = Fl::focus() == editor_;
    // Hidden first: hiding takes the focus away, which finishes again.
    editor_->hide();
    edit_row_ = edit_col_ = -1;
    if (commit && model_)
        model_->setCells({{static_cast<size_t>(row), static_cast<size_t>(col), text}});
    // Not take_focus(): as a group, the table would pass the focus on to a child.
    if (had_focus)
        Fl::focus(this);
    if ((rows_down != 0 || cols_right != 0) && rows() > 0 && cols() > 0)
    {
        const int next_row = std::clamp(row + rows_down, 0, rows() - 1);
        int next_col = std::clamp(col + cols_right, 0, cols() - 1);
        next_col = static_cast<int>(hidden_.nearestVisible(static_cast<size_t>(next_col), cols_right < 0 ? -1 : 1,
                                                           static_cast<size_t>(cols())));
        selectCell(next_row, next_col);
    }
    checkSelectionChange(true);
    redraw();
}

void GuiTableView::selectCell(int row, int col)
{
    set_selection(row, col, row, col);
    const bool row_visible = row > toprow && row < botrow;
    const bool col_visible = col < frozen_ || (col > leftcol && col < rightcol);
    if (!row_visible || !col_visible)
        scrollTo(row_visible ? -1 : row, col_visible ? -1 : col);
    checkSelectionChange();
    redraw();
}

void GuiTableView::clearSelectedCells()
{
    finishEditing(true);
    if (!model_ || !model_->editMode())
        return;
    int row_top, col_left, row_bottom, col_right;
    get_selection(row_top, col_left, row_bottom, col_right);
    if (row_top < 0 || col_left < 0)
        return;
    const size_t columns = hidden_.visibleIn(static_cast<size_t>(col_left), static_cast<size_t>(col_right));
    if (static_cast<size_t>(row_bottom - row_top + 1) * columns > MAX_CLEARED_CELLS)
    {
        NestedLoopGuard guard;
        fl_alert("Too many cells to clear at once.");
        return;
    }
    std::vector<TableModel::CellText> cells;
    for (int r = row_top; r <= row_bottom; ++r)
    {
        for (int c = col_left; c <= col_right; ++c)
        {
            if (!hidden_.contains(static_cast<size_t>(c)))
                cells.push_back({static_cast<size_t>(r), static_cast<size_t>(c), {}});
        }
    }
    model_->setCells(cells);
    checkSelectionChange(true);
    redraw();
}

void GuiTableView::paste()
{
    finishEditing(true);
    if (!model_ || !model_->editMode())
    {
        fl_beep();
        return;
    }
    // The text arrives as FL_PASTE.
    Fl::paste(*this, 1);
}

void GuiTableView::pasteText(const std::string &text)
{
    int row_top, col_left, row_bottom, col_right;
    get_selection(row_top, col_left, row_bottom, col_right);
    // An empty clipboard clears nothing.
    if (row_top < 0 || col_left < 0 || text.empty())
    {
        fl_beep();
        return;
    }
    const std::vector<TableModel::CellText> cells =
        pastedCells(parseTsv(text), static_cast<size_t>(row_top), static_cast<size_t>(col_left),
                    static_cast<size_t>(row_bottom), static_cast<size_t>(col_right), static_cast<size_t>(rows()),
                    static_cast<size_t>(cols()), &hidden_);
    if (cells.empty() || model_->setCells(cells) == 0)
    {
        fl_beep();
        return;
    }
    // The cells pasted into are selected.
    size_t last_row = 0, last_col = 0;
    for (const auto &cell : cells)
    {
        last_row = std::max(last_row, cell.row);
        last_col = std::max(last_col, cell.column);
    }
    set_selection(row_top, col_left, static_cast<int>(last_row), static_cast<int>(last_col));
    checkSelectionChange(true);
    redraw();
}

void GuiTableView::undo()
{
    finishEditing(true);
    CellPosition cell{SIZE_MAX, SIZE_MAX};
    if (!model_ || !model_->undo(&cell))
    {
        fl_beep();
        return;
    }
    if (cell.row < static_cast<size_t>(rows()) && cell.col < static_cast<size_t>(cols()))
        selectCell(static_cast<int>(cell.row), static_cast<int>(cell.col));
    checkSelectionChange(true);
    redraw();
}

void GuiTableView::redo()
{
    finishEditing(true);
    CellPosition cell{SIZE_MAX, SIZE_MAX};
    if (!model_ || !model_->redo(&cell))
    {
        fl_beep();
        return;
    }
    if (cell.row < static_cast<size_t>(rows()) && cell.col < static_cast<size_t>(cols()))
        selectCell(static_cast<int>(cell.row), static_cast<int>(cell.col));
    checkSelectionChange(true);
    redraw();
}
