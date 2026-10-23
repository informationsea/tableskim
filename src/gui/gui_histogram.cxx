#include "gui_histogram.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>

#include "cell_format.hxx"
#include "gui_table_view.hxx"
#include "gui_theme.hxx"

namespace
{
    const int WINDOW_WIDTH = 640;
    const int WINDOW_HEIGHT = 420;
    const int MARGIN = 8;
    const int LINE_HEIGHT = 25;
    const int CHOICE_LABEL_WIDTH = 60;
    const int CHOICE_WIDTH = 240;
    const int FONT_SIZE = 12;
    const int TICK_GAP = 6;
    // Rows of the most frequent values shown, at most.
    const size_t MAX_VALUE_ROWS = 100;
    // Bytes of a value shown as a row label.
    const size_t VALUE_LABEL_BYTES = 60;

    // A round step for count gridlines: 1, 2 or 5 times a power of ten.
    double gridStep(double raw)
    {
        if (raw <= 1)
            return 1;
        const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
        for (const double m : {1.0, 2.0, 5.0})
        {
            if (m * magnitude >= raw)
                return m * magnitude;
        }
        return 10 * magnitude;
    }

    void setActive(Fl_Widget *widget, bool active)
    {
        if (active)
            widget->activate();
        else
            widget->deactivate();
    }
} // namespace

// Bars of the owner's histogram: vertical ones for bins, horizontal ones for
// the most frequent values.
class HistogramWindow::Chart : public Fl_Widget
{
private:
    HistogramWindow *owner_;
    // Layout of the last draw, for finding the bar under the mouse.
    int plot_x_ = 0, plot_y_ = 0, plot_w_ = 0, plot_h_ = 0;
    size_t bars_ = 0;
    bool rows_ = false; // horizontal bars, one per row
    int row_height_ = 0;

    const Histogram &histogram() const { return owner_->state_.histogram; }

    std::optional<size_t> barAt(int mx, int my) const
    {
        if (bars_ == 0 || mx < plot_x_ || mx >= plot_x_ + plot_w_ || my < plot_y_)
            return std::nullopt;
        size_t index;
        if (rows_)
            index = static_cast<size_t>((my - plot_y_) / std::max(row_height_, 1));
        else
        {
            if (my > plot_y_ + plot_h_)
                return std::nullopt;
            index = static_cast<size_t>(static_cast<double>(mx - plot_x_) * static_cast<double>(bars_) / plot_w_);
        }
        if (index >= bars_)
            return std::nullopt;
        return index;
    }

    Fl_Color barColor(size_t index) const
    {
        return owner_->hovered_ == index ? fl_color_average(FL_SELECTION_COLOR, FL_FOREGROUND_COLOR, 0.6f)
                                         : FL_SELECTION_COLOR;
    }

    void drawMessage(const char *text)
    {
        fl_color(fl_inactive(FL_FOREGROUND_COLOR));
        fl_draw(text, x(), y(), w(), h(), FL_ALIGN_CENTER);
    }

    void drawBins()
    {
        const Histogram &hg = histogram();
        const size_t bins = hg.binCount();
        const size_t most = std::max<size_t>(1, *std::max_element(hg.counts.begin(), hg.counts.end()));
        const double step = gridStep(static_cast<double>(most) / 4);
        const double top = std::ceil(static_cast<double>(most) / step) * step;
        const int text_height = fl_height();
        const int axis_width = static_cast<int>(fl_width(std::to_string(static_cast<size_t>(top)).c_str())) + TICK_GAP;

        plot_x_ = x() + MARGIN + axis_width;
        plot_y_ = y() + MARGIN + text_height / 2;
        plot_w_ = std::max(1, w() - 2 * MARGIN - axis_width);
        plot_h_ = std::max(1, h() - 2 * MARGIN - text_height / 2 - text_height - TICK_GAP);
        bars_ = bins;
        rows_ = false;
        const int bottom = plot_y_ + plot_h_;

        // Gridlines with counts.
        const Fl_Color grid = fl_color_average(FL_FOREGROUND_COLOR, FL_BACKGROUND2_COLOR, 0.15f);
        for (double v = 0; v <= top + step / 2; v += step)
        {
            const int yy = bottom - static_cast<int>(std::lround(v / top * plot_h_));
            fl_color(grid);
            fl_line(plot_x_, yy, plot_x_ + plot_w_, yy);
            const std::string label = std::to_string(static_cast<size_t>(v));
            fl_color(FL_FOREGROUND_COLOR);
            fl_draw(label.c_str(), plot_x_ - TICK_GAP - static_cast<int>(fl_width(label.c_str())),
                    yy + text_height / 2 - fl_descent());
        }

        const double width = static_cast<double>(plot_w_) / static_cast<double>(bins);
        for (size_t i = 0; i < bins; ++i)
        {
            const int x0 = plot_x_ + static_cast<int>(std::lround(static_cast<double>(i) * width));
            const int x1 = plot_x_ + static_cast<int>(std::lround(static_cast<double>(i + 1) * width));
            const int gap = x1 - x0 > 4 ? 1 : 0;
            const int height = static_cast<int>(std::lround(static_cast<double>(hg.counts[i]) / top * plot_h_));
            if (height <= 0)
                continue;
            fl_color(barColor(i));
            fl_rectf(x0 + gap, bottom - height, std::max(1, x1 - x0 - 2 * gap), height);
        }
        fl_color(FL_FOREGROUND_COLOR);
        fl_line(plot_x_, bottom, plot_x_ + plot_w_, bottom);

        // Labels: under each bar for single integers, else at the edges,
        // as many as fit.
        const bool centered = hg.kind == HistogramKind::Number && hg.integers && hg.edges[1] - hg.edges[0] == 1;
        const size_t labels = centered ? bins : bins + 1;
        auto label = [&](size_t i)
        { return centered ? histogramBinLabel(hg, i) : histogramEdgeText(hg, hg.edges[i]); };
        int widest = 0;
        for (size_t i = 0; i < labels; ++i)
            widest = std::max(widest, static_cast<int>(fl_width(label(i).c_str())));
        const size_t every = std::max<size_t>(1, static_cast<size_t>(std::ceil((widest + 2 * TICK_GAP) / width)));
        int last_right = x();
        for (size_t i = 0; i < labels; i += every)
        {
            const std::string text = label(i);
            const double at = centered ? (static_cast<double>(i) + 0.5) * width : static_cast<double>(i) * width;
            const int tw = static_cast<int>(fl_width(text.c_str()));
            const int tx = std::clamp(plot_x_ + static_cast<int>(at) - tw / 2, x() + MARGIN, x() + w() - MARGIN - tw);
            // The last one, pushed inside the edge, may run into the one before.
            if (tx < last_right + TICK_GAP)
                continue;
            if (!centered)
                fl_line(plot_x_ + static_cast<int>(at), bottom, plot_x_ + static_cast<int>(at), bottom + 3);
            fl_draw(text.c_str(), tx, bottom + TICK_GAP + text_height - fl_descent());
            last_right = tx + tw;
        }
    }

    void drawValues()
    {
        const Histogram &hg = histogram();
        const int text_height = fl_height();
        row_height_ = text_height + 6;
        const size_t fit = static_cast<size_t>(std::max(0, (h() - 2 * MARGIN) / row_height_));
        const size_t rows = std::min({hg.top.size(), fit, MAX_VALUE_ROWS});
        std::vector<std::string> labels;
        int label_width = 0;
        for (size_t i = 0; i < rows; ++i)
        {
            labels.push_back(cellDisplayText(hg.top[i].value, VALUE_LABEL_BYTES));
            label_width = std::max(label_width, static_cast<int>(fl_width(labels.back().c_str())));
        }
        label_width = std::min(label_width, w() / 3);
        const int count_width = static_cast<int>(fl_width(std::to_string(hg.top.empty() ? 0 : hg.top[0].count).c_str()));
        plot_x_ = x() + MARGIN + label_width + TICK_GAP;
        plot_y_ = y() + MARGIN;
        plot_w_ = std::max(1, w() - 2 * MARGIN - label_width - 2 * TICK_GAP - count_width);
        plot_h_ = static_cast<int>(rows) * row_height_;
        bars_ = rows;
        rows_ = true;
        const double most = hg.top.empty() ? 1.0 : static_cast<double>(std::max<size_t>(1, hg.top[0].count));
        for (size_t i = 0; i < rows; ++i)
        {
            const int yy = plot_y_ + static_cast<int>(i) * row_height_;
            fl_color(FL_FOREGROUND_COLOR);
            fl_push_clip(x() + MARGIN, yy, label_width, row_height_);
            fl_draw(labels[i].c_str(), x() + MARGIN, yy, label_width, row_height_, FL_ALIGN_LEFT);
            fl_pop_clip();
            const int length = std::max(1, static_cast<int>(std::lround(static_cast<double>(hg.top[i].count) / most * plot_w_)));
            fl_color(barColor(i));
            fl_rectf(plot_x_, yy + 2, length, row_height_ - 4);
            fl_color(FL_FOREGROUND_COLOR);
            const std::string count = std::to_string(hg.top[i].count);
            fl_draw(count.c_str(), plot_x_ + length + TICK_GAP, yy + (row_height_ + text_height) / 2 - fl_descent());
        }
    }

public:
    Chart(int x, int y, int w, int h, HistogramWindow *owner) : Fl_Widget(x, y, w, h), owner_(owner) {}

    void draw() override
    {
        fl_push_clip(x(), y(), w(), h());
        fl_color(FL_BACKGROUND2_COLOR);
        fl_rectf(x(), y(), w(), h());
        fl_font(FL_HELVETICA, FONT_SIZE);
        bars_ = 0;
        const HistogramState &state = owner_->state_;
        const Histogram &hg = state.histogram;
        if (!state.active)
            drawMessage("");
        else if (hg.binCount() > 0 && state.counting)
            drawBins();
        else if (state.done && !hg.top.empty())
            drawValues();
        else if (state.done)
            drawMessage("No values");
        else
            drawMessage("Reading the column…");
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
        {
            const auto bar = event == FL_MOVE ? barAt(Fl::event_x(), Fl::event_y()) : std::nullopt;
            if (bar != owner_->hovered_)
            {
                owner_->hovered_ = bar;
                owner_->showDetail();
                redraw();
            }
            return 1;
        }
        case FL_PUSH:
            if (Fl::event_button() == FL_LEFT_MOUSE && Fl::event_clicks() > 0)
            {
                if (const auto bar = barAt(Fl::event_x(), Fl::event_y()))
                    owner_->filterBy(*bar);
            }
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }
};

HistogramWindow::HistogramWindow()
{
    window_ = std::make_unique<Fl_Double_Window>(WINDOW_WIDTH, WINDOW_HEIGHT, "Histogram");
    column_choice_ = new Fl_Choice(MARGIN + CHOICE_LABEL_WIDTH, MARGIN, CHOICE_WIDTH, LINE_HEIGHT, "Column:");
    column_choice_->callback(columnCallback, this);
    const int status_x = MARGIN + CHOICE_LABEL_WIDTH + CHOICE_WIDTH + MARGIN;
    status_ = new Fl_Box(status_x, MARGIN, WINDOW_WIDTH - MARGIN - status_x, LINE_HEIGHT);
    status_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    const int chart_y = 2 * MARGIN + LINE_HEIGHT;
    const int detail_y = WINDOW_HEIGHT - MARGIN - LINE_HEIGHT;
    chart_ = new Chart(MARGIN, chart_y, WINDOW_WIDTH - 2 * MARGIN, detail_y - MARGIN - chart_y, this);
    chart_->box(FL_DOWN_FRAME);
    detail_ = new Fl_Box(MARGIN, detail_y, WINDOW_WIDTH - 2 * MARGIN, LINE_HEIGHT);
    detail_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    window_->resizable(chart_);
    window_->size_range(360, 240);
    window_->callback(closeCallback, this);
    window_->end();
}

HistogramWindow::~HistogramWindow()
{
    Fl::remove_timeout(deferredUpdate, this);
    if (model_)
        model_->cancelHistogram();
}

void HistogramWindow::show(std::shared_ptr<TableModel> model, const std::string &sheet_name, size_t column)
{
    if (model_ && model_ != model)
        model_->cancelHistogram();
    model_ = std::move(model);
    setTitle(sheet_name);
    window_->show();
    start(column);
}

void HistogramWindow::setModel(std::shared_ptr<TableModel> model, const std::string &sheet_name)
{
    if (model == model_)
        return;
    if (model_)
        model_->cancelHistogram();
    model_ = std::move(model);
    setTitle(sheet_name);
    if (window_->shown() && model_)
    {
        start(column_);
        // The sheet switched to may not have that column.
        check_column_ = true;
    }
    else
        update();
}

bool HistogramWindow::shown() const
{
    return window_->shown();
}

void HistogramWindow::setTitle(const std::string &sheet_name)
{
    title_ = "Histogram - " + sheet_name;
    window_->copy_label(title_.c_str());
}

void HistogramWindow::start(size_t column)
{
    column_ = column;
    hovered_.reset();
    if (model_)
        model_->startHistogram(column);
    update();
}

void HistogramWindow::deferredUpdate(void *data)
{
    static_cast<HistogramWindow *>(data)->update();
}

void HistogramWindow::update()
{
    // An open menu (which holds the grab) may be the column choice showing column_menu_.
    if (Fl::grab())
    {
        Fl::remove_timeout(deferredUpdate, this);
        Fl::add_timeout(0.1, deferredUpdate, this);
        return;
    }
    HistogramState state = model_ ? model_->histogramState() : HistogramState{};
    if (state.generation != state_.generation)
        hovered_.reset();
    state_ = std::move(state);

    rebuildColumnMenu();
    if (check_column_ && model_ && model_->state() != ModelState::Opening)
    {
        check_column_ = false;
        if (state_.active && state_.column >= column_labels_.size())
        {
            model_->cancelHistogram(); // "Choose a column." instead of an empty histogram
            state_ = model_->histogramState();
        }
    }
    const bool shown = state_.active && state_.column < column_labels_.size();
    column_choice_->value(shown ? static_cast<int>(state_.column) : -1);
    setActive(column_choice_, model_ && !column_labels_.empty());

    std::string text;
    if (!model_ || model_->state() == ModelState::Error)
        text.clear();
    else if (!state_.active)
        text = column_labels_.empty() ? "" : "Choose a column.";
    else if (!state_.done)
        text = (state_.counting ? "Counting… " : "Reading… ") + std::to_string(state_.scanned_rows) + " rows";
    else
        text = histogramSummary(state_.histogram) + (model_->view().filtered() ? " (filtered rows)" : "");
    text = escapeLabel(text);
    if (!status_->label() || text != status_->label())
    {
        status_->copy_label(text.c_str());
        status_->redraw();
    }
    showDetail();
    chart_->redraw();
}

void HistogramWindow::showDetail()
{
    const Histogram &hg = state_.histogram;
    std::string text;
    if (hovered_ && hg.binCount() > 0 && *hovered_ < hg.binCount())
    {
        const size_t count = hg.counts[*hovered_];
        text = histogramBinRange(hg, *hovered_) + ": " + std::to_string(count) + " (" +
               percentText(count, hg.values) + ")" + (state_.done ? "  Double-click to filter." : "");
    }
    else if (hovered_ && hg.binCount() == 0 && *hovered_ < hg.top.size())
    {
        const ValueCount &value = hg.top[*hovered_];
        text = cellDisplayText(value.value, VALUE_LABEL_BYTES) + ": " + std::to_string(value.count) + " (" +
               percentText(value.count, hg.rows) + ")  Double-click to filter.";
    }
    else if (state_.done && (hg.binCount() > 0 || !hg.top.empty()))
        text = "Point at a bar for its range and count; double-click it to filter.";
    text = escapeLabel(text);
    if (!detail_->label() || text != detail_->label())
    {
        detail_->copy_label(text.c_str());
        detail_->redraw();
    }
}

void HistogramWindow::filterBy(size_t index)
{
    if (!model_ || !state_.done || !model_->viewCapabilities().filter)
        return;
    const Histogram &hg = state_.histogram;
    ViewSpec spec = model_->requestedView();
    if (hg.binCount() > 0)
    {
        if (index >= hg.binCount())
            return;
        for (FilterCondition &condition : histogramBinConditions(hg, state_.column, index))
            addFilterCondition(spec, std::move(condition));
    }
    else
    {
        if (index >= hg.top.size())
            return;
        addFilterCondition(spec, cellFilterCondition(state_.column, hg.top[index].value, true));
    }
    // The histogram then starts over on the rows left.
    model_->setView(std::move(spec));
}

void HistogramWindow::rebuildColumnMenu()
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

void HistogramWindow::columnCallback(Fl_Widget *, void *data)
{
    auto *self = static_cast<HistogramWindow *>(data);
    const int value = self->column_choice_->value();
    if (value >= 0)
        self->start(static_cast<size_t>(value));
}

void HistogramWindow::closeCallback(Fl_Widget *, void *data)
{
    auto *self = static_cast<HistogramWindow *>(data);
    self->window_->hide();
    // Nothing shows it any more.
    if (self->model_)
        self->model_->cancelHistogram();
}
