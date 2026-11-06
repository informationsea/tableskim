// The manual's GUI screenshots (docs/images/gui-*.png and chart-*.png),
// taken from the real windows using demo/examples/ datasets. Hidden: run on
// request, with the directory to write them to (docs/images to replace
// them), and look at each image before committing it:
//
//   TABLESKIM_SCREENSHOT_DIR=docs/images xvfb-run -a -s "-screen 0 1600x1200x24"
//       ./build/debug/tableskim-gui-tests "[.screenshots]"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include "cell_format.hxx"
#include "table_export.hxx"
#include "gui_image_export.hxx"
#include "gui_test_helpers.hxx"
#include "gui_window_menu.hxx"

using namespace gui_test;

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;
    const std::filesystem::path kExamples = kDemoData.parent_path() / "examples";

    std::filesystem::path outputDirectory()
    {
        const char *dir = std::getenv("TABLESKIM_SCREENSHOT_DIR");
        return dir ? std::filesystem::path(dir) : std::filesystem::path();
    }

    void visit(Fl_Widget &widget, const std::function<void(Fl_Widget &)> &f)
    {
        f(widget);
        if (Fl_Group *group = widget.as_group())
        {
            for (int i = 0; i < group->children(); ++i)
                visit(*group->child(i), f);
        }
    }

    // The chart widget of a chart window: the one in a sunken frame.
    Fl_Widget *chartOf(Fl_Window &window)
    {
        Fl_Widget *chart = nullptr;
        visit(window, [&](Fl_Widget &widget)
              {
            if (&widget != &window && widget.box() == FL_DOWN_FRAME)
                chart = &widget; });
        return chart;
    }

    // The line under the chart that describes what is pointed at: the
    // lowest box.
    Fl_Box *detailOf(Fl_Window &window)
    {
        Fl_Box *detail = nullptr;
        visit(window, [&](Fl_Widget &widget)
              {
            auto *box = dynamic_cast<Fl_Box *>(&widget);
            if (box && (!detail || box->y() > detail->y()))
                detail = box; });
        return detail;
    }

    void choose(Fl_Window &window, const std::string &label, int value)
    {
        Fl_Choice *choice = findChoice(window, label);
        REQUIRE(choice);
        choice->value(value);
        choice->do_callback();
    }

    void settle()
    {
        Fl::flush();
        waitFor([] { return false; }, std::chrono::milliseconds(300));
    }

    // Moves the mouse over the chart until the detail line contains
    // `text`, scanning the chart from the top left.
    void pointAt(Fl_Window &window, const std::string &text)
    {
        Fl_Widget *chart = chartOf(window);
        Fl_Box *detail = detailOf(window);
        REQUIRE(chart);
        REQUIRE(detail);
        // What lies where comes from the last draw.
        settle();
        for (int y = chart->y() + 4; y < chart->y() + chart->h(); y += 3)
        {
            for (int x = chart->x() + 4; x < chart->x() + chart->w(); x += 2)
            {
                Fl::e_x = x;
                Fl::e_y = y;
                chart->handle(FL_MOVE);
                if (detail->label() && std::string(detail->label()).find(text) != std::string::npos)
                {
                    settle();
                    return;
                }
            }
        }
        FAIL("nothing in the chart shows " << text);
    }

    void write(Fl_Window &window, const std::string &name)
    {
        settle();
        const std::filesystem::path path = outputDirectory() / name;
        INFO(path);
        CHECK(writeWidgetImage(&window, path).empty());
    }

    // The main window with a chart window over its bottom right corner,
    // framed, as they would be seen on the screen.
    void writeOver(Fl_Window &main, Fl_Window &chart, const std::string &name)
    {
        settle();
        Fl_Image_Surface surface(main.w(), main.h());
        surface.draw(&main, 0, 0);
        const int x = main.w() - chart.w() - 12;
        const int y = main.h() - chart.h() - 36;
        surface.draw(&chart, x, y);
        Fl_Surface_Device::push_current(&surface);
        fl_color(FL_DARK3);
        fl_rect(x - 1, y - 1, chart.w() + 2, chart.h() + 2);
        Fl_Surface_Device::pop_current();
        std::unique_ptr<Fl_RGB_Image> image(surface.image());
        const std::filesystem::path path = outputDirectory() / name;
        INFO(path);
        REQUIRE(image);
        CHECK(fl_write_png(path.string().c_str(), image.get()) == 0);
    }

    Fl_Window &histogramOf(Window &window, size_t column)
    {
        GuiTableView *view = window.currentView();
        view->set_selection(0, static_cast<int>(column), 0, static_cast<int>(column));
        window->show_histogram();
        Fl_Window *histogram = shownWindow("Histogram");
        REQUIRE(histogram);
        REQUIRE(waitFor([&] { return view->model()->histogramState().done; }));
        return *histogram;
    }

    Fl_Window &scatterOf(Window &window, size_t x, size_t y)
    {
        GuiTableView *view = window.currentView();
        view->set_selection(0, static_cast<int>(x), 0, static_cast<int>(y));
        window->show_scatter_plot();
        Fl_Window *scatter = shownWindow("Scatter Plot");
        REQUIRE(scatter);
        REQUIRE(waitFor([&] { return view->model()->scatterState().done; }));
        return *scatter;
    }

    bool waitHistogram(GuiTableView *view)
    {
        settle();
        return waitFor([&] { return view->model()->histogramState().done; });
    }

    bool waitScatter(GuiTableView *view)
    {
        settle();
        return waitFor([&] { return view->model()->scatterState().done; });
    }
} // namespace

TEST_CASE("GUI screenshots: histograms", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    Window window(kExamples / "iris.csv");
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 150));
    Fl_Window &histogram = histogramOf(window, 2); // petal_length (cm)
    write(histogram, "gui-histogram.png");
    pointAt(histogram, "[1.4, 1.6)");
    write(histogram, "chart-histogram.png");

    choose(histogram, "Stack by:", 5); // species, after None
    REQUIRE(waitHistogram(view));
    pointAt(histogram, "setosa:");
    write(histogram, "gui-histogram-stacked.png");
    choose(histogram, "Stack by:", 0);
    choose(histogram, "Column:", 4);
    REQUIRE(waitHistogram(view));
    write(histogram, "gui-histogram-text.png");
}

TEST_CASE("GUI screenshots: dates", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    Window window(kExamples / "hour.csv.xz");
    GuiTableView *view = window.currentView();
    REQUIRE(waitFor([&] { return view->model()->columnCount() == 17; }));
    Fl_Window &histogram = histogramOf(window, 1); // dteday
    write(histogram, "gui-histogram-dates.png");
    const HistogramState state = view->model()->histogramState();
    ViewSpec spec = view->model()->requestedView();
    addNarrowingConditions(spec, histogramBinConditions(state.histogram, 1, 0));
    view->model()->setView(spec);
    REQUIRE(waitFor([&] { return view->model()->view() == spec && view->model()->histogramState().done; }));
    REQUIRE(waitHistogram(view));
    window->main_window->resize(window->main_window->x(), window->main_window->y(), 1244, 746);
    writeOver(*window->main_window, histogram, "gui-histogram-filtered.png");
}

TEST_CASE("GUI screenshots: scatter plots", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    {
        Window window(kExamples / "iris.csv");
        GuiTableView *view = window.currentView();
        REQUIRE(window.waitForRows(view, 150));
        Fl_Window &scatter = scatterOf(window, 2, 3);
        write(scatter, "chart-scatter-fit.png");
        write(scatter, "gui-scatter.png");
        choose(scatter, "Color by:", 5); // species
        REQUIRE(waitScatter(view));
        write(scatter, "gui-scatter-colored.png");
        choose(scatter, "Color by:", 0);
        choose(scatter, "X:", 4); // species
        choose(scatter, "Y:", 2); // petal_length
        REQUIRE(waitScatter(view));
        choose(scatter, "Show:", 2);
        pointAt(scatter, "setosa:");
        write(scatter, "chart-boxplot.png");
        write(scatter, "gui-boxplot.png");

        ViewSpec spec = view->model()->requestedView();
        Filter filter;
        filter.conditions.push_back({4, FilterOp::Equals, "setosa"});
        spec.filter = filter;
        view->model()->setView(spec);
        REQUIRE(waitFor([&] { return view->model()->view() == spec && view->model()->rowCount().value == 50; }));
        REQUIRE(waitScatter(view));
        window->main_window->resize(window->main_window->x(), window->main_window->y(), 1244, 746);
        writeOver(*window->main_window, scatter, "chart-boxplot-filtered.png");
    }
    {
        Window window(kExamples / "penguins.csv");
        GuiTableView *view = window.currentView();
        REQUIRE(window.waitForRows(view, 344));
        Fl_Window &scatter = scatterOf(window, 0, 5); // species, body_mass_g
        choose(scatter, "Show:", 2);
        pointAt(scatter, "Gentoo:");
        write(scatter, "chart-boxplot-categories.png");
        choose(scatter, "Show:", 1);
        settle();
        write(scatter, "chart-density-categories.png");
    }
    {
        Window window(kExamples / "hour.csv.xz");
        GuiTableView *view = window.currentView();
        REQUIRE(waitFor([&] { return view->model()->columnCount() == 17; }));
        Fl_Window &scatter = scatterOf(window, 10, 16); // temp, cnt
        choose(scatter, "Show:", 1);
        REQUIRE(waitScatter(view));
        write(scatter, "gui-density.png");
    }
}

TEST_CASE("GUI screenshots: Iris basics and grouping", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    Window window(kExamples / "iris.csv");
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 150));
    window->main_window->resize(window->main_window->x(), window->main_window->y(), 1244, 746);
    for (int c = 0; c < 5; ++c)
        view->col_width(c, 155);
    view->jumpTo(0, 4);
    write(*window->main_window, "gui-main-window.png");
    window->show_content();
    write(*window->main_window, "gui-content-pane.png");
    window->show_search();
    window->search_text->value("setosa");
    window->search_button->do_callback();
    REQUIRE(waitFor([&] { return view->model()->searchState().done; }));
    write(*window->main_window, "gui-search-pane.png");
    window->set_show_sidebar(false);
    ViewSpec sorted;
    sorted.sort.push_back({2, false});
    view->model()->setView(sorted);
    REQUIRE(waitFor([&] { return view->model()->view() == sorted; }));
    write(*window->main_window, "gui-sorted.png");
    view->model()->setView({});
    REQUIRE(waitFor([&] { return view->model()->view().empty(); }));
    window->show_stats(2);
    REQUIRE(waitFor([&] { return view->model()->columnStatsState().done; }));
    write(*window->main_window, "gui-stats-pane.png");
    window->set_show_sidebar(false);
    Filter filter;
    filter.conditions = {{4, FilterOp::Equals, "virginica"}, {2, FilterOp::GreaterEqual, "6"}};
    ViewSpec filtered;
    filtered.filter = filter;
    view->model()->setView(filtered);
    REQUIRE(waitFor([&] { return view->model()->view() == filtered; }));
    window->show_filter();
    write(*window->main_window, "gui-filter-pane.png");
    window->clear_filter();
    window->set_show_sidebar(false);
    view->freezeColumns(2);
    write(*window->main_window, "gui-frozen.png");
    view->freezeColumns(0);

    const auto group = [&](bool values, bool totals, const std::string &dialog_name)
    {
        ModalAnswer answer([&](Fl_Window &dialog)
        {
            int browser = 0;
            visit(dialog, [&](Fl_Widget &widget)
            {
                if (auto *list = dynamic_cast<Fl_Check_Browser *>(&widget))
                {
                    list->check_none();
                    if (browser++ == 0)
                    {
                        if (!totals)
                            list->checked(5, 1); // species
                    }
                    else if (values)
                        list->checked(3, 1); // petal_length
                }
                if (auto *button = dynamic_cast<Fl_Check_Button *>(&widget))
                {
                    const std::string label = button->label() ? button->label() : "";
                    button->value((totals && label != "distinct") || label == "mean" || label == "min" || label == "max");
                }
            });
            if (!dialog_name.empty())
                write(dialog, dialog_name);
            return pressButton(dialog, "Group");
        });
        window->show_group_by(4);
        REQUIRE(answer.answered());
        REQUIRE(waitFor([&] { return window.currentView() != view; }));
        GuiTableView *result = window.currentView();
        REQUIRE(window.waitForRows(result, totals ? 1 : 3));
        REQUIRE(waitFor([&] { return result->cols() == static_cast<int>(result->model()->columnCount()); }));
        for (int c = 0; c < result->cols(); ++c)
            result->col_width(c, c == 0 ? (totals ? 110 : 155) : (c == 1 && !totals ? 90 : 220));
        return result;
    };
    window->main_window->resize(window->main_window->x(), window->main_window->y(), 1100, 280);
    group(false, false, "gui-group-by.png");
    write(*window->main_window, "chart-group-by-count.png");
    window->tab->value(view);
    group(true, false, "chart-group-by-dialog.png");
    GuiTableView *result = window.currentView();
    ColumnFormats formats;
    formats.set(2, {.decimals = 3});
    result->restoreColumnFormats(formats);
    write(*window->main_window, "chart-group-by-mean.png");
    write(*window->main_window, "gui-group-by-result.png");
    window->tab->value(view);
    window->main_window->resize(window->main_window->x(), window->main_window->y(), 1600, 220);
    group(true, true, "");
    write(*window->main_window, "chart-group-by-totals.png");
}

TEST_CASE("GUI screenshots: Iris SQL", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    TempDir temp;
    const auto parquet = temp.path() / "iris.parquet";
    REQUIRE(exportTextFile(kExamples / "iris.csv", parquet, ExportFormat::Parquet, {}).has_value());
    Window window(parquet);
    REQUIRE(window.waitForRows(window.currentView(), 150));
    window->main_window->resize(window->main_window->x(), window->main_window->y(), 1244, 746);
    const std::string query = "SELECT species, count(*) AS flowers,\n"
                              "       round(avg(petal_length), 3) AS mean_petal_length\n"
                              "FROM iris\nGROUP BY species\nORDER BY species";
    window->show_sql();
    Fl_Window *sql = shownWindow("SQL");
    REQUIRE(sql);
    visit(*sql, [&](Fl_Widget &widget)
    {
        if (auto *editor = dynamic_cast<Fl_Text_Editor *>(&widget))
            editor->buffer()->text(query.c_str());
    });
    REQUIRE(pressButton(*sql, "Run"));
    REQUIRE(waitFor([&] { return window.sheetLabels().size() == 2; }));
    REQUIRE(window.waitForRows(window.currentView(), 3));
    for (int c = 0; c < 3; ++c)
        window.currentView()->col_width(c, 220);
    writeOver(*window->main_window, *sql, "gui-sql-result.png");
}

namespace
{
    struct MenuShot
    {
        Fl_Window *main;
        std::string name;
    };

    void captureMenu(void *data)
    {
        auto &shot = *static_cast<MenuShot *>(data);
        Fl::flush();
        Fl_Image_Surface surface(shot.main->w(), shot.main->h());
        surface.draw(shot.main, 0, 0);
        // Menus are real FLTK windows, outside the main widget tree.
        for (Fl_Window *menu = Fl::first_window(); menu; menu = Fl::next_window(menu))
        {
            if (menu != shot.main && menu->shown() && !menu->border())
                surface.draw(menu, menu->x() - shot.main->x(), menu->y() - shot.main->y());
        }
        std::unique_ptr<Fl_RGB_Image> image(surface.image());
        REQUIRE(image);
        CHECK(fl_write_png((outputDirectory() / shot.name).string().c_str(), image.get()) == 0);
        Fl::e_keysym = FL_Escape;
        if (Fl_Window *grab = Fl::grab())
            Fl::handle(FL_KEYBOARD, grab);
    }

    void openFormatMenu(void *data)
    {
        auto &shot = *static_cast<MenuShot *>(data);
        Fl::e_x_root = shot.main->x() + 430;
        Fl::e_y_root = shot.main->y() + 206;
        if (Fl_Window *grab = Fl::grab())
            Fl::handle(FL_MOVE, grab);
    }

    void menuShot(Window &window, Fl_Widget &target, int x, int y, bool right, const std::string &name)
    {
        MenuShot shot{window->main_window, name};
        if (name == "gui-format-menu.png")
            Fl::add_timeout(0.1, openFormatMenu, &shot);
        Fl::add_timeout(0.5, captureMenu, &shot);
        Fl::e_x = x;
        Fl::e_y = y;
        Fl::e_x_root = window->main_window->x() + x;
        Fl::e_y_root = window->main_window->y() + y;
        Fl::e_keysym = FL_Button + (right ? FL_RIGHT_MOUSE : FL_LEFT_MOUSE);
        Fl::e_state = right ? FL_BUTTON3 : FL_BUTTON1;
        target.handle(FL_PUSH);
        Fl::e_state = 0;
        Fl::remove_timeout(captureMenu, &shot);
        Fl::remove_timeout(openFormatMenu, &shot);
    }
}

TEST_CASE("GUI screenshots: Iris menus", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    Window window(kExamples / "iris.csv");
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 150));
    window->main_window->resize(window->main_window->x(), window->main_window->y(), 1244, 746);
    for (int c = 0; c < 5; ++c)
        view->col_width(c, 155);
    settle();
    menuShot(window, *window->menu_bar, 15, 12, false, "gui-menu-file.png");
    menuShot(window, *window->menu_bar, 55, 12, false, "gui-menu-edit.png");
    menuShot(window, *window->menu_bar, 100, 12, false, "gui-menu-view.png");
    menuShot(window, *view, 410, view->y() + 10, true, "gui-header-menu.png");
    menuShot(window, *view, 410, view->y() + 10, true, "gui-format-menu.png");
    menuShot(window, *view, 735, view->y() + 55, true, "gui-cell-menu.png");
    ModalAnswer answer([&](Fl_Window &dialog)
    {
        visit(dialog, [](Fl_Widget &widget)
        {
            if (auto *input = dynamic_cast<Fl_Input *>(&widget))
                input->value("petal_length");
        });
        write(dialog, "gui-goto.png");
        return pressButton(dialog, "Cancel");
    });
    window->go_to();
    REQUIRE(answer.answered());
}

TEST_CASE("GUI screenshots: Iris filter operators", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    Window window(kExamples / "iris.csv");
    auto *view = window.currentView();
    REQUIRE(window.waitForRows(view, 150));
    window->main_window->resize(window->main_window->x(), window->main_window->y(), 1244, 746);
    window->show_filter(4);
    settle();
    Fl_Choice *operators = nullptr;
    visit(*window->filter_pane, [&](Fl_Widget &widget)
    {
        if (auto *choice = dynamic_cast<Fl_Choice *>(&widget))
        {
            if (choice->text(0) && std::string(choice->text(0)) == "contains")
                operators = choice;
        }
    });
    REQUIRE(operators);
    menuShot(window, *operators, operators->x() + 20, operators->y() + 10, false, "gui-filter-ops.png");
}
