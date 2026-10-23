#ifndef _GUI_FILTER_PANE_HXX_
#define _GUI_FILTER_PANE_HXX_

#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Item.H>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gui_theme.hxx"
#include "table.hxx"

class Fl_Box;
class Fl_Button;
class Fl_Choice;
class Fl_Input;
class Fl_Round_Button;
class Fl_Scroll;

// The sidebar's Filter tab: edits the current sheet's filter as a list of
// conditions. Edits are applied with Apply or Enter; removing a condition and
// Clear apply at once.
class GuiFilterPane : public Fl_Group
{
private:
    struct ConditionRow
    {
        Fl_Choice *column = nullptr;
        Fl_Choice *op = nullptr;
        Fl_Input *value = nullptr;
        Fl_Button *remove = nullptr;
    };

    std::shared_ptr<TableModel> model_;
    // The model's filter as last loaded or applied here; a different one was
    // set elsewhere (e.g. a quick filter) and replaces the edited conditions.
    Filter synced_;
    // The model was replaced: the next update() loads its filter.
    bool reload_ = false;
    // A filter that Apply refused (an invalid regular expression), and why.
    std::optional<Filter> refused_;
    std::string refused_error_;
    std::function<size_t()> default_column_;

    Fl_Box *match_label_;
    Fl_Round_Button *match_all_;
    Fl_Round_Button *match_any_;
    Fl_Scroll *scroll_;
    Fl_Box *content_;
    Fl_Button *add_button_;
    Fl_Button *apply_button_;
    Fl_Button *clear_button_;
    Fl_Box *status_;
    ChangingTooltip status_tooltip_;
    std::vector<ConditionRow> rows_;
    // Shared by the column choices; the labels must outlive the items.
    std::vector<std::string> column_labels_;
    std::vector<Fl_Menu_Item> column_menu_;

    static void deferredUpdate(void *data);
    void rebuildColumnMenu();
    void loadFilter(const Filter &filter);
    void addRow(const FilterCondition &condition);
    void removeRow(Fl_Widget *button);
    Filter editedFilter() const;
    void apply();
    void clear();
    void layout();
    void updateState();

public:
    GuiFilterPane(int x, int y, int w, int h, const char *label = 0);
    ~GuiFilterPane() override;

    // The sheet whose filter is edited; null for none.
    void setModel(std::shared_ptr<TableModel> model);
    const std::shared_ptr<TableModel> &model() const { return model_; }
    // Called after model events: picks up filters set elsewhere and refreshes
    // the status line.
    void update();
    // Adds a condition on the column and focuses its value.
    void addCondition(size_t column);
    // Adds a condition on the default column (as "+ Add condition" does).
    void addDefaultCondition() { addCondition(default_column_ ? default_column_() : 0); }
    bool hasConditions() const { return !rows_.empty(); }
    // The column of a condition added with "+ Add condition".
    void setDefaultColumn(std::function<size_t()> provider) { default_column_ = std::move(provider); }

    void resize(int x, int y, int w, int h) override;
};

#endif // _GUI_FILTER_PANE_HXX_
