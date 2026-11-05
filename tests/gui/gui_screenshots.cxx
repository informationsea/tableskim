// The manual's screenshots of the GUI's chart windows (docs/images/gui-*.png
// and chart-*.png), taken from the windows as they are. Hidden: run on
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
#include <FL/Fl_Group.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include "cell_format.hxx"
#include "gui_image_export.hxx"
#include "gui_test_helpers.hxx"
#include "gui_window_menu.hxx"

using namespace gui_test;

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

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
    Window window(kDemoData / "normal.csv");
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 54));

    Fl_Window &histogram = histogramOf(window, 6); // Salary
    write(histogram, "gui-histogram.png");

    choose(histogram, "Column:", 7); // Birthday
    REQUIRE(waitHistogram(view));
    write(histogram, "gui-histogram-dates.png");

    choose(histogram, "Column:", 4); // City
    REQUIRE(waitHistogram(view));
    write(histogram, "gui-histogram-text.png");

    // Age stacked by City, a part of a bar pointed at.
    choose(histogram, "Column:", 3);
    choose(histogram, "Stack by:", 5);
    REQUIRE(waitHistogram(view));
    pointAt(histogram, "Sendai: 3 rows");
    write(histogram, "gui-histogram-stacked.png");
    choose(histogram, "Stack by:", 0);

    // Birthday filtered to its 1994-1995 bar, as a double-click does.
    choose(histogram, "Column:", 7);
    REQUIRE(waitHistogram(view));
    const HistogramState state = view->model()->histogramState();
    std::optional<size_t> bar;
    for (size_t bin = 0; bin < state.histogram.binCount(); ++bin)
    {
        if (histogramBinLabel(state.histogram, bin) == "1994")
            bar = bin;
    }
    REQUIRE(bar);
    ViewSpec spec = view->model()->requestedView();
    addNarrowingConditions(spec, histogramBinConditions(state.histogram, 7, *bar));
    view->model()->setView(spec);
    REQUIRE(waitFor([&] { return view->model()->view() == spec && view->model()->histogramState().done; }));
    REQUIRE(waitHistogram(view));
    window->main_window->resize(window->main_window->x(), window->main_window->y(), 1244, 746);
    view->set_selection(0, 7, 0, 7);
    writeOver(*window->main_window, histogram, "gui-histogram-filtered.png");
}

TEST_CASE("GUI screenshots: the charts guide's histogram", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    Window window(kDemoData / "large.parquet");
    GuiTableView *view = window.currentView();
    REQUIRE(window.waitForRows(view, 10000));
    Fl_Window &histogram = histogramOf(window, 4); // Score
    pointAt(histogram, "[45, 50)");
    write(histogram, "chart-histogram.png");
}

TEST_CASE("GUI screenshots: scatter plots", "[.screenshots]")
{
    if (outputDirectory().empty())
        SKIP("TABLESKIM_SCREENSHOT_DIR is not set");
    {
        Window window(kDemoData / "large.parquet");
        GuiTableView *view = window.currentView();
        REQUIRE(window.waitForRows(view, 10000));
        Fl_Window &scatter = scatterOf(window, 3, 4); // Value, Score
        write(scatter, "gui-scatter.png");

        choose(scatter, "Show:", 1); // Density
        REQUIRE(waitScatter(view));
        pointAt(scatter, "x [-30000, -25000) × y [60, 65)");
        write(scatter, "gui-density.png");

        // Category on X: box plots, and the density of each category.
        choose(scatter, "X:", 2);
        REQUIRE(waitScatter(view));
        choose(scatter, "Show:", 2); // Box Plot
        pointAt(scatter, "gamma:");
        write(scatter, "chart-boxplot-categories.png");
        choose(scatter, "Show:", 1);
        settle();
        write(scatter, "chart-density-categories.png");
    }
    {
        Window window(kDemoData / "normal.csv");
        GuiTableView *view = window.currentView();
        REQUIRE(window.waitForRows(view, 54));
        Fl_Window &scatter = scatterOf(window, 3, 6); // Age, Salary
        write(scatter, "chart-scatter-fit.png");

        choose(scatter, "Color by:", 5); // City
        REQUIRE(waitScatter(view));
        write(scatter, "gui-scatter-colored.png");
        choose(scatter, "Color by:", 0);
        REQUIRE(waitScatter(view));

        choose(scatter, "X:", 5); // Department
        REQUIRE(waitScatter(view));
        choose(scatter, "Show:", 2);
        pointAt(scatter, "Sales:");
        write(scatter, "chart-boxplot.png");

        choose(scatter, "X:", 4); // City
        choose(scatter, "Y:", 3); // Age
        REQUIRE(waitScatter(view));
        choose(scatter, "Show:", 2);
        settle();
        write(scatter, "gui-boxplot.png");
    }
}
