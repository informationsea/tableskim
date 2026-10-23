#include "tui_table_view.hxx"

#include <algorithm>
#include <climits>

#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/dom/node.hpp>

#include "cell_format.hxx"
#include "tui_text.hxx"

using namespace ftxui;

namespace
{
    const int DEFAULT_COLUMN_WIDTH = 10;
    // Widths include one space on each side of the text.
    const int CELL_PADDING = 2;
    const int MIN_COLUMN_WIDTH = 3;
    const int MAX_AUTO_COLUMN_WIDTH = 42;
    const int MAX_COLUMN_WIDTH = 400;
    // " ▲" after a sorted column's name.
    const int SORT_INDICATOR_WIDTH = 2;
    // " ▿" after a filtered column's name; columns are not widened for it.
    const int FILTER_INDICATOR_WIDTH = 2;
    const size_t WHEEL_ROWS = 3;
    const auto DOUBLE_CLICK_TIME = std::chrono::milliseconds(400);

    // xterm reports Shift+arrow as CSI 1;2 A..D; FTXUI has no named events for them.
    const Event SHIFT_UP = Event::Special("\x1B[1;2A");
    const Event SHIFT_DOWN = Event::Special("\x1B[1;2B");
    const Event SHIFT_RIGHT = Event::Special("\x1B[1;2C");
    const Event SHIFT_LEFT = Event::Special("\x1B[1;2D");

    int digitCount(size_t value)
    {
        return static_cast<int>(std::to_string(std::max<size_t>(value, 1)).size());
    }

    // Builds its content once its box is known, so that only what fits is
    // generated and requested from the model, even right after a resize.
    class LayoutNode : public Node
    {
    private:
        std::function<Element(const Box &)> build_;
        Element child_;

    public:
        explicit LayoutNode(std::function<Element(const Box &)> build) : build_(std::move(build)) {}

        void ComputeRequirement() override
        {
            requirement_ = Requirement{};
            requirement_.flex_grow_x = 1;
            requirement_.flex_grow_y = 1;
            requirement_.flex_shrink_x = 1;
            requirement_.flex_shrink_y = 1;
        }

        void SetBox(Box box) override
        {
            Node::SetBox(box);
            child_ = build_(box);
            child_->ComputeRequirement();
            child_->SetBox(box);
        }

        void Render(Screen &screen) override
        {
            if (child_)
                child_->Render(screen);
        }
    };

    size_t addClamped(size_t value, long long delta, size_t max_value)
    {
        if (delta < 0)
        {
            const auto amount = static_cast<size_t>(-delta);
            return value > amount ? value - amount : 0;
        }
        const auto amount = static_cast<size_t>(delta);
        return max_value - value > amount ? value + amount : max_value;
    }
} // namespace

TuiTableView::TuiTableView(std::shared_ptr<TableModel> model, Poster poster)
    : model_(std::move(model)),
      poster_(std::move(poster)),
      token_(std::make_shared<UpdateToken>())
{
    token_->view = this;
    if (model_)
    {
        // Called on the worker thread: just schedule one UI-thread update.
        subscription_ = model_->subscribe([token = token_, poster = poster_](const TableEvent &)
                                          {
            if (!token->pending.exchange(true))
                poster([token]
                       {
                    token->pending = false;
                    if (token->view)
                        token->view->sync(); }); });
    }
    sync();
}

TuiTableView::~TuiTableView()
{
    subscription_.reset(); // waits for an in-flight listener call
    token_->view = nullptr;
}

void TuiTableView::sync()
{
    if (!model_)
        return;
    if (model_->state() == ModelState::Error && !error_reported_ && error_callback_)
    {
        error_reported_ = true;
        error_callback_(model_->errorMessage());
    }
    if (const auto view_error = model_->takeViewError(); view_error && error_callback_)
        error_callback_("Sort and filter were cleared:\n" + *view_error);
    applyPendingView();
    view_ = model_->view();
    autosizeColumns();
    applyPendingScroll();
    if (update_callback_)
        update_callback_();
}

void TuiTableView::autosizeColumns()
{
    const size_t column_count = model_->columnCount();
    if (column_widths_.size() < column_count)
        column_widths_.resize(column_count, 0);
    if (sized_columns_ >= column_count)
        return;
    // Wait for the first block so that widths reflect real data.
    if (model_->peek(0, 0).state == CellState::Loading)
        return;

    const ColumnWidthMetrics metrics{
        .measure = [](const std::string &text)
        { return string_width(text); },
        .header_padding = CELL_PADDING + SORT_INDICATOR_WIDTH,
        .cell_padding = CELL_PADDING,
        .min_width = MIN_COLUMN_WIDTH,
        .max_width = MAX_AUTO_COLUMN_WIDTH,
    };
    for (size_t c = sized_columns_; c < column_count; ++c)
        column_widths_[c] = fittingColumnWidth(*model_, c, metrics);
    sized_columns_ = column_count;
}

int TuiTableView::columnWidth(size_t column) const
{
    return column < column_widths_.size() && column_widths_[column] > 0 ? column_widths_[column] : DEFAULT_COLUMN_WIDTH;
}

TuiTableView::ScrollPosition TuiTableView::scrollPosition() const
{
    return {top_row_, left_col_, cursor_row_, cursor_col_};
}

void TuiTableView::restoreScrollPosition(ScrollPosition position)
{
    pending_scroll_ = position;
    applyPendingScroll();
}

void TuiTableView::restoreView(ViewSpec spec)
{
    if (spec.empty())
        return;
    pending_view_ = std::move(spec);
    applyPendingView();
}

void TuiTableView::applyPendingView()
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

void TuiTableView::applyPendingScroll()
{
    if (!pending_scroll_ || !model_)
        return;
    const size_t rows = model_->displayRowCount();
    const size_t cols = model_->columnCount();
    // Rows of sequential sources appear as they are read, so the position is
    // approached step by step until it exists.
    const ScrollPosition target = *pending_scroll_;
    const size_t last_row = rows > 0 ? rows - 1 : 0;
    const size_t last_col = cols > 0 ? cols - 1 : 0;
    top_row_ = std::min(target.top_row, last_row);
    left_col_ = std::min(target.left_col, last_col);
    cursor_row_ = std::min(target.cursor_row, last_row);
    cursor_col_ = std::min(target.cursor_col, last_col);
    follow_cursor_ = false;

    const bool rows_final = model_->rowCount().isExact() || model_->state() == ModelState::Error;
    const bool reached = target.top_row < rows && target.cursor_row < rows && target.left_col < cols && target.cursor_col < cols;
    if (rows_final || reached)
        pending_scroll_.reset();
}

void TuiTableView::clampCursor()
{
    const size_t rows = model_->displayRowCount();
    const size_t cols = model_->columnCount();
    cursor_row_ = rows > 0 ? std::min(cursor_row_, rows - 1) : 0;
    cursor_col_ = cols > 0 ? std::min(cursor_col_, cols - 1) : 0;
    top_row_ = rows > 0 ? std::min(top_row_, rows - 1) : 0;
    left_col_ = cols > 0 ? std::min(left_col_, cols - 1) : 0;
    if (anchor_)
    {
        anchor_->first = rows > 0 ? std::min(anchor_->first, rows - 1) : 0;
        anchor_->second = cols > 0 ? std::min(anchor_->second, cols - 1) : 0;
    }
    cursor_col_ = hidden_.nearestVisible(cursor_col_, 1, cols);
    // Frozen columns are always drawn, so scrolling starts after them.
    if (frozen_ < cols)
        left_col_ = std::max(left_col_, frozen_);
}

int TuiTableView::frozenWidth() const
{
    const size_t cols = model_ ? model_->columnCount() : 0;
    int width = 0;
    for (size_t c = 0; c < std::min(frozen_, cols); ++c)
    {
        if (!hidden_.contains(c))
            width += columnWidth(c) + 1;
    }
    return width;
}

void TuiTableView::freezeColumns(size_t count)
{
    // Columns that were frozen come back into view (as far as the cursor allows).
    if (count < frozen_)
        left_col_ = std::min(left_col_, count);
    frozen_ = count;
    follow_cursor_ = true;
}

void TuiTableView::scrollCursorIntoView(int body_width)
{
    const size_t visible_rows = static_cast<size_t>(std::max(body_rows_, 1));
    if (cursor_row_ < top_row_)
        top_row_ = cursor_row_;
    else if (cursor_row_ >= top_row_ + visible_rows)
        top_row_ = cursor_row_ - visible_rows + 1;

    // Frozen columns are always in view.
    if (cursor_col_ < frozen_)
        return;
    body_width -= frozenWidth();
    if (cursor_col_ < left_col_)
    {
        left_col_ = cursor_col_;
        return;
    }
    // Scroll right until the cursor column fits (or is the first one drawn).
    while (left_col_ < cursor_col_)
    {
        int used = 0;
        for (size_t c = left_col_; c <= cursor_col_; ++c)
        {
            if (!hidden_.contains(c))
                used += columnWidth(c) + 1;
        }
        if (used <= body_width)
            break;
        ++left_col_;
    }
}

TuiTableView::Selection TuiTableView::selection() const
{
    if (!anchor_)
        return {cursor_row_, cursor_col_, cursor_row_, cursor_col_};
    return {std::min(cursor_row_, anchor_->first), std::min(cursor_col_, anchor_->second),
            std::max(cursor_row_, anchor_->first), std::max(cursor_col_, anchor_->second)};
}

void TuiTableView::setCursor(size_t row, size_t col, bool extend)
{
    if (extend || visual_)
    {
        if (!anchor_)
            anchor_ = std::make_pair(cursor_row_, cursor_col_);
    }
    else
    {
        anchor_.reset();
    }
    cursor_row_ = row;
    cursor_col_ = col;
    follow_cursor_ = true;
    pending_scroll_.reset();
}

void TuiTableView::moveCursor(long long rows, long long cols, bool extend)
{
    const size_t row_count = model_->displayRowCount();
    const size_t col_count = model_->columnCount();
    if (row_count == 0 || col_count == 0)
        return;
    size_t col = addClamped(cursor_col_, cols, col_count - 1);
    if (cols != 0)
        col = hidden_.nearestVisible(col, cols < 0 ? -1 : 1, col_count);
    setCursor(addClamped(cursor_row_, rows, row_count - 1), col, extend);
}

void TuiTableView::hideColumns(size_t first, size_t last)
{
    const size_t cols = model_ ? model_->columnCount() : 0;
    if (hidden_.hide(first, last, cols) == 0)
        return;
    anchor_.reset();
    visual_ = false;
    setCursor(cursor_row_, hidden_.nearestVisible(first, 1, cols), false);
}

void TuiTableView::jumpTo(size_t row, size_t col)
{
    visual_ = false;
    hidden_.show(col);
    if (model_ && row >= model_->displayRowCount() && !model_->rowCount().isExact() &&
        model_->state() != ModelState::Error)
    {
        anchor_.reset();
        // Like a restored position: the row is put at the top once it exists.
        restoreScrollPosition({row, col, row, col});
        return;
    }
    setCursor(row, col, false);
}

bool TuiTableView::cycleSort()
{
    if (!model_ || model_->columnCount() == 0 || !model_->viewCapabilities().sort)
        return false;
    ViewSpec spec = model_->requestedView();
    spec.sort = nextSortCycle(spec.sort, cursor_col_);
    model_->setView(std::move(spec));
    return true;
}

void TuiTableView::widenColumn(int delta)
{
    if (!model_ || model_->columnCount() == 0)
        return;
    if (column_widths_.size() <= cursor_col_)
        column_widths_.resize(cursor_col_ + 1, 0);
    column_widths_[cursor_col_] = std::clamp(columnWidth(cursor_col_) + delta, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
}

Element TuiTableView::OnRender()
{
    if (!model_)
        return text("") | flex | reflect(box_);
    switch (model_->state())
    {
    case ModelState::Opening:
        return text("Loading…") | dim | center | flex | reflect(box_);
    case ModelState::Error:
        return paragraph(model_->errorMessage()) | color(Color::Red) | flex | reflect(box_);
    case ModelState::Ready:
        break;
    }

    clampCursor();
    return std::make_shared<LayoutNode>([this](const Box &box)
                                        { return renderGrid(box); });
}

Element TuiTableView::renderGrid(const Box &box)
{
    box_ = box;
    clampCursor();
    const size_t rows = model_->displayRowCount();
    const size_t cols = model_->columnCount();

    const int width = std::max(box.x_max - box.x_min + 1, 0);
    const int height = std::max(box.y_max - box.y_min + 1, 0);
    body_rows_ = std::max(height - 1, 1);
    row_header_width_ = digitCount(rows) + 1;
    const int body_width = std::max(width - row_header_width_ - 1, 1);

    if (follow_cursor_)
        scrollCursorIntoView(body_width);
    const size_t visible_rows = rows > top_row_ ? std::min(rows - top_row_, static_cast<size_t>(body_rows_)) : 0;
    if (visible_rows > 0)
        model_->requestRange(top_row_, top_row_ + visible_rows - 1);

    // Frozen columns, then those that fit from left_col_; the last one may be cut.
    drawn_columns_.clear();
    size_t frozen_drawn = 0;
    {
        int x = row_header_width_ + 1;
        auto add = [&](size_t c)
        {
            const int w = columnWidth(c);
            drawn_columns_.push_back({c, x, std::min(x + w, row_header_width_ + body_width)});
            x += w + 1;
        };
        for (size_t c = 0; c < std::min(frozen_, cols) && x - row_header_width_ - 1 < body_width; ++c)
        {
            if (!hidden_.contains(c))
                add(c);
        }
        frozen_drawn = drawn_columns_.size();
        for (size_t c = std::max(left_col_, frozen_); c < cols && x - row_header_width_ - 1 < body_width; ++c)
        {
            if (!hidden_.contains(c))
                add(c);
        }
    }

    const Selection sel = selection();
    const bool range = anchor_.has_value();
    const auto header_style = bold | bgcolor(Color::GrayDark) | color(Color::White);
    const auto cursor_style = bgcolor(Color::Blue) | color(Color::White);
    // Black on yellow reads well on both light and dark terminals.
    const auto hit_style = bgcolor(Color::Yellow) | color(Color::Black);
    // Edited and not saved yet.
    const auto modified_style = bgcolor(Color::Green) | color(Color::Black);
    // A new node each time: a node used twice is drawn only at its last position.
    const auto separator = []
    { return text("│") | color(Color::GrayDark); };
    // The edge of the frozen columns.
    const auto separatorAfter = [&](size_t index)
    { return index + 1 == frozen_drawn ? text("┃") | color(Color::GrayLight) : separator(); };

    Elements lines;
    lines.reserve(visible_rows + 1);

    // Column header
    {
        Elements cells;
        cells.push_back(text(std::string(static_cast<size_t>(row_header_width_), ' ')) | header_style);
        cells.push_back(separator());
        for (size_t index = 0; index < drawn_columns_.size(); ++index)
        {
            const DrawnColumn &drawn = drawn_columns_[index];
            const int w = columnWidth(drawn.column);
            std::string suffix;
            int suffix_width = 0;
            if (filtersColumn(view_, drawn.column))
            {
                suffix += " ▿";
                suffix_width += FILTER_INDICATOR_WIDTH;
            }
            if (!view_.sort.empty() && view_.sort.front().column == drawn.column)
            {
                suffix += view_.sort.front().ascending ? " ▲" : " ▼";
                suffix_width += SORT_INDICATOR_WIDTH;
            }
            const int name_width = std::max(w - CELL_PADDING - suffix_width, 0);
            std::string label =
                " " + fitText(singleLine(model_->columnName(drawn.column)), name_width, TextAlign::Center) + suffix + " ";
            label = fitText(label, drawn.x_max - drawn.x_min, TextAlign::Left, false);
            const bool active = drawn.column >= sel.col_left && drawn.column <= sel.col_right;
            cells.push_back(text(label) | (active ? cursor_style : header_style));
            cells.push_back(separatorAfter(index));
        }
        lines.push_back(hbox(std::move(cells)));
    }

    for (size_t i = 0; i < visible_rows; ++i)
    {
        const size_t r = top_row_ + i;
        const bool row_active = r >= sel.row_top && r <= sel.row_bottom;
        Elements cells;
        cells.push_back(text(fitText(std::to_string(r + 1), row_header_width_ - 1, TextAlign::Right) + " ") |
                        (row_active ? cursor_style : color(Color::GrayLight)));
        cells.push_back(separator());
        for (size_t index = 0; index < drawn_columns_.size(); ++index)
        {
            const DrawnColumn &drawn = drawn_columns_[index];
            const int w = columnWidth(drawn.column);
            const CellRef cell = model_->peek(r, drawn.column);
            std::string content;
            Decorator style = nothing;
            switch (cell.state)
            {
            case CellState::Ready:
                content = fitText(cellDisplayText(*cell.value), w - CELL_PADDING,
                                  isNumericCell(*cell.value) ? TextAlign::Right : TextAlign::Left);
                break;
            case CellState::Loading:
                content = fitText("…", w - CELL_PADDING, TextAlign::Left);
                style = dim;
                break;
            case CellState::Error:
                content = fitText("!", w - CELL_PADDING, TextAlign::Left);
                style = color(Color::Red);
                break;
            case CellState::Empty:
            case CellState::OutOfRange:
                content = std::string(static_cast<size_t>(std::max(w - CELL_PADDING, 0)), ' ');
                break;
            }
            std::string label = fitText(" " + content + " ", drawn.x_max - drawn.x_min, TextAlign::Left, false);

            const bool is_cursor = r == cursor_row_ && drawn.column == cursor_col_;
            const bool selected = range && row_active && drawn.column >= sel.col_left && drawn.column <= sel.col_right;
            Element element = text(std::move(label)) | style;
            if (is_cursor)
                element = element | cursor_style;
            else if (selected)
                element = element | inverted;
            else if (search_ && searchHitIndex(*search_, {r, drawn.column}))
                element = element | hit_style;
            else if (model_->cellModified(r, drawn.column))
                element = element | modified_style;
            cells.push_back(std::move(element));
            cells.push_back(separatorAfter(index));
        }
        lines.push_back(hbox(std::move(cells)));
    }

    if (cols == 0)
        lines.push_back(text(rows == 0 ? "(empty)" : "") | dim);

    return vbox(std::move(lines));
}

std::optional<std::pair<size_t, size_t>> TuiTableView::cellAt(int x, int y) const
{
    if (!box_.Contain(x, y) || !model_)
        return std::nullopt;
    const int body_y = y - box_.y_min - 1;
    if (body_y < 0)
        return std::nullopt;
    const size_t row = top_row_ + static_cast<size_t>(body_y);
    if (row >= model_->displayRowCount())
        return std::nullopt;
    const int rel_x = x - box_.x_min;
    for (const DrawnColumn &drawn : drawn_columns_)
    {
        if (rel_x >= drawn.x_min && rel_x <= drawn.x_max)
            return std::make_pair(row, drawn.column);
    }
    return std::nullopt;
}

bool TuiTableView::handleMouse(Event &event)
{
    Mouse &mouse = event.mouse();
    const bool inside = box_.Contain(mouse.x, mouse.y);

    switch (mouse.button)
    {
    case Mouse::WheelUp:
    case Mouse::WheelDown:
    case Mouse::WheelLeft:
    case Mouse::WheelRight:
    {
        if (!inside)
            return false;
        const bool up = mouse.button == Mouse::WheelUp;
        const bool horizontal = mouse.shift || mouse.button == Mouse::WheelLeft || mouse.button == Mouse::WheelRight;
        const bool back = up || mouse.button == Mouse::WheelLeft;
        const size_t rows = model_->displayRowCount();
        const size_t cols = model_->columnCount();
        if (horizontal)
            left_col_ = std::max(hidden_.nearestVisible(addClamped(left_col_, back ? -1 : 1, cols > 0 ? cols - 1 : 0),
                                                        back ? -1 : 1, cols),
                                 std::min(frozen_, cols > 0 ? cols - 1 : 0));
        else
            top_row_ = addClamped(top_row_, back ? -static_cast<long long>(WHEEL_ROWS) : static_cast<long long>(WHEEL_ROWS),
                                  rows > 0 ? rows - 1 : 0);
        follow_cursor_ = false;
        pending_scroll_.reset();
        return true;
    }
    case Mouse::Left:
        break;
    default:
        return false;
    }

    if (mouse.motion == Mouse::Released)
    {
        const bool was_dragging = dragging_;
        dragging_ = false;
        return was_dragging;
    }

    const auto cell = cellAt(mouse.x, mouse.y);
    if (mouse.motion == Mouse::Moved)
    {
        if (!dragging_ || !cell)
            return dragging_;
        setCursor(cell->first, cell->second, true);
        return true;
    }

    // Pressed
    if (!cell)
        return false;
    TakeFocus();
    const auto now = std::chrono::steady_clock::now();
    const bool double_click = *cell == last_click_cell_ && now - last_click_time_ < DOUBLE_CLICK_TIME;
    last_click_time_ = now;
    last_click_cell_ = *cell;
    if (!mouse.shift)
        visual_ = false;
    setCursor(cell->first, cell->second, mouse.shift);
    dragging_ = !mouse.shift;
    if (double_click && activate_callback_)
    {
        last_click_cell_ = {SIZE_MAX, SIZE_MAX};
        dragging_ = false;
        activate_callback_();
    }
    return true;
}

bool TuiTableView::OnEvent(Event event)
{
    if (!model_ || model_->state() != ModelState::Ready)
        return false;
    if (event.is_mouse())
        return handleMouse(event);

    const long long page = std::max(body_rows_ - 1, 1);
    const long long far = LLONG_MAX / 2;

    if (event == Event::ArrowUp || event == Event::k)
        moveCursor(-1, 0, false);
    else if (event == Event::ArrowDown || event == Event::j)
        moveCursor(1, 0, false);
    else if (event == Event::ArrowLeft || event == Event::h)
        moveCursor(0, -1, false);
    else if (event == Event::ArrowRight || event == Event::l)
        moveCursor(0, 1, false);
    else if (event == SHIFT_UP)
        moveCursor(-1, 0, true);
    else if (event == SHIFT_DOWN)
        moveCursor(1, 0, true);
    else if (event == SHIFT_LEFT)
        moveCursor(0, -1, true);
    else if (event == SHIFT_RIGHT)
        moveCursor(0, 1, true);
    else if (event == Event::PageUp || event == Event::CtrlB)
        moveCursor(-page, 0, false);
    else if (event == Event::PageDown || event == Event::CtrlF)
        moveCursor(page, 0, false);
    else if (event == Event::ArrowUpCtrl || event == Event::g)
        moveCursor(-far, 0, false);
    else if (event == Event::ArrowDownCtrl || event == Event::G)
        moveCursor(far, 0, false);
    else if (event == Event::ArrowLeftCtrl || event == Event::Home || event == Event::Character('0'))
        moveCursor(0, -far, false);
    else if (event == Event::ArrowRightCtrl || event == Event::End || event == Event::Character('$'))
        moveCursor(0, far, false);
    else if (event == Event::v)
    {
        // Terminals that do not report Shift+arrows can still select a range.
        visual_ = !visual_;
        if (visual_)
            anchor_ = std::make_pair(cursor_row_, cursor_col_);
        else
            anchor_.reset();
    }
    else if (event == Event::CtrlA)
    {
        const size_t rows = model_->displayRowCount();
        const size_t cols = model_->columnCount();
        if (rows == 0 || cols == 0)
            return true;
        anchor_ = std::make_pair(size_t{0}, size_t{0});
        visual_ = false;
        cursor_row_ = rows - 1;
        cursor_col_ = cols - 1;
        follow_cursor_ = true;
    }
    else if (event == Event::Escape)
    {
        if (!anchor_)
            return false;
        anchor_.reset();
        visual_ = false;
    }
    else if (event == Event::Return)
    {
        if (activate_callback_)
            activate_callback_();
    }
    else
        return false;
    return true;
}
