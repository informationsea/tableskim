#include "gui_group_by.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Return_Button.H>
#include <algorithm>
#include <memory>
#include <string>

#include "gui_table_view.hxx"
#include "gui_theme.hxx"
#include "table.hxx"

namespace
{
    const int WINDOW_WIDTH = 520;
    const int WINDOW_HEIGHT = 420;
    const int MARGIN = 10;
    const int LABEL_HEIGHT = 20;
    const int CHECK_HEIGHT = 22;
    const int CHECK_WIDTH = 120;
    const int BUTTON_WIDTH = 80;
    const int BUTTON_HEIGHT = 25;
    const int AGGREGATE_COLUMNS = 4;

    struct DialogState
    {
        bool done = false;
        bool accepted = false;
    };

    void finish(DialogState &state, bool accepted)
    {
        state.accepted = accepted;
        state.done = true;
    }
} // namespace

std::optional<GroupBySpec> askGroupBySpec(const TableModel &model, const HiddenColumns &hidden,
                                          const GroupBySpec &initial)
{
    const size_t column_count = model.columnCount();
    std::vector<size_t> columns; // browser item - 1 -> model column
    for (size_t c = 0; c < column_count; ++c)
    {
        if (!hidden.contains(c))
            columns.push_back(c);
    }
    const auto chosen = [](const std::vector<size_t> &list, size_t column)
    { return std::find(list.begin(), list.end(), column) != list.end(); };

    DialogState state;
    Fl_Double_Window dialog(WINDOW_WIDTH, WINDOW_HEIGHT, "Group By");
    const int list_width = (WINDOW_WIDTH - 3 * MARGIN) / 2;
    const int aggregate_rows = (static_cast<int>(std::size(AGGREGATES)) + AGGREGATE_COLUMNS - 1) / AGGREGATE_COLUMNS;
    const int list_top = MARGIN + LABEL_HEIGHT;
    const int list_height = WINDOW_HEIGHT - list_top - 3 * MARGIN - LABEL_HEIGHT - aggregate_rows * CHECK_HEIGHT -
                            BUTTON_HEIGHT;

    Fl_Box keys_label(MARGIN, MARGIN, list_width, LABEL_HEIGHT, "Group by:");
    keys_label.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    Fl_Check_Browser keys(MARGIN, list_top, list_width, list_height);
    Fl_Box values_label(2 * MARGIN + list_width, MARGIN, list_width, LABEL_HEIGHT, "Aggregate:");
    values_label.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    Fl_Check_Browser values(2 * MARGIN + list_width, list_top, list_width, list_height);
    for (const size_t column : columns)
    {
        // '@' would start a symbol.
        const std::string name = escapeLabel(model.columnName(column));
        keys.add(name.c_str(), chosen(initial.keys, column) ? 1 : 0);
        values.add(name.c_str(), chosen(initial.values, column) ? 1 : 0);
    }

    const int aggregates_top = list_top + list_height + MARGIN;
    Fl_Box aggregates_label(MARGIN, aggregates_top, WINDOW_WIDTH - 2 * MARGIN, LABEL_HEIGHT,
                            "Of each aggregated column (besides the rows of each group):");
    aggregates_label.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    std::vector<std::unique_ptr<Fl_Check_Button>> aggregate_buttons;
    for (size_t i = 0; i < std::size(AGGREGATES); ++i)
    {
        const int x = MARGIN + static_cast<int>(i % AGGREGATE_COLUMNS) * CHECK_WIDTH;
        const int y = aggregates_top + LABEL_HEIGHT + static_cast<int>(i / AGGREGATE_COLUMNS) * CHECK_HEIGHT;
        auto button = std::make_unique<Fl_Check_Button>(x, y, CHECK_WIDTH, CHECK_HEIGHT, aggregateName(AGGREGATES[i]));
        button->value(std::find(initial.aggregates.begin(), initial.aggregates.end(), AGGREGATES[i]) !=
                      initial.aggregates.end());
        aggregate_buttons.push_back(std::move(button));
    }

    Fl_Button cancel(WINDOW_WIDTH - 2 * (MARGIN + BUTTON_WIDTH), WINDOW_HEIGHT - MARGIN - BUTTON_HEIGHT, BUTTON_WIDTH,
                     BUTTON_HEIGHT, "Cancel");
    cancel.callback([](Fl_Widget *, void *data)
                    { finish(*static_cast<DialogState *>(data), false); }, &state);
    Fl_Return_Button ok(WINDOW_WIDTH - MARGIN - BUTTON_WIDTH, WINDOW_HEIGHT - MARGIN - BUTTON_HEIGHT, BUTTON_WIDTH,
                        BUTTON_HEIGHT, "Group");
    ok.callback([](Fl_Widget *, void *data)
                { finish(*static_cast<DialogState *>(data), true); }, &state);
    dialog.end();
    dialog.callback([](Fl_Widget *, void *data)
                    { finish(*static_cast<DialogState *>(data), false); }, &state);
    dialog.set_modal();
    dialog.show();
    {
        NestedLoopGuard guard;
        while (!state.done && dialog.shown())
            Fl::wait();
    }
    dialog.hide();
    if (!state.accepted)
        return std::nullopt;

    GroupBySpec spec;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (keys.checked(static_cast<int>(i) + 1))
            spec.keys.push_back(columns[i]);
        if (values.checked(static_cast<int>(i) + 1))
            spec.values.push_back(columns[i]);
    }
    for (size_t i = 0; i < std::size(AGGREGATES); ++i)
    {
        if (aggregate_buttons[i]->value())
            spec.aggregates.push_back(AGGREGATES[i]);
    }
    return spec;
}
