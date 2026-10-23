#include "gui_filter_pane.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scroll.H>
#include <algorithm>
#include <cstring>
#include <iterator>

#include "cell_format.hxx"
#include "gui_theme.hxx"

namespace
{
    const int MARGIN = 5;
    const int GAP = 5;
    const int LINE_HEIGHT = 24;
    const int MATCH_LABEL_WIDTH = 50;
    const int MATCH_BUTTON_WIDTH = 50;
    const int REMOVE_WIDTH = 24;
    const int MAX_OP_WIDTH = 110;
    const int BUTTON_WIDTH = 70;
    const int ADD_BUTTON_WIDTH = 140;
    const int STATUS_HEIGHT = 40;

    const Fl_Menu_Item *opMenu()
    {
        static const std::vector<Fl_Menu_Item> items = []
        {
            std::vector<Fl_Menu_Item> result;
            for (const FilterOp op : FILTER_OPS)
                result.push_back({filterOpLabel(op), 0, nullptr, nullptr, 0});
            result.push_back({nullptr});
            return result;
        }();
        return items.data();
    }

    int opIndex(FilterOp op)
    {
        const auto it = std::find(std::begin(FILTER_OPS), std::end(FILTER_OPS), op);
        return it != std::end(FILTER_OPS) ? static_cast<int>(it - std::begin(FILTER_OPS)) : 0;
    }

    // The combine mode does not matter without conditions.
    bool sameFilter(const Filter &a, const Filter &b)
    {
        return (a.conditions.empty() && b.conditions.empty()) || a == b;
    }

    Filter filterOf(const ViewSpec &spec)
    {
        return spec.filter.value_or(Filter{});
    }

    int labelWidth(const Fl_Widget *widget)
    {
        int w = 0;
        int h = 0;
        widget->measure_label(w, h);
        return w;
    }

    void setActive(Fl_Widget *widget, bool active)
    {
        if (active)
            widget->activate();
        else
            widget->deactivate();
    }
} // namespace

GuiFilterPane::GuiFilterPane(int x, int y, int w, int h, const char *label)
    : Fl_Group(x, y, w, h, label)
{
    auto changed = [](Fl_Widget *, void *data)
    { static_cast<GuiFilterPane *>(data)->updateState(); };

    match_label_ = new Fl_Box(x, y, MATCH_LABEL_WIDTH, LINE_HEIGHT, "Match:");
    match_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    match_all_ = new Fl_Round_Button(x, y, MATCH_BUTTON_WIDTH, LINE_HEIGHT, "All");
    match_all_->type(FL_RADIO_BUTTON);
    match_all_->value(1);
    match_all_->tooltip("Show rows that match all conditions (AND)");
    match_all_->callback(changed, this);
    match_any_ = new Fl_Round_Button(x, y, MATCH_BUTTON_WIDTH, LINE_HEIGHT, "Any");
    match_any_->type(FL_RADIO_BUTTON);
    match_any_->tooltip("Show rows that match any condition (OR)");
    match_any_->callback(changed, this);

    scroll_ = new Fl_Scroll(x, y, w, h);
    scroll_->type(Fl_Scroll::VERTICAL);
    scroll_->box(FL_FLAT_BOX);
    // Spans the whole content, margins included: Fl_Scroll derives its
    // position from the children's bounding box when drawing.
    content_ = new Fl_Box(x, y, w, h);
    add_button_ = new Fl_Button(x, y, ADD_BUTTON_WIDTH, LINE_HEIGHT, "+ Add condition");
    add_button_->callback([](Fl_Widget *, void *data)
                          { static_cast<GuiFilterPane *>(data)->addDefaultCondition(); }, this);
    scroll_->end();

    apply_button_ = new Fl_Button(x, y, BUTTON_WIDTH, LINE_HEIGHT, "Apply");
    apply_button_->callback([](Fl_Widget *, void *data)
                            { static_cast<GuiFilterPane *>(data)->apply(); }, this);
    clear_button_ = new Fl_Button(x, y, BUTTON_WIDTH, LINE_HEIGHT, "Clear");
    clear_button_->tooltip("Remove all conditions");
    clear_button_->callback([](Fl_Widget *, void *data)
                            { static_cast<GuiFilterPane *>(data)->clear(); }, this);
    status_ = new Fl_Box(x, y, w, STATUS_HEIGHT);
    status_->align(FL_ALIGN_TOP_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_CLIP);
    end();

    layout();
    updateState();
}

void GuiFilterPane::resize(int x, int y, int w, int h)
{
    // Laid out by hand: Fl_Group would scale the rows with the pane.
    Fl_Widget::resize(x, y, w, h);
    layout();
}

void GuiFilterPane::layout()
{
    const int left = x() + MARGIN;
    const int width = std::max(w() - 2 * MARGIN, 10);
    int top = y() + MARGIN;
    // Sized from the labels, which depend on the font and its size; an
    // inside label is also inset by a few pixels.
    const int label_width = std::max(labelWidth(match_label_) + 2 * GAP, MATCH_LABEL_WIDTH);
    // A round button draws its box (about the label height) left of the label.
    const int all_width = std::max(labelWidth(match_all_) + LINE_HEIGHT + GAP, MATCH_BUTTON_WIDTH);
    const int any_width = std::max(labelWidth(match_any_) + LINE_HEIGHT + GAP, MATCH_BUTTON_WIDTH);
    match_label_->resize(left, top, label_width, LINE_HEIGHT);
    match_all_->resize(left + label_width, top, all_width, LINE_HEIGHT);
    match_any_->resize(left + label_width + all_width, top, any_width, LINE_HEIGHT);
    top += LINE_HEIGHT + GAP;

    const int buttons_top = y() + h() - MARGIN - STATUS_HEIGHT - GAP - LINE_HEIGHT;
    scroll_->resize(x(), top, w(), std::max(buttons_top - GAP - top, LINE_HEIGHT));

    // Room for the scrollbar is always kept, so that it does not reflow the rows.
    const int inner_width = std::max(w() - 2 * MARGIN - Fl::scrollbar_size(), 40);
    const int op_width = std::min(MAX_OP_WIDTH, inner_width / 2);
    const int content_height = static_cast<int>(rows_.size()) * (2 * LINE_HEIGHT + 4 * GAP) + LINE_HEIGHT + 2 * GAP;
    const int max_scroll = std::max(content_height - scroll_->h(), 0);
    const int scroll_top = std::clamp(scroll_->yposition(), 0, max_scroll);
    scroll_->scroll_to(0, scroll_top);
    content_->resize(x(), scroll_->y() - scroll_top, inner_width + MARGIN, content_height);
    int row_top = content_->y() + GAP;
    for (const ConditionRow &row : rows_)
    {
        row.column->resize(left, row_top, inner_width - REMOVE_WIDTH - GAP, LINE_HEIGHT);
        row.remove->resize(left + inner_width - REMOVE_WIDTH, row_top, REMOVE_WIDTH, LINE_HEIGHT);
        row_top += LINE_HEIGHT + GAP;
        row.op->resize(left, row_top, op_width, LINE_HEIGHT);
        row.value->resize(left + op_width + GAP, row_top, inner_width - op_width - GAP, LINE_HEIGHT);
        row_top += LINE_HEIGHT + 3 * GAP;
    }
    add_button_->resize(left, row_top, std::min(inner_width, ADD_BUTTON_WIDTH), LINE_HEIGHT);

    apply_button_->resize(left, buttons_top, BUTTON_WIDTH, LINE_HEIGHT);
    clear_button_->resize(left + BUTTON_WIDTH + GAP, buttons_top, BUTTON_WIDTH, LINE_HEIGHT);
    status_->resize(left, buttons_top + LINE_HEIGHT + GAP, width, STATUS_HEIGHT);
    redraw();
}

void GuiFilterPane::setModel(std::shared_ptr<TableModel> model)
{
    if (model == model_)
    {
        update();
        return;
    }
    model_ = std::move(model);
    // Through update(), which waits while a menu is open.
    reload_ = true;
    update();
}

GuiFilterPane::~GuiFilterPane()
{
    Fl::remove_timeout(deferredUpdate, this);
}

void GuiFilterPane::deferredUpdate(void *data)
{
    static_cast<GuiFilterPane *>(data)->update();
}

void GuiFilterPane::update()
{
    // An open menu (which holds the grab) may be a column choice showing
    // column_menu_, or belong to a row that loadFilter() would delete.
    if (Fl::grab())
    {
        Fl::remove_timeout(deferredUpdate, this);
        Fl::add_timeout(0.1, deferredUpdate, this);
        return;
    }
    rebuildColumnMenu();
    if (model_ || reload_)
    {
        const Filter current = model_ ? filterOf(model_->requestedView()) : Filter{};
        if (reload_ || !sameFilter(current, synced_))
        {
            reload_ = false;
            synced_ = current;
            loadFilter(current);
        }
    }
    updateState();
}

void GuiFilterPane::rebuildColumnMenu()
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
    for (const ConditionRow &row : rows_)
    {
        const int value = row.column->value();
        row.column->menu(column_menu_.data());
        if (!column_labels_.empty())
            row.column->value(std::clamp(value, 0, static_cast<int>(column_labels_.size()) - 1));
    }
}

void GuiFilterPane::loadFilter(const Filter &filter)
{
    for (const ConditionRow &row : rows_)
    {
        for (Fl_Widget *widget : {static_cast<Fl_Widget *>(row.column), static_cast<Fl_Widget *>(row.op),
                                  static_cast<Fl_Widget *>(row.value), static_cast<Fl_Widget *>(row.remove)})
        {
            scroll_->remove(widget);
            // Deferred: this may run in the callback of one of them.
            Fl::delete_widget(widget);
        }
    }
    rows_.clear();
    (filter.combine == Filter::Combine::Or ? match_any_ : match_all_)->setonly();
    for (const FilterCondition &condition : filter.conditions)
        addRow(condition);
    scroll_->scroll_to(0, 0);
    layout();
}

void GuiFilterPane::addRow(const FilterCondition &condition)
{
    // Created outside any group, then added before the "+ Add condition" button.
    Fl_Group *const saved = Fl_Group::current();
    Fl_Group::current(nullptr);
    auto changed = [](Fl_Widget *, void *data)
    { static_cast<GuiFilterPane *>(data)->updateState(); };

    ConditionRow row;
    row.column = new Fl_Choice(0, 0, 10, LINE_HEIGHT);
    row.column->menu(column_menu_.data());
    if (!column_labels_.empty())
        row.column->value(static_cast<int>(std::min(condition.column, column_labels_.size() - 1)));
    row.column->callback(changed, this);
    row.op = new Fl_Choice(0, 0, 10, LINE_HEIGHT);
    row.op->menu(opMenu());
    row.op->value(opIndex(condition.op));
    row.op->callback(changed, this);
    row.value = new Fl_Input(0, 0, 10, LINE_HEIGHT);
    row.value->value(condition.value.c_str());
    row.value->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    row.value->callback([](Fl_Widget *, void *data)
                        {
        auto *self = static_cast<GuiFilterPane *>(data);
        if (Fl::callback_reason() == FL_REASON_ENTER_KEY)
            self->apply();
        else
            self->updateState(); }, this);
    row.remove = new Fl_Button(0, 0, REMOVE_WIDTH, LINE_HEIGHT, "×");
    row.remove->tooltip("Remove this condition");
    row.remove->callback([](Fl_Widget *w, void *data)
                         { static_cast<GuiFilterPane *>(data)->removeRow(w); }, this);
    Fl_Group::current(saved);

    for (Fl_Widget *widget : {static_cast<Fl_Widget *>(row.column), static_cast<Fl_Widget *>(row.op),
                              static_cast<Fl_Widget *>(row.value), static_cast<Fl_Widget *>(row.remove)})
        scroll_->insert(*widget, add_button_);
    rows_.push_back(row);
}

void GuiFilterPane::removeRow(Fl_Widget *button)
{
    const auto it = std::find_if(rows_.begin(), rows_.end(), [button](const ConditionRow &row)
                                 { return row.remove == button; });
    if (it == rows_.end())
        return;
    for (Fl_Widget *widget : {static_cast<Fl_Widget *>(it->column), static_cast<Fl_Widget *>(it->op),
                              static_cast<Fl_Widget *>(it->value), static_cast<Fl_Widget *>(it->remove)})
    {
        scroll_->remove(widget);
        Fl::delete_widget(widget);
    }
    rows_.erase(it);
    layout();
    apply();
}

void GuiFilterPane::addCondition(size_t column)
{
    if (!model_ || !model_->viewCapabilities().filter || column_labels_.empty())
        return;
    addRow({column, FilterOp::Contains, {}});
    layout();
    // Scroll the new row into view.
    const ConditionRow &row = rows_.back();
    const int overflow = add_button_->y() + add_button_->h() + GAP - (scroll_->y() + scroll_->h());
    if (overflow > 0)
    {
        scroll_->scroll_to(0, scroll_->yposition() + overflow);
        layout();
    }
    row.value->take_focus();
    updateState();
}

Filter GuiFilterPane::editedFilter() const
{
    Filter filter;
    filter.combine = match_any_->value() ? Filter::Combine::Or : Filter::Combine::And;
    for (const ConditionRow &row : rows_)
    {
        const int column = row.column->value();
        const int op = row.op->value();
        if (column < 0 || op < 0 || op >= static_cast<int>(std::size(FILTER_OPS)))
            continue;
        FilterCondition condition{static_cast<size_t>(column), FILTER_OPS[op], {}};
        if (filterOpTakesValue(condition.op))
            condition.value = row.value->value();
        filter.conditions.push_back(std::move(condition));
    }
    return filter;
}

void GuiFilterPane::apply()
{
    if (model_ && model_->viewCapabilities().filter)
    {
        Filter filter = editedFilter();
        if (const auto error = filterError(filter))
        {
            refused_ = filter;
            refused_error_ = *error;
            updateState();
            return;
        }
        refused_.reset();
        ViewSpec spec = model_->requestedView();
        synced_ = filter;
        if (filter.conditions.empty())
            spec.filter.reset();
        else
            spec.filter = std::move(filter);
        model_->setView(std::move(spec));
    }
    updateState();
}

void GuiFilterPane::clear()
{
    loadFilter({});
    apply();
}

void GuiFilterPane::updateState()
{
    const bool usable = model_ && model_->viewCapabilities().filter;
    for (Fl_Widget *widget : {static_cast<Fl_Widget *>(match_all_), static_cast<Fl_Widget *>(match_any_),
                              static_cast<Fl_Widget *>(add_button_), static_cast<Fl_Widget *>(apply_button_),
                              static_cast<Fl_Widget *>(clear_button_)})
        setActive(widget, usable);
    for (const ConditionRow &row : rows_)
    {
        setActive(row.column, usable);
        setActive(row.op, usable);
        setActive(row.remove, usable);
        const int op = row.op->value();
        setActive(row.value, usable && op >= 0 && filterOpTakesValue(FILTER_OPS[op]));
    }

    std::string text;
    if (!model_ || model_->state() == ModelState::Error)
        text.clear();
    else if (!usable)
        text = "Still opening (cannot filter yet).";
    else if (refused_ && *refused_ == editedFilter())
        text = refused_error_;
    else if (!(model_->requestedView() == model_->view()))
        text = "Filtering…";
    else if (!sameFilter(editedFilter(), synced_))
        text = "Press Apply (or Enter) to filter.";
    else
    {
        const bool filtered = model_->view().filtered();
        text = filteredRowCountText(model_->rowCount(), model_->unfilteredRowCount(), filtered);
        if (!filtered)
            text = "No filter (" + text + ")";
    }
    text = escapeLabel(text);
    if (!status_->label() || text != status_->label())
    {
        status_->copy_label(text.c_str());
        // A narrow sidebar cuts long messages; the tooltip shows them whole.
        status_tooltip_.set(status_, text);
        redraw();
    }
}
