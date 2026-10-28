#include "gui_scatter.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <array>
#include <cmath>

#include "cell_format.hxx"
#include "gui_image_export.hxx"
#include "gui_theme.hxx"

namespace
{
    const int WINDOW_WIDTH = 800;
    const int WINDOW_HEIGHT = 560;
    const int MARGIN = 8;
    const int LINE_HEIGHT = 25;
    const int AXIS_LABEL_WIDTH = 24;
    const int CHOICE_WIDTH = 190;
    const int MODE_LABEL_WIDTH = 44;
    const int MODE_WIDTH = 110;
    const int LOG_WIDTH = 140;
    const int FIT_WIDTH = 150;
    const int SAVE_WIDTH = 100;
    // A category's column: the box takes this part of it, the points are
    // spread over it.
    const double BOX_WIDTH = 0.6;
    const double SPREAD_WIDTH = 0.7;
    const double CELL_WIDTH = 0.9;
    enum Mode
    {
        MODE_POINTS,
        MODE_DENSITY,
        MODE_BOX,
    };
    const int FONT_SIZE = 12;
    const int TICK_GAP = 6;
    const int COLOR_BAR_WIDTH = 14;
    // A drag shorter than this is a click.
    const int DRAG_THRESHOLD = 4;
    // How near the mouse a point must be to be pointed at.
    const int POINT_REACH = 6;

    std::array<unsigned char, 3> rgbOf(Fl_Color color)
    {
        unsigned char r, g, b;
        Fl::get_color(color, r, g, b);
        return {r, g, b};
    }

    void setActive(Fl_Widget *widget, bool active)
    {
        if (active)
            widget->activate();
        else
            widget->deactivate();
    }

    // The coordinates an axis spans: its ticks, and the density bins too.
    std::pair<double, double> axisRange(const Histogram &axis, const Histogram &bins)
    {
        double low = axis.edges.empty() ? 0 : axis.edges.front();
        double high = axis.edges.empty() ? 1 : axis.edges.back();
        if (!bins.edges.empty())
        {
            low = std::min(low, bins.edges.front());
            high = std::max(high, bins.edges.back());
        }
        if (!(high > low))
            high = low + 1;
        return {low, high};
    }

    // Where in its category's column a point is drawn, from -0.5 to 0.5:
    // the same for a row every time (a hash of its number).
    double spreadOf(size_t view_row)
    {
        uint64_t h = static_cast<uint64_t>(view_row) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 32;
        return static_cast<double>(h % 10000) / 10000.0 - 0.5;
    }
} // namespace

// The plot: axes with ticks, then points or density cells.
class ScatterWindow::Chart : public Fl_Widget
{
private:
    ScatterWindow *owner_;
    // Layout of the last draw, for finding what lies under the mouse.
    bool laid_out_ = false;
    int plot_x_ = 0, plot_y_ = 0, plot_w_ = 1, plot_h_ = 1;
    double x_low_ = 0, x_high_ = 1, y_low_ = 0, y_high_ = 1;
    // A rectangle being dragged.
    int press_x_ = 0, press_y_ = 0, drag_x_ = 0, drag_y_ = 0;
    bool pressed_ = false;
    bool dragging_ = false;
    // Gridlines of the last draw (pixels), for drawing them under the points.
    std::vector<int> grid_x_, grid_y_;

    const ScatterPlot &plot() const { return owner_->state_.plot; }

    double toX(double value) const { return plot_x_ + (value - x_low_) / (x_high_ - x_low_) * plot_w_; }
    double toY(double value) const { return plot_y_ + plot_h_ - (value - y_low_) / (y_high_ - y_low_) * plot_h_; }
    double fromX(int px) const { return x_low_ + (px - plot_x_) / static_cast<double>(plot_w_) * (x_high_ - x_low_); }
    double fromY(int py) const
    {
        return y_low_ + (plot_y_ + plot_h_ - py) / static_cast<double>(plot_h_) * (y_high_ - y_low_);
    }
    bool inPlot(int px, int py) const
    {
        return laid_out_ && px >= plot_x_ && px < plot_x_ + plot_w_ && py >= plot_y_ && py < plot_y_ + plot_h_;
    }

    void drawMessage(const char *text)
    {
        fl_color(fl_inactive(FL_FOREGROUND_COLOR));
        fl_draw(text, x(), y(), w(), h(), FL_ALIGN_CENTER);
    }

    size_t mostInCell() const
    {
        const auto &counts = plot().counts;
        return counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
    }

    bool logScale() const { return owner_->log_button_->value() != 0; }
    bool categorical() const { return plot().categorical(); }
    bool boxMode() const { return owner_->box_ && categorical(); }
    bool densityMode() const { return owner_->density_; }

    // The X a point is drawn at: spread over its category's column.
    double pointX(const ScatterPoint &point) const
    {
        return categorical() ? point.x + SPREAD_WIDTH * spreadOf(point.view_row) : point.x;
    }

    // The X range of a column of the density grid.
    std::pair<double, double> columnSpan(size_t xi) const
    {
        const ScatterPlot &p = plot();
        if (categorical())
            return {static_cast<double>(xi) - CELL_WIDTH / 2, static_cast<double>(xi) + CELL_WIDTH / 2};
        return {p.x_bins.edges[xi], p.x_bins.edges[xi + 1]};
    }

    std::optional<size_t> categoryAt(int mx, int my) const
    {
        if (!inPlot(mx, my) || !categorical())
            return std::nullopt;
        const double at = std::round(fromX(mx));
        if (at < 0 || at >= static_cast<double>(plot().xBinCount()))
            return std::nullopt;
        return static_cast<size_t>(at);
    }

    void layout()
    {
        const ScatterPlot &p = plot();
        if (categorical())
        {
            x_low_ = -0.5;
            x_high_ = static_cast<double>(p.xBinCount()) - 0.5;
        }
        else
            std::tie(x_low_, x_high_) = axisRange(p.x_axis, p.x_bins);
        std::tie(y_low_, y_high_) = axisRange(p.y_axis, p.y_bins);
        int y_label_width = 0;
        for (const double edge : p.y_axis.edges)
            y_label_width = std::max(y_label_width, static_cast<int>(fl_width(histogramEdgeText(p.y_axis, edge).c_str())));
        int legend_width = 0;
        if (owner_->density_)
        {
            const std::string most = std::to_string(mostInCell());
            legend_width = 2 * MARGIN + COLOR_BAR_WIDTH + TICK_GAP + static_cast<int>(fl_width(most.c_str()));
        }
        const int text_height = fl_height();
        // The axes' titles: the Y one turned, left of its labels; the X one
        // below the labels.
        plot_x_ = x() + MARGIN + text_height + TICK_GAP + y_label_width + TICK_GAP;
        plot_y_ = y() + MARGIN + text_height / 2;
        plot_w_ = std::max(1, x() + w() - MARGIN - legend_width - plot_x_);
        plot_h_ = std::max(1, h() - 2 * MARGIN - text_height / 2 - 2 * text_height - 2 * TICK_GAP);
        laid_out_ = true;
    }

    void drawTitles()
    {
        const auto &model = owner_->model_;
        if (!model)
            return;
        const ScatterState &state = owner_->state_;
        const int text_height = fl_height();
        fl_color(FL_FOREGROUND_COLOR);
        const std::string x_title = model->columnName(state.x_column);
        fl_draw(x_title.c_str(), plot_x_, plot_y_ + plot_h_ + 2 * TICK_GAP + text_height, plot_w_, text_height,
                FL_ALIGN_CENTER | FL_ALIGN_CLIP, nullptr, 0);
        const std::string y_title = model->columnName(state.y_column);
        const int title_width = static_cast<int>(fl_width(y_title.c_str()));
        fl_draw(90, y_title.c_str(), x() + MARGIN + text_height - fl_descent(),
                plot_y_ + plot_h_ / 2 + std::min(title_width, plot_h_) / 2);
    }

    void drawAxes()
    {
        const ScatterPlot &p = plot();
        const int text_height = fl_height();
        const int bottom = plot_y_ + plot_h_;
        const Fl_Color grid = fl_color_average(FL_FOREGROUND_COLOR, FL_BACKGROUND2_COLOR, 0.15f);

        // X: gridlines at every tick, labels as many as fit; categories get
        // a label each, cut to its column.
        grid_x_.clear();
        grid_y_.clear();
        int last_right = x();
        if (categorical())
        {
            const double slot = static_cast<double>(plot_w_) / static_cast<double>(p.xBinCount());
            fl_color(FL_FOREGROUND_COLOR);
            for (size_t i = 0; i < p.xBinCount(); ++i)
            {
                const int px = static_cast<int>(std::lround(toX(static_cast<double>(i))));
                fl_yxline(px, bottom, bottom + 3);
                const std::string text = cellDisplayText(CellValue{scatterCategoryText(p, i)}, 64);
                const int width = std::max(1, static_cast<int>(slot) - 2);
                fl_push_clip(px - width / 2, bottom + TICK_GAP, width, text_height);
                fl_draw(text.c_str(), px - width / 2, bottom + TICK_GAP, width, text_height, FL_ALIGN_CENTER, nullptr, 0);
                fl_pop_clip();
            }
        }
        for (const double edge : categorical() ? std::vector<double>{} : p.x_axis.edges)
        {
            const int px = static_cast<int>(std::lround(toX(edge)));
            if (px < plot_x_ || px > plot_x_ + plot_w_)
                continue;
            grid_x_.push_back(px);
            fl_color(grid);
            fl_yxline(px, plot_y_, bottom);
            const std::string text = histogramEdgeText(p.x_axis, edge);
            const int tw = static_cast<int>(fl_width(text.c_str()));
            const int tx = std::clamp(px - tw / 2, x() + MARGIN, x() + w() - MARGIN - tw);
            if (tx < last_right + TICK_GAP)
                continue;
            fl_color(FL_FOREGROUND_COLOR);
            fl_yxline(px, bottom, bottom + 3);
            fl_draw(text.c_str(), tx, bottom + TICK_GAP + text_height - fl_descent());
            last_right = tx + tw;
        }
        // Y, from the bottom up.
        int last_top = y() + h();
        for (const double edge : p.y_axis.edges)
        {
            const int py = static_cast<int>(std::lround(toY(edge)));
            if (py < plot_y_ || py > bottom)
                continue;
            grid_y_.push_back(py);
            fl_color(grid);
            fl_xyline(plot_x_, py, plot_x_ + plot_w_);
            if (py + text_height / 2 > last_top - 2)
                continue;
            const std::string text = histogramEdgeText(p.y_axis, edge);
            fl_color(FL_FOREGROUND_COLOR);
            fl_xyline(plot_x_ - 3, py, plot_x_);
            fl_draw(text.c_str(), plot_x_ - TICK_GAP - static_cast<int>(fl_width(text.c_str())),
                    py + text_height / 2 - fl_descent());
            last_top = py - text_height / 2;
        }
    }

    void drawFrame()
    {
        fl_color(FL_FOREGROUND_COLOR);
        fl_rect(plot_x_, plot_y_, plot_w_ + 1, plot_h_ + 1);
    }

    void drawPoints()
    {
        const auto &points = plot().points;
        if (!points || points->empty())
            return;
        const int width = plot_w_;
        const int height = plot_h_;
        const auto background = rgbOf(FL_BACKGROUND2_COLOR);
        const auto foreground = rgbOf(FL_SELECTION_COLOR);
        const auto grid = rgbOf(fl_color_average(FL_FOREGROUND_COLOR, FL_BACKGROUND2_COLOR, 0.15f));
        std::vector<unsigned char> image(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
        auto pixelAt = [&](int px, int py)
        { return &image[(static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)) * 3]; };
        for (size_t i = 0; i < image.size(); i += 3)
            std::copy(background.begin(), background.end(), image.begin() + static_cast<std::ptrdiff_t>(i));
        // The image covers the gridlines drawn with the axes.
        for (const int gx : grid_x_)
        {
            for (int py = 0; gx - plot_x_ >= 0 && gx - plot_x_ < width && py < height; ++py)
                std::copy(grid.begin(), grid.end(), pixelAt(gx - plot_x_, py));
        }
        for (const int gy : grid_y_)
        {
            for (int px = 0; gy - plot_y_ >= 0 && gy - plot_y_ < height && px < width; ++px)
                std::copy(grid.begin(), grid.end(), pixelAt(px, gy - plot_y_));
        }
        // Fewer points are drawn larger and darker; many overlap, so the
        // dense areas build up.
        const size_t n = points->size();
        const int radius = n < 2000 ? 2 : n < 50000 ? 1 : 0;
        const double alpha = std::clamp(3000.0 / static_cast<double>(n), 0.15, 0.8);
        for (const ScatterPoint &point : *points)
        {
            const auto cx = static_cast<int>(toX(pointX(point))) - plot_x_;
            const auto cy = static_cast<int>(toY(point.y)) - plot_y_;
            for (int py = std::max(cy - radius, 0); py <= std::min(cy + radius, height - 1); ++py)
            {
                for (int px = std::max(cx - radius, 0); px <= std::min(cx + radius, width - 1); ++px)
                {
                    unsigned char *pixel = pixelAt(px, py);
                    for (size_t k = 0; k < 3; ++k)
                        pixel[k] = static_cast<unsigned char>(std::lround(pixel[k] * (1 - alpha) + foreground[k] * alpha));
                }
            }
        }
        fl_draw_image(image.data(), plot_x_, plot_y_, width, height, 3);
        if (owner_->hovered_point_ && *owner_->hovered_point_ < n)
        {
            const ScatterPoint &point = (*points)[*owner_->hovered_point_];
            fl_color(FL_FOREGROUND_COLOR);
            fl_line_style(FL_SOLID, 2);
            fl_circle(toX(pointX(point)), toY(point.y), 5);
            fl_line_style(0);
        }
    }

    void drawDensity()
    {
        const ScatterPlot &p = plot();
        const size_t most = mostInCell();
        for (size_t xi = 0; xi < p.xBinCount(); ++xi)
        {
            const auto [left, right] = columnSpan(xi);
            const int x0 = static_cast<int>(std::lround(toX(left)));
            const int x1 = static_cast<int>(std::lround(toX(right)));
            for (size_t yi = 0; yi < p.y_bins.binCount(); ++yi)
            {
                const size_t count = p.count(xi, yi);
                if (count == 0)
                    continue;
                const int y0 = static_cast<int>(std::lround(toY(p.y_bins.edges[yi + 1])));
                const int y1 = static_cast<int>(std::lround(toY(p.y_bins.edges[yi])));
                const auto rgb = densityColor(densityScale(count, most, logScale()));
                fl_rectf(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0), rgb[0], rgb[1], rgb[2]);
            }
        }
        if (const auto &cell = owner_->hovered_cell_; cell && cell->first < p.xBinCount() &&
                                                       cell->second < p.y_bins.binCount())
        {
            const auto [left, right] = columnSpan(cell->first);
            const int x0 = static_cast<int>(std::lround(toX(left)));
            const int x1 = static_cast<int>(std::lround(toX(right)));
            const int y0 = static_cast<int>(std::lround(toY(p.y_bins.edges[cell->second + 1])));
            const int y1 = static_cast<int>(std::lround(toY(p.y_bins.edges[cell->second])));
            fl_color(FL_FOREGROUND_COLOR);
            fl_rect(x0 - 1, y0 - 1, std::max(1, x1 - x0) + 2, std::max(1, y1 - y0) + 2);
        }
    }

    // A box per category: Q1 to Q3 with the median, whiskers, the outliers
    // as rings and the mean as a cross.
    void drawBoxes()
    {
        const ScatterPlot &p = plot();
        const double slot = static_cast<double>(plot_w_) / static_cast<double>(std::max<size_t>(p.xBinCount(), 1));
        const int half = std::max(2, static_cast<int>(slot * BOX_WIDTH / 2));
        const auto py = [this](double value)
        { return static_cast<int>(std::lround(toY(value))); };
        for (size_t i = 0; i < p.boxes.size(); ++i)
        {
            const BoxStats &box = p.boxes[i];
            if (box.count == 0)
                continue;
            const int cx = static_cast<int>(std::lround(toX(static_cast<double>(i))));
            const int top = py(box.q3);
            const int bottom = py(box.q1);
            const bool hovered = owner_->hovered_category_ == i;
            fl_color(fl_color_average(FL_SELECTION_COLOR, FL_BACKGROUND2_COLOR, hovered ? 0.7f : 0.35f));
            fl_rectf(cx - half, top, 2 * half + 1, std::max(1, bottom - top + 1));
            fl_color(FL_FOREGROUND_COLOR);
            fl_rect(cx - half, top, 2 * half + 1, std::max(1, bottom - top + 1));
            // Whiskers with caps half as wide as the box.
            fl_yxline(cx, py(box.high_whisker), top);
            fl_yxline(cx, bottom, py(box.low_whisker));
            fl_xyline(cx - half / 2, py(box.high_whisker), cx + half / 2);
            fl_xyline(cx - half / 2, py(box.low_whisker), cx + half / 2);
            fl_line_style(FL_SOLID, 2);
            fl_xyline(cx - half, py(box.median), cx + half);
            fl_line_style(0);
            for (const double value : box.outliers)
                fl_circle(cx, toY(value), 2.5);
            const int my = py(box.mean);
            fl_color(fl_color_average(FL_RED, FL_FOREGROUND_COLOR, 0.8f));
            fl_line_style(FL_SOLID, 2);
            fl_line(cx - 4, my - 4, cx + 4, my + 4);
            fl_line(cx - 4, my + 4, cx + 4, my - 4);
            fl_line_style(0);
        }
    }

    // The colour bar right of the plot, with counts at its ends and middle.
    void drawColorBar()
    {
        const size_t most = mostInCell();
        const int bar_x = plot_x_ + plot_w_ + 2 * MARGIN;
        for (int i = 0; i < plot_h_; ++i)
        {
            const auto rgb = densityColor(1 - static_cast<double>(i) / std::max(plot_h_ - 1, 1));
            fl_color(rgb[0], rgb[1], rgb[2]);
            fl_xyline(bar_x, plot_y_ + i, bar_x + COLOR_BAR_WIDTH - 1);
        }
        fl_color(FL_FOREGROUND_COLOR);
        fl_rect(bar_x, plot_y_, COLOR_BAR_WIDTH, plot_h_);
        const int text_x = bar_x + COLOR_BAR_WIDTH + TICK_GAP;
        const int text_height = fl_height();
        const size_t middle = logScale() ? static_cast<size_t>(std::lround(std::expm1(std::log1p(static_cast<double>(most)) / 2)))
                                         : most / 2;
        fl_draw(std::to_string(most).c_str(), text_x, plot_y_ + text_height / 2 - fl_descent());
        if (middle > 0 && middle < most)
            fl_draw(std::to_string(middle).c_str(), text_x, plot_y_ + plot_h_ / 2 + text_height / 2 - fl_descent());
        fl_draw(logScale() ? "1" : "0", text_x, plot_y_ + plot_h_ + text_height / 2 - fl_descent());
    }

    // The least-squares line across the plot.
    void drawFit()
    {
        if (owner_->fit_button_->value() == 0)
            return;
        const ScatterStats &stats = plot().stats;
        const auto slope = stats.slope();
        const auto intercept = stats.intercept();
        if (!slope || !intercept || !stats.correlation())
            return;
        fl_color(fl_color_average(FL_RED, FL_FOREGROUND_COLOR, 0.8f));
        fl_line_style(FL_SOLID, 2);
        fl_line(static_cast<int>(std::lround(toX(x_low_))), static_cast<int>(std::lround(toY(*intercept + *slope * x_low_))),
                static_cast<int>(std::lround(toX(x_high_))), static_cast<int>(std::lround(toY(*intercept + *slope * x_high_))));
        fl_line_style(0);
    }

    void drawRubberBand()
    {
        if (!dragging_)
            return;
        fl_color(FL_FOREGROUND_COLOR);
        fl_line_style(FL_DASH);
        fl_rect(std::min(press_x_, drag_x_), std::min(press_y_, drag_y_), std::abs(drag_x_ - press_x_) + 1,
                std::abs(drag_y_ - press_y_) + 1);
        fl_line_style(0);
    }

    std::optional<size_t> pointAt(int mx, int my) const
    {
        const auto &points = plot().points;
        if (!points || !inPlot(mx, my))
            return std::nullopt;
        std::optional<size_t> nearest;
        double best = POINT_REACH * POINT_REACH + 1;
        for (size_t i = 0; i < points->size(); ++i)
        {
            const double dx = toX(pointX((*points)[i])) - mx;
            const double dy = toY((*points)[i].y) - my;
            const double distance = dx * dx + dy * dy;
            if (distance < best)
            {
                best = distance;
                nearest = i;
            }
        }
        return nearest;
    }

    std::optional<std::pair<size_t, size_t>> cellAt(int mx, int my) const
    {
        if (!inPlot(mx, my))
            return std::nullopt;
        const ScatterPlot &p = plot();
        const auto xi = categorical() ? categoryAt(mx, my) : histogramBin(p.x_bins.edges, fromX(mx));
        const auto yi = histogramBin(p.y_bins.edges, fromY(my));
        if (!xi || !yi)
            return std::nullopt;
        return std::pair{*xi, *yi};
    }

    void hover(int mx, int my, bool inside)
    {
        std::optional<size_t> point;
        std::optional<std::pair<size_t, size_t>> cell;
        std::optional<size_t> category;
        if (inside && !dragging_)
        {
            if (densityMode())
                cell = cellAt(mx, my);
            else if (boxMode())
                category = categoryAt(mx, my);
            else
                point = pointAt(mx, my);
        }
        if (point != owner_->hovered_point_ || cell != owner_->hovered_cell_ || category != owner_->hovered_category_)
        {
            owner_->hovered_point_ = point;
            owner_->hovered_cell_ = cell;
            owner_->hovered_category_ = category;
            owner_->showDetail();
            redraw();
        }
    }

public:
    Chart(int x, int y, int w, int h, ScatterWindow *owner) : Fl_Widget(x, y, w, h), owner_(owner) {}

    void draw() override
    {
        fl_push_clip(x(), y(), w(), h());
        fl_color(FL_BACKGROUND2_COLOR);
        fl_rectf(x(), y(), w(), h());
        fl_font(FL_HELVETICA, FONT_SIZE);
        laid_out_ = false;
        const ScatterState &state = owner_->state_;
        if (!state.active)
            drawMessage("");
        else if (!plot().plottable())
            drawMessage(state.done ? "Nothing to plot" : "Reading the columns…");
        else
        {
            layout();
            drawAxes();
            drawTitles();
            fl_push_clip(plot_x_, plot_y_, plot_w_, plot_h_);
            if (densityMode())
                drawDensity();
            else if (boxMode())
                drawBoxes();
            else
                drawPoints();
            drawFit();
            fl_pop_clip();
            drawFrame();
            if (owner_->density_)
                drawColorBar();
            drawRubberBand();
        }
        fl_pop_clip();
    }

    int handle(int event) override
    {
        switch (event)
        {
        case FL_ENTER:
            return 1;
        case FL_MOVE:
        case FL_LEAVE:
            hover(Fl::event_x(), Fl::event_y(), event == FL_MOVE);
            return 1;
        case FL_PUSH:
            if (Fl::event_button() != FL_LEFT_MOUSE || !inPlot(Fl::event_x(), Fl::event_y()))
                return 1;
            if (densityMode() && Fl::event_clicks() > 0)
            {
                // Double-click: the cell's rows.
                pressed_ = false;
                if (const auto cell = cellAt(Fl::event_x(), Fl::event_y()))
                    owner_->filterCell(cell->first, cell->second);
                return 1;
            }
            if (boxMode() && Fl::event_clicks() > 0)
            {
                // Double-click: the category's rows.
                pressed_ = false;
                if (const auto category = categoryAt(Fl::event_x(), Fl::event_y()))
                    owner_->filterCategory(*category);
                return 1;
            }
            pressed_ = true;
            dragging_ = false;
            press_x_ = drag_x_ = Fl::event_x();
            press_y_ = drag_y_ = Fl::event_y();
            return 1;
        case FL_DRAG:
            if (!pressed_)
                return 1;
            drag_x_ = std::clamp(Fl::event_x(), plot_x_, plot_x_ + plot_w_ - 1);
            drag_y_ = std::clamp(Fl::event_y(), plot_y_, plot_y_ + plot_h_ - 1);
            // Categories have no range to drag over.
            if (!dragging_ && !categorical() &&
                (std::abs(drag_x_ - press_x_) > DRAG_THRESHOLD || std::abs(drag_y_ - press_y_) > DRAG_THRESHOLD))
            {
                dragging_ = true;
                hover(0, 0, false);
            }
            if (dragging_)
                redraw();
            return 1;
        case FL_RELEASE:
            if (!pressed_)
                return 1;
            pressed_ = false;
            if (dragging_)
            {
                dragging_ = false;
                redraw();
                owner_->filterRange(fromX(std::min(press_x_, drag_x_)), fromX(std::max(press_x_, drag_x_)),
                                    fromY(std::max(press_y_, drag_y_)), fromY(std::min(press_y_, drag_y_)));
            }
            else if (!densityMode() && !boxMode())
            {
                if (const auto point = pointAt(Fl::event_x(), Fl::event_y()))
                    owner_->goToPoint(*point);
            }
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }
};

ScatterWindow::ScatterWindow()
{
    window_ = std::make_unique<Fl_Double_Window>(WINDOW_WIDTH, WINDOW_HEIGHT, "Scatter Plot");
    int x = MARGIN + AXIS_LABEL_WIDTH;
    x_choice_ = new Fl_Choice(x, MARGIN, CHOICE_WIDTH, LINE_HEIGHT, "X:");
    x_choice_->callback(columnCallback, this);
    x += CHOICE_WIDTH + MARGIN + AXIS_LABEL_WIDTH;
    y_choice_ = new Fl_Choice(x, MARGIN, CHOICE_WIDTH, LINE_HEIGHT, "Y:");
    y_choice_->callback(columnCallback, this);
    x += CHOICE_WIDTH + MARGIN + MODE_LABEL_WIDTH;
    mode_choice_ = new Fl_Choice(x, MARGIN, MODE_WIDTH, LINE_HEIGHT, "Show:");
    mode_choice_->add("Points");
    mode_choice_->add("Density");
    mode_choice_->add("Box Plot");
    mode_choice_->value(MODE_POINTS);
    mode_choice_->callback(modeCallback, this);
    x += MODE_WIDTH + MARGIN;
    log_button_ = new Fl_Check_Button(x, MARGIN, LOG_WIDTH, LINE_HEIGHT, "Log color scale");
    log_button_->value(1);
    log_button_->callback(modeCallback, this);
    const int status_y = MARGIN + LINE_HEIGHT;
    status_ = new Fl_Box(MARGIN, status_y, WINDOW_WIDTH - 2 * MARGIN, LINE_HEIGHT);
    status_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    const int stats_y = status_y + LINE_HEIGHT;
    const int save_x = WINDOW_WIDTH - MARGIN - SAVE_WIDTH;
    const int fit_x = save_x - MARGIN - FIT_WIDTH;
    stats_ = new Fl_Box(MARGIN, stats_y, fit_x - 2 * MARGIN, LINE_HEIGHT);
    stats_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    save_button_ = new Fl_Button(save_x, stats_y, SAVE_WIDTH, LINE_HEIGHT, "Save Image...");
    save_button_->callback(saveCallback, this);
    fit_button_ = new Fl_Check_Button(fit_x, stats_y, FIT_WIDTH, LINE_HEIGHT, "Regression line");
    fit_button_->value(1);
    fit_button_->callback([](Fl_Widget *, void *data) { static_cast<ScatterWindow *>(data)->chart_->redraw(); }, this);
    const int chart_y = stats_y + LINE_HEIGHT;
    const int detail_y = WINDOW_HEIGHT - MARGIN - LINE_HEIGHT;
    chart_ = new Chart(MARGIN, chart_y, WINDOW_WIDTH - 2 * MARGIN, detail_y - MARGIN - chart_y, this);
    chart_->box(FL_DOWN_FRAME);
    detail_ = new Fl_Box(MARGIN, detail_y, WINDOW_WIDTH - 2 * MARGIN, LINE_HEIGHT);
    detail_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    window_->resizable(chart_);
    window_->size_range(480, 320);
    window_->callback(closeCallback, this);
    window_->end();
    setActive(log_button_, false);
}

ScatterWindow::~ScatterWindow()
{
    Fl::remove_timeout(deferredUpdate, this);
    if (model_)
        model_->cancelScatter();
    // The choices read their menus (column_menu_) while the window hides.
    window_.reset();
}

void ScatterWindow::show(std::shared_ptr<TableModel> model, const std::string &sheet_name, size_t x_column,
                         size_t y_column)
{
    if (model_ && model_ != model)
        model_->cancelScatter();
    model_ = std::move(model);
    setTitle(sheet_name);
    window_->show();
    start(x_column, y_column);
}

void ScatterWindow::setModel(std::shared_ptr<TableModel> model, const std::string &sheet_name)
{
    if (model == model_)
        return;
    if (model_)
        model_->cancelScatter();
    model_ = std::move(model);
    setTitle(sheet_name);
    if (window_->shown() && model_)
    {
        start(x_column_, y_column_);
        // The sheet switched to may not have those columns.
        check_columns_ = true;
    }
    else
        update();
}

bool ScatterWindow::shown() const
{
    return window_->shown();
}

void ScatterWindow::setTitle(const std::string &sheet_name)
{
    title_ = "Scatter Plot - " + sheet_name;
    window_->copy_label(title_.c_str());
}

void ScatterWindow::start(size_t x_column, size_t y_column)
{
    x_column_ = x_column;
    y_column_ = y_column;
    hovered_point_.reset();
    hovered_cell_.reset();
    hovered_category_.reset();
    if (model_)
        model_->startScatter(x_column, y_column);
    update();
}

void ScatterWindow::deferredUpdate(void *data)
{
    static_cast<ScatterWindow *>(data)->update();
}

void ScatterWindow::update()
{
    // An open menu (which holds the grab) may be a column choice showing column_menu_.
    if (Fl::grab())
    {
        Fl::remove_timeout(deferredUpdate, this);
        Fl::add_timeout(0.1, deferredUpdate, this);
        return;
    }
    ScatterState state = model_ ? model_->scatterState() : ScatterState{};
    if (state.generation != state_.generation)
    {
        hovered_point_.reset();
        hovered_cell_.reset();
        hovered_category_.reset();
    }
    state_ = std::move(state);
    // Text on X is best seen as box plots, numbers as points; once the
    // layout is known.
    if ((state_.counting || state_.done) && state_.plot.categorical() != shown_categorical_)
    {
        shown_categorical_ = state_.plot.categorical();
        if (shown_categorical_ && !density_)
            setMode(MODE_BOX);
        else if (!shown_categorical_ && box_)
            setMode(MODE_POINTS);
    }
    const int box_flags = mode_choice_->mode(MODE_BOX);
    mode_choice_->mode(MODE_BOX, shown_categorical_ ? box_flags & ~FL_MENU_INACTIVE : box_flags | FL_MENU_INACTIVE);

    rebuildColumnMenu();
    const size_t columns = column_labels_.size();
    if (check_columns_ && model_ && model_->state() != ModelState::Opening)
    {
        check_columns_ = false;
        if (state_.active && (state_.x_column >= columns || state_.y_column >= columns))
        {
            model_->cancelScatter(); // "Choose the columns." instead of an empty plot
            state_ = model_->scatterState();
        }
    }
    const bool shown = state_.active && state_.x_column < columns && state_.y_column < columns;
    x_choice_->value(shown ? static_cast<int>(state_.x_column) : -1);
    y_choice_->value(shown ? static_cast<int>(state_.y_column) : -1);
    setActive(x_choice_, model_ && columns > 0);
    setActive(y_choice_, model_ && columns > 0);
    setActive(log_button_, density_);
    setActive(fit_button_, !state_.plot.categorical());
    setActive(save_button_, state_.active && state_.plot.plottable());

    std::string text;
    if (!model_ || model_->state() == ModelState::Error)
        text.clear();
    else if (!state_.active)
        text = columns == 0 ? "" : "Choose the columns.";
    else if (!state_.done)
        text = (state_.counting ? "Counting… " : "Reading… ") + std::to_string(state_.scanned_rows) + " rows";
    else
        text = scatterSummary(state_.plot, !density_) + (model_->view().filtered() ? " (filtered rows)" : "");
    text = escapeLabel(text);
    if (!status_->label() || text != status_->label())
    {
        status_->copy_label(text.c_str());
        status_->redraw();
    }
    // Of the rows counted so far while counting.
    std::string stats = state_.active && state_.counting ? scatterStatsText(state_.plot) : std::string();
    stats = escapeLabel(stats);
    if (!stats_->label() || stats != stats_->label())
    {
        stats_->copy_label(stats.c_str());
        stats_->redraw();
    }
    showDetail();
    chart_->redraw();
}

void ScatterWindow::showDetail()
{
    const ScatterPlot &plot = state_.plot;
    std::string text;
    if (hovered_point_ && plot.points && *hovered_point_ < plot.points->size())
    {
        const ScatterPoint &point = (*plot.points)[*hovered_point_];
        const std::string x = plot.categorical() ? scatterCategoryText(plot, static_cast<size_t>(point.x))
                                                 : scatterValueText(plot.x_axis, point.x);
        text = "x = " + x + ", y = " + scatterValueText(plot.y_axis, point.y) + ", row " +
               std::to_string(point.view_row + 1) + "  Click to go to the row.";
    }
    else if (hovered_category_)
    {
        text = boxPlotText(plot, *hovered_category_);
        if (state_.done && *hovered_category_ < plot.categories.size())
            text += "  Double-click to filter.";
    }
    else if (hovered_cell_)
        text = densityCellText(plot, hovered_cell_->first, hovered_cell_->second) +
               (state_.done ? "  Double-click to filter." : "");
    else if (plot.plottable() && plot.categorical())
        text = density_ ? "Point at a cell for its value, range and count; double-click it to filter."
               : box_   ? "Point at a box for its quartiles; double-click it to filter to its value."
                        : "Point at a point for its values; click it to go to its row.";
    else if (plot.plottable())
        text = density_ ? "Point at a cell for its ranges and count; double-click it or drag a rectangle to filter."
                        : "Point at a point for its values; click it to go to its row; drag a rectangle to filter.";
    text = escapeLabel(text);
    if (!detail_->label() || text != detail_->label())
    {
        detail_->copy_label(text.c_str());
        detail_->redraw();
    }
}

void ScatterWindow::goToPoint(size_t index)
{
    const auto &points = state_.plot.points;
    if (!model_ || !points || index >= points->size() || !row_callback_)
        return;
    row_callback_(model_, (*points)[index].view_row, state_.x_column);
}

void ScatterWindow::filterCell(size_t x_bin, size_t y_bin)
{
    if (!state_.done || x_bin >= state_.plot.xBinCount() || y_bin >= state_.plot.y_bins.binCount())
        return;
    addFilterConditions(densityCellConditions(state_.plot, state_.x_column, state_.y_column, x_bin, y_bin));
}

void ScatterWindow::filterCategory(size_t category)
{
    if (state_.done)
        addFilterConditions(scatterCategoryConditions(state_.plot, state_.x_column, state_.y_column, category));
}

void ScatterWindow::filterRange(double x_low, double x_high, double y_low, double y_high)
{
    addFilterConditions(
        scatterRangeConditions(state_.plot, state_.x_column, state_.y_column, x_low, x_high, y_low, y_high));
}

void ScatterWindow::addFilterConditions(std::vector<FilterCondition> conditions)
{
    if (!model_ || conditions.empty() || !model_->viewCapabilities().filter)
        return;
    ViewSpec spec = model_->requestedView();
    addNarrowingConditions(spec, std::move(conditions));
    // The plot then starts over on the rows left.
    model_->setView(std::move(spec));
}

void ScatterWindow::rebuildColumnMenu()
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
    x_choice_->menu(column_menu_.data());
    y_choice_->menu(column_menu_.data());
}

void ScatterWindow::columnCallback(Fl_Widget *, void *data)
{
    auto *self = static_cast<ScatterWindow *>(data);
    const int x = self->x_choice_->value();
    const int y = self->y_choice_->value();
    self->start(x >= 0 ? static_cast<size_t>(x) : self->x_column_, y >= 0 ? static_cast<size_t>(y) : self->y_column_);
}

void ScatterWindow::setMode(int mode)
{
    mode_choice_->value(mode);
    density_ = mode == MODE_DENSITY;
    box_ = mode == MODE_BOX;
    hovered_point_.reset();
    hovered_cell_.reset();
    hovered_category_.reset();
}

void ScatterWindow::modeCallback(Fl_Widget *, void *data)
{
    auto *self = static_cast<ScatterWindow *>(data);
    self->setMode(self->mode_choice_->value());
    self->update();
}

void ScatterWindow::saveCallback(Fl_Widget *, void *data)
{
    auto *self = static_cast<ScatterWindow *>(data);
    if (!self->model_ || !self->state_.active)
        return;
    // Without what is pointed at.
    self->hovered_point_.reset();
    self->hovered_cell_.reset();
    self->hovered_category_.reset();
    self->showDetail();
    const char *kind = self->density_ ? "density" : self->box_ ? "boxplot" : "scatter";
    saveWidgetImage(self->chart_, imageFileName(std::string(kind) + "-" + self->model_->columnName(self->state_.x_column) +
                                                "-" + self->model_->columnName(self->state_.y_column)));
}

void ScatterWindow::closeCallback(Fl_Widget *, void *data)
{
    auto *self = static_cast<ScatterWindow *>(data);
    self->window_->hide();
    // Nothing shows it any more.
    if (self->model_)
        self->model_->cancelScatter();
}
