#include "gui_stats_pane.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Table.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>

#include "cell_format.hxx"
#include "gui_table_view.hxx"
#include "gui_theme.hxx"

namespace
{
    const int MARGIN = 5;
    const int GAP = 5;
    const int LINE_HEIGHT = 24;
    const int STATUS_HEIGHT = 20;
    const int BUTTON_WIDTH = 70;
    const int FONT_SIZE = 14;
    const int ROW_HEIGHT = FONT_SIZE + 8;
    const int CELL_PADDING = 12;
    const int MIN_FLEX_WIDTH = 40;
    const int MIN_VALUES_HEIGHT = 3 * ROW_HEIGHT;
    // Menu items whose value is longer are cut.
    const size_t MENU_VALUE_BYTES = 40;

    enum class MenuAction : intptr_t
    {
        None,
        FilterEqual,
        FilterNotEqual,
        Copy,
    };

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

    void setActive(Fl_Widget *widget, bool active)
    {
        if (active)
            widget->activate();
        else
            widget->deactivate();
    }
} // namespace

// A read-only table of text rows. One column takes the width that the
// others leave, so that there is no horizontal scrolling.
class GuiTextTable : public Fl_Table
{
private:
    std::vector<std::string> headers_; // none: no header row
    std::vector<Fl_Align> aligns_;
    size_t flex_column_;
    std::vector<std::vector<std::string>> rows_;
    int selected_ = -1;

    void fitColumns()
    {
        fl_font(FL_HELVETICA, FONT_SIZE);
        int fixed = 0;
        for (size_t c = 0; c < aligns_.size(); ++c)
        {
            if (c == flex_column_)
                continue;
            int width = headers_.empty() ? 0 : static_cast<int>(fl_width(headers_[c].c_str()));
            for (const auto &row : rows_)
                width = std::max(width, static_cast<int>(fl_width(row[c].c_str())));
            col_width(static_cast<int>(c), width + CELL_PADDING);
            fixed += width + CELL_PADDING;
        }
        col_width(static_cast<int>(flex_column_), std::max(MIN_FLEX_WIDTH, tiw - fixed));
    }

public:
    // Called with the row double-clicked, or right-clicked (selectable tables only).
    std::function<void(size_t)> activate_callback;
    std::function<void(size_t)> menu_callback;

    GuiTextTable(int x, int y, int w, int h, std::vector<std::string> headers, std::vector<Fl_Align> aligns,
                 size_t flex_column)
        : Fl_Table(x, y, w, h), headers_(std::move(headers)), aligns_(std::move(aligns)), flex_column_(flex_column)
    {
        col_header(headers_.empty() ? 0 : 1);
        cols(static_cast<int>(aligns_.size()));
        rows(0);
        row_height_all(ROW_HEIGHT);
        col_header_height(ROW_HEIGHT);
        color(FL_BACKGROUND2_COLOR);
        // Fl_Table's constructor leaves its internal scroll group open.
        end();
        fitColumns();
    }

    void setRows(std::vector<std::vector<std::string>> rows)
    {
        if (rows == rows_)
            return;
        rows_ = std::move(rows);
        const int count = static_cast<int>(std::min<size_t>(rows_.size(), INT_MAX));
        if (count != Fl_Table::rows())
            Fl_Table::rows(count);
        if (selected_ >= count)
            selected_ = -1;
        fitColumns();
        redraw();
    }

    void clearSelection()
    {
        selected_ = -1;
        redraw();
    }

    // Tall enough to show every row without scrolling.
    int fullHeight() const
    {
        return (headers_.empty() ? 0 : ROW_HEIGHT) + static_cast<int>(rows_.size()) * ROW_HEIGHT + 2 * Fl::box_dy(box());
    }

    int handle(int event) override
    {
        int R, C;
        ResizeFlag resize_flag;
        const TableContext context = cursor2rowcol(R, C, resize_flag);
        if (event == FL_PUSH && context == CONTEXT_COL_HEADER)
            return 1;
        if (event == FL_PUSH && context == CONTEXT_CELL && (activate_callback || menu_callback))
        {
            selected_ = R;
            redraw();
            if (Fl::event_button() == FL_RIGHT_MOUSE)
            {
                if (menu_callback)
                    menu_callback(static_cast<size_t>(R));
            }
            else if (Fl::event_clicks() && activate_callback)
            {
                activate_callback(static_cast<size_t>(R));
            }
            return 1;
        }
        return Fl_Table::handle(event);
    }

    void draw() override
    {
        styleScrollbar(vscrollbar);
        styleScrollbar(hscrollbar);
        Fl_Table::draw();
    }

    void draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H) override
    {
        switch (context)
        {
        case CONTEXT_STARTPAGE:
            fl_font(FL_HELVETICA, FONT_SIZE);
            return;

        case CONTEXT_COL_HEADER:
            if (C >= 0 && static_cast<size_t>(C) < headers_.size())
                drawHeaderCell(headers_[static_cast<size_t>(C)].c_str(), X, Y, W, H, col_header_color());
            return;

        case CONTEXT_CELL:
        {
            if (R < 0 || static_cast<size_t>(R) >= rows_.size() || C < 0 || static_cast<size_t>(C) >= aligns_.size())
                return;
            const ThemeColors &colors = themeColors();
            fl_push_clip(X, Y, W, H);
            fl_color(R == selected_ ? colors.cell_selection : colors.cell_background);
            fl_rectf(X, Y, W, H);
            fl_color(colors.cell_text);
            // draw_symbols = 0: keep "@..." as text (see GuiTableView::draw_cell).
            fl_draw(rows_[static_cast<size_t>(R)][static_cast<size_t>(C)].c_str(), X + 4, Y, W - 8, H,
                    aligns_[static_cast<size_t>(C)] | FL_ALIGN_CLIP, nullptr, 0);
            fl_color(colors.grid_line);
            fl_xyline(X, Y + H - 1, X + W - 1);
            fl_pop_clip();
            return;
        }

        default:
            return;
        }
    }

    void resize(int x, int y, int w, int h) override
    {
        Fl_Table::resize(x, y, w, h);
        fitColumns();
    }
};

GuiStatsPane::GuiStatsPane(int x, int y, int w, int h, const char *label)
    : Fl_Group(x, y, w, h, label)
{
    column_choice_ = new Fl_Choice(x, y, w, LINE_HEIGHT);
    column_choice_->tooltip("The column to summarize");
    column_choice_->callback([](Fl_Widget *, void *data)
                             {
        auto *self = static_cast<GuiStatsPane *>(data);
        const int column = self->column_choice_->value();
        if (column >= 0)
            self->showColumn(static_cast<size_t>(column)); }, this);
    cancel_button_ = new Fl_Button(x, y, BUTTON_WIDTH, LINE_HEIGHT, "Cancel");
    cancel_button_->tooltip("Stop computing the statistics");
    cancel_button_->callback([](Fl_Widget *, void *data)
                             {
        auto *self = static_cast<GuiStatsPane *>(data);
        if (self->model_)
            self->model_->cancelColumnStats();
        self->update(); }, this);
    status_ = new Fl_Box(x, y, w, STATUS_HEIGHT);
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    summary_ = new GuiTextTable(x, y, w, h, {}, {FL_ALIGN_LEFT, FL_ALIGN_LEFT}, 1);
    values_ = new GuiTextTable(x, y, w, h, {"Most frequent", "Count", "%"},
                               {FL_ALIGN_LEFT, FL_ALIGN_RIGHT, FL_ALIGN_RIGHT}, 0);
    values_->tooltip("Double-click a value to filter by it; right-click for more");
    values_->activate_callback = [this](size_t index)
    { filterByValue(index, true); };
    values_->menu_callback = [this](size_t index)
    { showValueMenu(index); };
    end();

    layout();
    update();
}

GuiStatsPane::~GuiStatsPane()
{
    Fl::remove_timeout(deferredUpdate, this);
}

void GuiStatsPane::resize(int x, int y, int w, int h)
{
    // Laid out by hand: Fl_Group would scale the tables with the pane.
    Fl_Widget::resize(x, y, w, h);
    layout();
}

void GuiStatsPane::layout()
{
    const int left = x() + MARGIN;
    const int width = std::max(w() - 2 * MARGIN, 10);
    int top = y() + MARGIN;
    column_choice_->resize(left, top, std::max(width - BUTTON_WIDTH - GAP, 10), LINE_HEIGHT);
    cancel_button_->resize(left + width - BUTTON_WIDTH, top, BUTTON_WIDTH, LINE_HEIGHT);
    top += LINE_HEIGHT + GAP;
    status_->resize(left, top, width, STATUS_HEIGHT);
    top += STATUS_HEIGHT + GAP;
    // The summary is short: shown whole, unless the pane is too low for it.
    const int bottom = y() + h() - MARGIN;
    const int summary_height = std::min(summary_->fullHeight(), std::max(bottom - top - GAP - MIN_VALUES_HEIGHT, ROW_HEIGHT));
    summary_->resize(left, top, width, summary_height);
    top += summary_height + GAP;
    values_->resize(left, top, width, std::max(bottom - top, ROW_HEIGHT));
    redraw();
}

void GuiStatsPane::setModel(std::shared_ptr<TableModel> model)
{
    if (model != model_)
    {
        model_ = std::move(model);
        values_->clearSelection();
    }
    update();
}

void GuiStatsPane::showColumn(size_t column)
{
    if (!model_)
        return;
    model_->startColumnStats(column);
    values_->clearSelection();
    update();
}

void GuiStatsPane::deferredUpdate(void *data)
{
    static_cast<GuiStatsPane *>(data)->update();
}

void GuiStatsPane::update()
{
    // An open menu (which holds the grab) may be the column choice showing column_menu_.
    if (Fl::grab())
    {
        Fl::remove_timeout(deferredUpdate, this);
        Fl::add_timeout(0.1, deferredUpdate, this);
        return;
    }
    ColumnStatsState state = model_ ? model_->columnStatsState() : ColumnStatsState{};
    if (state.generation != state_.generation || state.active != state_.active)
        values_->clearSelection();
    state_ = std::move(state);

    rebuildColumnMenu();
    const bool shown = state_.active && state_.column < column_labels_.size();
    column_choice_->value(shown ? static_cast<int>(state_.column) : -1);
    setActive(column_choice_, model_ && !column_labels_.empty());
    setActive(cancel_button_, state_.active && !state_.done);

    std::string text;
    if (!model_ || model_->state() == ModelState::Error)
        text.clear();
    else if (!state_.active)
        text = column_labels_.empty() ? "" : "Choose a column.";
    else if (!state_.done)
        text = "Computing… " + std::to_string(state_.scanned_rows) + " rows";
    else
        text = rowCountText({state_.scanned_rows, RowCount::Kind::Exact}) +
               (model_->view().filtered() ? " (filtered)" : "");
    text = escapeLabel(text);
    if (!status_->label() || text != status_->label())
    {
        status_->copy_label(text.c_str());
        status_->redraw();
    }

    std::vector<std::vector<std::string>> summary;
    std::vector<std::vector<std::string>> values;
    // Nothing is published until the first rows have been scanned.
    if (state_.active && (state_.done || state_.stats.rows > 0))
    {
        for (const StatsLine &line : columnStatsLines(state_.stats))
            summary.push_back({line.label, line.value});
        for (const ValueCount &value : state_.stats.top)
            values.push_back({cellDisplayText(value.value), std::to_string(value.count),
                              percentText(value.count, state_.stats.rows)});
    }
    const int old_height = summary_->fullHeight();
    summary_->setRows(std::move(summary));
    values_->setRows(std::move(values));
    if (summary_->fullHeight() != old_height)
        layout();
}

void GuiStatsPane::rebuildColumnMenu()
{
    std::vector<std::string> labels;
    const size_t count = model_ ? model_->columnCount() : 0;
    const bool named = model_ && model_->hasHeader();
    labels.reserve(count);
    for (size_t c = 0; c < count; ++c)
    {
        // Header names may repeat; the letter tells them apart.
        const std::string name = model_->columnName(c);
        labels.push_back(escapeLabel(named ? name + " (" + columnLetter(c) + ")" : name, true));
    }
    if (labels == column_labels_ && !column_menu_.empty())
        return;
    column_labels_ = std::move(labels);
    column_menu_.clear();
    for (const std::string &label : column_labels_)
        column_menu_.push_back({label.c_str(), 0, nullptr, nullptr, 0});
    column_menu_.push_back({nullptr});
    column_choice_->menu(column_menu_.data());
}

void GuiStatsPane::filterByValue(size_t index, bool equal)
{
    if (!model_ || !model_->viewCapabilities().filter || index >= state_.stats.top.size())
        return;
    ViewSpec spec = model_->requestedView();
    addFilterCondition(spec, cellFilterCondition(state_.column, state_.stats.top[index].value, equal));
    model_->setView(std::move(spec));
}

void GuiStatsPane::showValueMenu(size_t index)
{
    if (!model_ || index >= state_.stats.top.size())
        return;
    const CellValue value = state_.stats.top[index].value;
    const std::string equal_label = quickFilterLabel(cellFilterCondition(state_.column, value, true));
    const std::string not_equal_label = quickFilterLabel(cellFilterCondition(state_.column, value, false));
    const int quick = model_->viewCapabilities().filter ? 0 : FL_MENU_INACTIVE;
    auto item = [](const char *label, MenuAction action, int flags)
    {
        return Fl_Menu_Item{label, 0, nullptr, reinterpret_cast<void *>(static_cast<intptr_t>(action)), flags};
    };
    const Fl_Menu_Item items[] = {
        item(equal_label.c_str(), MenuAction::FilterEqual, quick),
        item(not_equal_label.c_str(), MenuAction::FilterNotEqual, quick | FL_MENU_DIVIDER),
        item("Copy Value", MenuAction::Copy, 0),
        {nullptr},
    };
    const std::shared_ptr<TableModel> model = model_;
    MenuAction action;
    {
        NestedLoopGuard guard;
        action = menuAction(items[0].popup(Fl::event_x(), Fl::event_y()));
    }
    // A new SQL result may have replaced the sheet meanwhile.
    if (model_ != model)
        return;
    switch (action)
    {
    case MenuAction::FilterEqual:
    case MenuAction::FilterNotEqual:
        filterByValue(index, action == MenuAction::FilterEqual);
        break;
    case MenuAction::Copy:
    {
        const std::string text = formatCell(value);
        Fl::copy(text.c_str(), static_cast<int>(text.size()), 2);
        break;
    }
    default:
        break;
    }
}
