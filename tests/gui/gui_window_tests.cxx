#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "file_util.hxx"
#include "gui_test_helpers.hxx"

using namespace gui_test;

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    GuiTableView *viewNamed(const Window &window, const std::string &label)
    {
        for (GuiTableView *view : window.views())
        {
            if (view->label() && view->label() == label)
                return view;
        }
        return nullptr;
    }

    // Runs a query and waits for its sheet.
    GuiTableView *runSql(Window &window, const std::string &sql, size_t rows)
    {
        window->run_sql(sql, false);
        GuiTableView *view = nullptr;
        REQUIRE(waitFor([&]
                        {
            view = viewNamed(window, "SQL Result");
            return view && view->model() && view->model()->rowCount().isExact() &&
                   view->model()->rowCount().value == rows; }));
        return view;
    }

    // The current view's cell as text, once loaded ("" until then).
    std::string currentCell(const Window &window, size_t row, size_t column)
    {
        GuiTableView *view = window.currentView();
        if (!view || !view->model())
            return {};
        const CellRef cell = view->model()->peek(row, column);
        return cell.state == CellState::Ready ? formatCell(*cell.value) : std::string();
    }

    // Whether the current view shows `rows` rows (an exact count). A view
    // made by a reopen may take the old one's address, so tests tell a
    // reopen by what it shows.
    bool currentRows(const Window &window, size_t rows)
    {
        GuiTableView *view = window.currentView();
        return view && view->model() && view->model()->rowCount().isExact() && view->model()->rowCount().value == rows;
    }

    // The model of every view is its tab's sheet.
    void checkViewsMatchTabs(const Window &window)
    {
        for (GuiTableView *view : window.views())
        {
            REQUIRE(view->model());
            CHECK(view->model()->name() == view->label());
        }
    }

    // Reopens the file, as File > Revert and Auto Revert do, and waits for
    // the new views.
    void reopen(Window &window)
    {
        const std::string path = window->opened_path();
        REQUIRE(window->open(path, false));
        pump();
    }
} // namespace

TEST_CASE("GUI: a file opens with a tab per sheet")
{
    Window window(kDemoData / "multisheet.xlsx");
    REQUIRE(window.views().size() >= 2);
    CHECK(window->opened_path() == utf8String(kDemoData / "multisheet.xlsx"));
    checkViewsMatchTabs(window);
    GuiTableView *view = window.currentView();
    REQUIRE(view);
    CHECK(waitFor([&] { return view->model() && view->model()->rowCount().isExact(); }));
}

TEST_CASE("GUI: an SQL result goes before the Group By sheet", "[regression]")
{
    Window window(kDemoData / "normal.parquet");
    REQUIRE(window.waitForRows(window.currentView(), 54));

    {
        ModalAnswer group([](Fl_Window &dialog) { return pressButton(dialog, "Group"); });
        window->show_group_by(4); // City
        REQUIRE(group.answered());
    }
    REQUIRE(waitFor([&] { return window.sheetLabels().size() == 2; }));
    GuiTableView *group_by = viewNamed(window, "Group By");
    REQUIRE(group_by);
    REQUIRE(window.waitForRows(group_by, 12));

    runSql(window, "SELECT 1 AS one", 1);
    CHECK(window.sheetLabels() == std::vector<std::string>{"normal.parquet", "SQL Result", "Group By"});
    checkViewsMatchTabs(window);
    CHECK(window.waitForRows(viewNamed(window, "Group By"), 12));

    SECTION("and stays there when the file is reopened on the Group By sheet")
    {
        window->tab->value(viewNamed(window, "Group By"));
        reopen(window);
        REQUIRE(waitFor([&] { return window.sheetLabels().size() == 3; }));
        CHECK(window.sheetLabels() == std::vector<std::string>{"normal.parquet", "SQL Result", "Group By"});
        checkViewsMatchTabs(window);
        REQUIRE(window.currentView());
        CHECK(window.currentView()->label() == std::string("Group By"));
        CHECK(window.waitForRows(viewNamed(window, "SQL Result"), 1));
        CHECK(window.waitForRows(viewNamed(window, "Group By"), 12));
    }
}

TEST_CASE("GUI: the SQL Result sheet keeps its columns' state when the file is reopened", "[regression]")
{
    Window window(kDemoData / "normal.parquet");
    REQUIRE(window.waitForRows(window.currentView(), 54));
    GuiTableView *result = runSql(window, "SELECT * FROM normal", 54);
    result->hideColumns(1, 1);
    result->freezeColumns(3);
    REQUIRE(result->hiddenColumns().contains(1));
    REQUIRE(result->frozenColumns() == 3);

    reopen(window);
    GuiTableView *again = nullptr;
    REQUIRE(waitFor([&]
                    {
        again = viewNamed(window, "SQL Result");
        return again && again->model() && again->model()->rowCount().isExact(); }));
    CHECK(again->hiddenColumns().contains(1));
    CHECK(again->hiddenColumns().size() == 1);
    CHECK(again->frozenColumns() == 3);
}

TEST_CASE("GUI: Auto Revert waits while a cell is being edited", "[regression]")
{
    TempDir dir;
    const auto path = dir.write("t.csv", "a,b\n1,x\n2,y\n");
    Window window(path);
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 3));
    window->set_focused(true); // polls every second
    window->set_editing(true);
    REQUIRE(window->editing());
    view->editCell(1, 0, std::string("typed"));
    REQUIRE(view->editingCell());

    std::ofstream(path, std::ios::binary | std::ios::app) << "3,z\n";
    pump(2500ms);
    CHECK(currentRows(window, 3)); // not reopened
    CHECK(view->editingCell());

    // Cancelled: nothing is lost by reading the file again.
    view->finishEditing(false);
    CHECK(waitFor([&] { return currentRows(window, 4); }));
}

TEST_CASE("GUI: a pending sort does not move the cell being edited", "[regression]")
{
    TempDir dir;
    // No header is detected: the rows are "a,b", "1,x" and "2,y".
    const auto path = dir.write("t.csv", "a,b\n1,x\n2,y\n");
    Window window(path);
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 3));
    window->set_editing(true);
    view->editCell(2, 0, std::string("typed"));
    REQUIRE(view->editingCell());

    // The sort a reopen restores can arrive while the user is typing.
    view->restoreView({.sort = {{.column = 1, .ascending = false}}});
    CHECK_FALSE(view->editingCell());
    REQUIRE(waitFor([&] { return !view->model()->view().sort.empty(); }));
    // "2,y" sorts first now; the text went to its row, not to the row that
    // is third after the sort.
    CHECK(waitFor([&] { return currentCell(window, 0, 1) == "y" && currentCell(window, 0, 0) == "typed"; }));
    CHECK(currentCell(window, 1, 0) == "1");
    CHECK(currentCell(window, 2, 0) == "a");
}

TEST_CASE("GUI: Follow does not reopen the file while a menu or dialog runs its loop", "[regression]")
{
    TempDir dir;
    const auto path = dir.write("log.csv", "time,level\n12:00,INFO\n");
    Window window(path);
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 2));
    window->set_follow(true);

    std::ofstream(path, std::ios::binary | std::ios::app) << "12:01,WARN\n";
    REQUIRE(window.waitForRows(view, 3)); // appended, same view

    // No read left running: the next one is the one started below.
    window->set_follow(false);
    pump(700ms);

    // Rewritten while a read is under way, whose answer (read the file
    // again) comes while a menu or dialog runs its loop: the reopen waits,
    // as it would delete widgets that loop is using.
    dir.write("log.csv", "other,header\nx,y\n");
    window->set_follow(true); // starts the read at once
    {
        NestedLoopGuard guard;
        pump(1500ms);
        CHECK(currentRows(window, 3));
        CHECK(currentCell(window, 0, 0) == "time");
    }
    CHECK(waitFor([&] { return currentRows(window, 2) && currentCell(window, 0, 0) == "other"; }));
    window->set_follow(false);
}

TEST_CASE("GUI: charts and statistics start on a visible column", "[regression]")
{
    Window window(kDemoData / "normal.parquet");
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 54));
    view->hideColumns(0, 0);
    view->set_selection(0, 0, 0, 0); // the hidden column

    window->show_stats();
    CHECK(view->model()->columnStatsState().column == 1);
    window->show_histogram();
    CHECK(view->model()->histogramState().column == 1);

    // A selection that starts on a hidden column: its visible ends.
    view->set_selection(0, 0, 0, 3);
    window->show_scatter_plot();
    CHECK(view->model()->scatterState().x_column == 1);
    CHECK(view->model()->scatterState().y_column == 3);
}

TEST_CASE("GUI: edits are saved and the file is read again")
{
    TempDir dir;
    const auto path = dir.write("t.csv", "a,b\n1,x\n2,y\n");
    Window window(path);
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 3));
    window->set_editing(true);
    view->editCell(1, 0, std::string("42"));
    view->finishEditing(true);
    REQUIRE(waitFor([&] { return view->model()->modified(); }));

    REQUIRE(window->save(false));
    CHECK(readFile(path) == "a,b\n42,x\n2,y\n");
    GuiTableView *reopened = window.currentView();
    REQUIRE(reopened);
    REQUIRE(window.waitForRows(reopened, 3));
    CHECK(window->editing()); // edit mode stays on
    CHECK(waitFor([&]
                  {
        const CellRef cell = reopened->model()->peek(1, 0);
        return cell.state == CellState::Ready && formatCell(*cell.value) == "42"; }));
}
