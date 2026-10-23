#include "gui_search_results.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <climits>

#include "cell_format.hxx"
#include "gui_theme.hxx"

namespace
{
    const int FONT_SIZE = 14;
    const int CELL_PADDING = 12;
    const int MAX_COLUMN_NAME_WIDTH = 120;
    // Minimums for a narrow sidebar; text is clipped rather than scrolled.
    const int MIN_COLUMN_NAME_WIDTH = 40;
    const int MIN_VALUE_WIDTH = 40;
    // Column names wider than the cap are clipped anyway, so a few are enough.
    const size_t MEASURED_COLUMNS = 100;

    enum Column
    {
        COLUMN_ROW,
        COLUMN_NAME,
        COLUMN_VALUE,
        COLUMN_COUNT,
    };

    const char *const COLUMN_HEADERS[COLUMN_COUNT] = {"Row", "Column", "Value"};

    int textWidth(const std::string &text)
    {
        return static_cast<int>(fl_width(text.c_str())) + CELL_PADDING;
    }
} // namespace

GuiSearchResults::GuiSearchResults(int x, int y, int w, int h, const char *label)
    : Fl_Table(x, y, w, h, label)
{
    col_header(1);
    cols(COLUMN_COUNT);
    rows(0);
    row_height_all(FONT_SIZE + 8);
    col_header_height(FONT_SIZE + 8);
    color(FL_BACKGROUND2_COLOR);
    // Fl_Table's constructor leaves its internal scroll group open.
    end();
    fitColumns();
}

void GuiSearchResults::setModel(std::shared_ptr<TableModel> model)
{
    model_ = std::move(model);
    state_ = {};
    clearSelection();
    update();
}

void GuiSearchResults::update()
{
    SearchState state = model_ ? model_->searchState() : SearchState{};
    // Awake callbacks are coalesced, so a restart may come with as many hits
    // as before (a sort keeps the count); the generation tells.
    if (state.generation != state_.generation || state.active != state_.active)
        clearSelection();
    state_ = std::move(state);
    const int new_rows = static_cast<int>(std::min<size_t>(state_.hits.size(), INT_MAX));
    if (new_rows != rows())
        rows(new_rows);
    fitColumns();
    redraw();
}

std::string GuiSearchResults::statusText() const
{
    if (!state_.active)
        return {};
    if (!state_.error.empty())
        return state_.error;
    const size_t count = state_.hits.size();
    const std::string hits = std::to_string(count) + (count == 1 ? " hit" : " hits");
    if (!state_.done)
        return "Searching... " + hits + " in " + std::to_string(state_.scanned_rows) + " rows";
    if (state_.truncated)
        return hits + " (stopped: too many hits)";
    return hits;
}

void GuiSearchResults::clearSelection()
{
    current_row = current_col = select_row = select_col = -1;
    reported_row_ = -1;
    redraw();
}

void GuiSearchResults::fitColumns()
{
    fl_font(FL_HELVETICA, FONT_SIZE);
    // Hits are in view order, so the last one has the largest row number.
    const size_t last_row = state_.hits.empty() ? 0 : state_.hits.back().row + 1;
    const int row_width = std::max(textWidth(COLUMN_HEADERS[COLUMN_ROW]), textWidth(std::to_string(std::max<size_t>(last_row, 100))));
    int name_width = textWidth(COLUMN_HEADERS[COLUMN_NAME]);
    if (model_)
    {
        const size_t columns = std::min(model_->columnCount(), MEASURED_COLUMNS);
        for (size_t c = 0; c < columns && name_width < MAX_COLUMN_NAME_WIDTH; ++c)
            name_width = std::max(name_width, textWidth(model_->columnName(c)));
    }
    // In a narrow sidebar, column names are clipped before values are.
    name_width = std::clamp(std::min(name_width, tiw - row_width - MIN_VALUE_WIDTH), MIN_COLUMN_NAME_WIDTH, MAX_COLUMN_NAME_WIDTH);
    col_width(COLUMN_ROW, row_width);
    col_width(COLUMN_NAME, name_width);
    // The value column takes the rest, so that there is no horizontal scrolling.
    col_width(COLUMN_VALUE, std::max(MIN_VALUE_WIDTH, tiw - row_width - name_width));
}

void GuiSearchResults::resize(int x, int y, int w, int h)
{
    Fl_Table::resize(x, y, w, h);
    fitColumns();
}

int GuiSearchResults::handle(int event)
{
    int R, C;
    ResizeFlag resize_flag;
    const TableContext context = cursor2rowcol(R, C, resize_flag);
    // Fl_Table selects a whole column on a header click, which would jump to the last hit.
    if (event == FL_PUSH && context == CONTEXT_COL_HEADER)
        return 1;

    // Fl_Table takes the focus only when the mouse enters it; the arrow keys
    // should browse the hits right after a click.
    if (event == FL_PUSH && context == CONTEXT_CELL && Fl::focus() != this)
        take_focus();
    const int handled = Fl_Table::handle(event);

    // Rows are hits, so the selection always spans a whole row; select_row is
    // the end that follows the mouse and the arrow keys.
    if (select_row < 0 || select_row >= rows())
        return handled;
    const int row = select_row;
    if (current_row != row || select_row != row || current_col != 0 || select_col != COLUMN_COUNT - 1)
        set_selection(row, 0, row, COLUMN_COUNT - 1);
    // Clicking the selected hit again jumps back to it after the user scrolled away.
    const bool clicked = event == FL_PUSH && context == CONTEXT_CELL;
    if (row != reported_row_ || clicked)
    {
        reported_row_ = row;
        if (activate_callback_)
            activate_callback_(state_.hits[static_cast<size_t>(row)]);
    }
    return handled;
}

void GuiSearchResults::draw()
{
    styleScrollbar(vscrollbar);
    styleScrollbar(hscrollbar);
    Fl_Table::draw();
}

void GuiSearchResults::draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H)
{
    switch (context)
    {
    case CONTEXT_STARTPAGE:
        fl_font(FL_HELVETICA, FONT_SIZE);
        return;

    case CONTEXT_COL_HEADER:
        if (C >= 0 && C < COLUMN_COUNT)
            drawHeaderCell(COLUMN_HEADERS[C], X, Y, W, H, col_header_color());
        return;

    case CONTEXT_CELL:
    {
        if (R < 0 || static_cast<size_t>(R) >= state_.hits.size())
            return;
        const CellPosition hit = state_.hits[static_cast<size_t>(R)];
        const ThemeColors &colors = themeColors();
        fl_push_clip(X, Y, W, H);
        fl_color(R == select_row ? colors.cell_selection : colors.cell_background);
        fl_rectf(X, Y, W, H);

        std::string text;
        Fl_Color text_color = colors.cell_text;
        Fl_Align align = FL_ALIGN_LEFT;
        switch (C)
        {
        case COLUMN_ROW:
            text = std::to_string(hit.row + 1);
            align = FL_ALIGN_RIGHT;
            break;
        case COLUMN_NAME:
            text = model_ ? model_->columnName(hit.col) : std::string();
            break;
        case COLUMN_VALUE:
        {
            // Hits far from the visible rows may have been evicted; peek()
            // loads them again and the next update() redraws.
            const CellRef cell = model_ ? model_->peek(hit.row, hit.col) : CellRef{CellState::OutOfRange};
            if (cell.state == CellState::Ready)
            {
                text = cellDisplayText(*cell.value);
            }
            else if (cell.state == CellState::Loading)
            {
                text = "…";
                text_color = colors.cell_muted_text;
            }
            break;
        }
        default:
            break;
        }
        fl_color(text_color);
        // draw_symbols = 0: keep "@..." as text (see GuiTableView::draw_cell).
        fl_draw(text.c_str(), X + 4, Y, W - 8, H, align | FL_ALIGN_CLIP, nullptr, 0);

        fl_color(colors.grid_line);
        fl_xyline(X, Y + H - 1, X + W - 1);
        fl_pop_clip();
        return;
    }

    default:
        return;
    }
}
