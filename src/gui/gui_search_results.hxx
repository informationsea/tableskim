#ifndef _GUI_SEARCH_RESULTS_HXX_
#define _GUI_SEARCH_RESULTS_HXX_

#include <FL/Fl_Table.H>
#include <functional>
#include <memory>
#include <string>
#include "table.hxx"

// Sidebar list of the hits of a TableModel's current search, one row per hit.
// It does not subscribe to the model: the owner calls update() on the main
// thread when the model has reported progress.
class GuiSearchResults : public Fl_Table
{
private:
    std::shared_ptr<TableModel> model_;
    SearchState state_;
    int reported_row_ = -1; // hit last passed to the activate callback
    std::function<void(CellPosition)> activate_callback_;

    void clearSelection();
    void fitColumns();

public:
    GuiSearchResults(int x, int y, int w, int h, const char *label = 0);

    // Shows the model's search (nullptr: none) and clears the selection.
    void setModel(std::shared_ptr<TableModel> model);
    const std::shared_ptr<TableModel> &model() const { return model_; }
    // Picks up new hits; a restarted search clears the selection.
    void update();

    // "Searching...", the number of hits, ...; empty without a search.
    std::string statusText() const;

    // Called when the user selects a hit (click or arrow keys). Clicking the
    // selected hit again calls it again.
    void setActivateCallback(std::function<void(CellPosition)> callback) { activate_callback_ = std::move(callback); }

    virtual int handle(int event) override;
    virtual void draw() override;
    virtual void draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H) override;
    virtual void resize(int x, int y, int w, int h) override;
};

#endif // _GUI_SEARCH_RESULTS_HXX_
