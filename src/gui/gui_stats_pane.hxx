#ifndef _GUI_STATS_PANE_HXX_
#define _GUI_STATS_PANE_HXX_

#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Item.H>
#include <memory>
#include <string>
#include <vector>

#include "table.hxx"

class Fl_Box;
class Fl_Button;
class Fl_Choice;
class GuiTextTable;

// The sidebar's Statistics tab: the current sheet's column statistics
// (TableModel::startColumnStats()), with the most frequent values below.
// Double-clicking a value filters the sheet by it. Like GuiSearchResults it
// does not subscribe to the model: the owner calls update() after events.
class GuiStatsPane : public Fl_Group
{
private:
    std::shared_ptr<TableModel> model_;
    ColumnStatsState state_;

    Fl_Choice *column_choice_;
    Fl_Button *cancel_button_;
    Fl_Box *status_;
    GuiTextTable *summary_;
    GuiTextTable *values_;
    // Shared by the column choice; the labels must outlive the items.
    std::vector<std::string> column_labels_;
    std::vector<Fl_Menu_Item> column_menu_;

    static void deferredUpdate(void *data);
    void rebuildColumnMenu();
    void layout();
    void filterByValue(size_t index, bool equal);
    void showValueMenu(size_t index);

public:
    GuiStatsPane(int x, int y, int w, int h, const char *label = 0);
    ~GuiStatsPane() override;

    // The sheet whose statistics are shown; null for none.
    void setModel(std::shared_ptr<TableModel> model);
    const std::shared_ptr<TableModel> &model() const { return model_; }
    // Starts the statistics of the column (they replace those of another).
    void showColumn(size_t column);
    // Picks up the model's progress.
    void update();

    void resize(int x, int y, int w, int h) override;
};

#endif // _GUI_STATS_PANE_HXX_
