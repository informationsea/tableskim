#include "cell_format.hxx"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace
{
    // Commas between the groups of three digits of a plain number's integer
    // part ("-1234567.5" → "-1,234,567.5"); anything else ("1e+20") as it is.
    std::string groupThousands(const std::string &number)
    {
        const size_t begin = !number.empty() && (number[0] == '-' || number[0] == '+') ? 1 : 0;
        size_t end = begin;
        while (end < number.size() && number[end] >= '0' && number[end] <= '9')
            ++end;
        if (end == begin)
            return number;
        if (end < number.size())
        {
            if (number[end] != '.')
                return number;
            for (size_t i = end + 1; i < number.size(); ++i)
            {
                if (number[i] < '0' || number[i] > '9')
                    return number;
            }
        }
        std::string grouped = number.substr(0, begin);
        const size_t digits = end - begin;
        for (size_t i = 0; i < digits; ++i)
        {
            if (i > 0 && (digits - i) % 3 == 0)
                grouped += ',';
            grouped += number[begin + i];
        }
        return grouped + number.substr(end);
    }

    std::string fixedDecimals(double value, int decimals)
    {
        const int size = std::snprintf(nullptr, 0, "%.*f", decimals, value);
        std::string text(static_cast<size_t>(std::max(size, 0)), '\0');
        std::snprintf(text.data(), text.size() + 1, "%.*f", decimals, value);
        // -0.001 rounded is 0.
        if (text.starts_with('-') && text.find_first_not_of("-0.") == std::string::npos)
            text.erase(0, 1);
        return text;
    }

    std::optional<std::string> formatNumberAs(const CellValue &value, const ColumnFormat &format)
    {
        std::string text;
        if (const auto *i = std::get_if<int64_t>(&value))
        {
            // Large integers would lose digits as doubles.
            constexpr int64_t exact = int64_t{1} << 53;
            if (format.decimals && *i >= -exact && *i <= exact)
                text = fixedDecimals(static_cast<double>(*i), *format.decimals);
            else
            {
                text = std::to_string(*i);
                if (format.decimals && *format.decimals > 0)
                    text += "." + std::string(static_cast<size_t>(*format.decimals), '0');
            }
        }
        else if (const auto *d = std::get_if<double>(&value))
        {
            if (!std::isfinite(*d))
                return std::nullopt;
            text = format.decimals ? fixedDecimals(*d, *format.decimals) : formatCell(value);
        }
        else if (const auto *s = std::get_if<std::string>(&value))
        {
            const auto number = parseNumber(*s);
            if (!number || !std::isfinite(*number))
                return std::nullopt;
            if (format.decimals)
                text = fixedDecimals(*number, *format.decimals);
            else
            {
                // The digits as written, without the spaces and "+" parseNumber() allows.
                const size_t first = s->find_first_not_of(" \t+");
                text = s->substr(first, s->find_last_not_of(" \t") - first + 1);
            }
        }
        else
            return std::nullopt;
        return format.thousands ? groupThousands(text) : text;
    }

    std::string dateText(int32_t days, DateStyle style)
    {
        static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
        const int y = static_cast<int>(ymd.year());
        const unsigned m = static_cast<unsigned>(ymd.month());
        const unsigned d = static_cast<unsigned>(ymd.day());
        char buf[64];
        switch (style)
        {
        case DateStyle::Iso:
            std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
            break;
        case DateStyle::Slash:
            std::snprintf(buf, sizeof(buf), "%04d/%02u/%02u", y, m, d);
            break;
        case DateStyle::MonthDayYear:
            std::snprintf(buf, sizeof(buf), "%02u/%02u/%04d", m, d, y);
            break;
        case DateStyle::DayMonthYear:
            std::snprintf(buf, sizeof(buf), "%02u/%02u/%04d", d, m, y);
            break;
        case DateStyle::Japanese:
            std::snprintf(buf, sizeof(buf), "%d年%u月%u日", y, m, d);
            break;
        case DateStyle::MonthName:
            std::snprintf(buf, sizeof(buf), "%s %u, %d", months[(m + 11) % 12], d, y);
            break;
        }
        return buf;
    }

    std::optional<std::string> formatDateAs(const CellValue &value, DateStyle style)
    {
        constexpr int64_t micros_per_day = 86400LL * 1000000;
        std::optional<DateTime> date_time;
        if (const auto *date = std::get_if<Date>(&value))
            return dateText(date->days_since_epoch, style);
        if (const auto *dt = std::get_if<DateTime>(&value))
            date_time = *dt;
        else if (const auto *s = std::get_if<std::string>(&value))
        {
            const auto temporal = parseTemporal(*s);
            if (!temporal)
                return std::nullopt;
            if (!temporal->has_time)
                return dateText(static_cast<int32_t>(temporal->micros / micros_per_day), style);
            date_time = DateTime{temporal->micros};
        }
        if (!date_time)
            return std::nullopt;
        // The time of day as formatCell() writes it, after "YYYY-MM-DD".
        const std::string iso = formatCell(*date_time);
        const int64_t micros = date_time->micros_since_epoch;
        const int64_t days = micros / micros_per_day - (micros % micros_per_day < 0 ? 1 : 0);
        return dateText(static_cast<int32_t>(days), style) + iso.substr(std::min<size_t>(iso.size(), 10));
    }
} // namespace

std::string dateStyleExample(DateStyle style)
{
    // 2024-01-31
    return dateText(19753, style);
}

std::string formatCellAs(const CellValue &value, const ColumnFormat &format)
{
    if (format.thousands || format.decimals)
    {
        if (auto text = formatNumberAs(value, format))
            return *text;
    }
    if (format.date != DateStyle::Iso)
    {
        if (auto text = formatDateAs(value, format.date))
            return *text;
    }
    return formatCell(value);
}

const ColumnFormat *ColumnFormats::find(size_t column) const
{
    const auto it = formats_.find(column);
    return it != formats_.end() ? &it->second : nullptr;
}

ColumnFormat ColumnFormats::get(size_t column) const
{
    const ColumnFormat *format = find(column);
    return format ? *format : ColumnFormat{};
}

void ColumnFormats::set(size_t column, const ColumnFormat &format)
{
    if (format.isDefault())
        formats_.erase(column);
    else
        formats_[column] = format;
}

std::string cellDisplayText(const CellValue &value, size_t max_bytes, const ColumnFormat *format)
{
    std::string text = format ? formatCellAs(value, *format) : formatCell(value);
    if (text.size() > max_bytes)
    {
        // cut at a UTF-8 character boundary
        size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
        text.resize(cut);
        text += "…";
    }
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    std::replace(text.begin(), text.end(), '\t', ' ');
    return text;
}

bool isNumericCell(const CellValue &value)
{
    if (std::holds_alternative<int64_t>(value) || std::holds_alternative<double>(value))
        return true;
    const auto *s = std::get_if<std::string>(&value);
    return s && parseNumber(*s).has_value();
}

size_t HiddenColumns::hide(size_t first, size_t last, size_t column_count)
{
    size_t hidden = 0;
    for (size_t c = first; c <= last && c < column_count; ++c)
    {
        if (columns_.contains(c))
            continue;
        if (columns_.size() + 1 >= column_count)
            break;
        columns_.insert(c);
        ++hidden;
    }
    return hidden;
}

bool HiddenColumns::hidesAll(size_t first, size_t last, size_t column_count) const
{
    for (size_t c = 0; c < column_count; ++c)
    {
        if (!columns_.contains(c) && (c < first || c > last))
            return false;
    }
    return true;
}

size_t HiddenColumns::visibleIn(size_t first, size_t last) const
{
    if (last < first)
        return 0;
    const auto hidden = std::distance(columns_.lower_bound(first), columns_.upper_bound(last));
    return last - first + 1 - static_cast<size_t>(hidden);
}

size_t HiddenColumns::nearestVisible(size_t column, int direction, size_t column_count) const
{
    if (column_count == 0)
        return column;
    column = std::min(column, column_count - 1);
    for (const int step : {direction, -direction})
    {
        for (size_t c = column;; c += static_cast<size_t>(step))
        {
            if (!columns_.contains(c))
                return c;
            if ((step < 0 && c == 0) || (step > 0 && c + 1 >= column_count))
                break;
        }
    }
    return column;
}

std::vector<std::vector<std::string>> parseTsv(std::string_view text)
{
    // One cell is copied verbatim (rangeToTsv()), line breaks and all;
    // several cells end with a line break.
    if (text.find('\t') == std::string_view::npos && !text.ends_with('\n'))
        return {{std::string(text)}};
    std::vector<std::vector<std::string>> rows(1);
    std::string field;
    bool quoted = false;
    bool field_start = true;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (quoted)
        {
            if (c != '"')
                field += c;
            else if (i + 1 < text.size() && text[i + 1] == '"')
            {
                field += '"';
                ++i;
            }
            else
                quoted = false;
            continue;
        }
        if (c == '"' && field_start)
        {
            quoted = true;
            field_start = false;
            continue;
        }
        if (c == '\t' || c == '\n' || c == '\r')
        {
            rows.back().push_back(std::move(field));
            field.clear();
            field_start = true;
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            if (c != '\t')
                rows.emplace_back();
            continue;
        }
        field += c;
        field_start = false;
    }
    if (field_start && rows.back().empty())
        rows.pop_back(); // after the last line break
    else
        rows.back().push_back(std::move(field));
    return rows;
}

std::vector<TableModel::CellText> pastedCells(const std::vector<std::vector<std::string>> &values, size_t row_top,
                                              size_t col_left, size_t row_bottom, size_t col_right, size_t row_count,
                                              size_t column_count, const HiddenColumns *hidden)
{
    std::vector<TableModel::CellText> cells;
    if (values.empty() || values.front().empty())
        return cells;
    auto visible = [&](size_t column)
    {
        return !hidden || !hidden->contains(column);
    };
    const bool fill = values.size() == 1 && values.front().size() == 1;
    for (size_t i = 0; row_top + i < row_count && (fill ? row_top + i <= row_bottom : i < values.size()); ++i)
    {
        const size_t row = row_top + i;
        size_t j = 0;
        for (size_t column = col_left; column < column_count; ++column)
        {
            if (fill ? column > col_right : j >= values[i].size())
                break;
            if (!visible(column))
                continue;
            cells.push_back({row, column, fill ? values[0][0] : values[i][j]});
            ++j;
        }
    }
    return cells;
}

TsvCopyResult rangeToTsv(TableModel &model, size_t row_top, size_t col_left, size_t row_bottom, size_t col_right,
                         const HiddenColumns *hidden)
{
    TsvCopyResult result;
    std::vector<size_t> columns;
    for (size_t c = col_left; c <= col_right; ++c)
    {
        if (!hidden || !hidden->contains(c))
            columns.push_back(c);
    }
    const bool single_cell = row_top == row_bottom && columns.size() == 1;
    for (size_t r = row_top; r <= row_bottom; ++r)
    {
        std::string row_text;
        bool available = true;
        for (const size_t c : columns)
        {
            if (c != columns.front())
                row_text += '\t';
            const CellRef cell = model.peek(r, c);
            if (cell.state == CellState::Ready)
            {
                std::string cell_text = formatCell(*cell.value);
                if (!single_cell)
                {
                    std::replace_if(cell_text.begin(), cell_text.end(), [](char c)
                                    { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
                    // A leading quote would open a quoted field when pasted
                    // (parseTsv(), spreadsheets).
                    if (cell_text.starts_with('"'))
                    {
                        std::string quoted = "\"";
                        for (const char c : cell_text)
                        {
                            quoted += c;
                            if (c == '"')
                                quoted += '"';
                        }
                        cell_text = quoted + '"';
                    }
                }
                row_text += cell_text;
            }
            else if (cell.state != CellState::Empty)
            {
                available = false;
                break;
            }
        }
        if (available)
        {
            result.text += row_text;
            if (!single_cell)
                result.text += '\n';
            ++result.copied_rows;
        }
        else
            ++result.skipped_rows;
    }
    return result;
}

int fittingColumnWidth(TableModel &model, size_t column, const ColumnWidthMetrics &metrics)
{
    int width = metrics.measure(model.columnName(column)) + metrics.header_padding;
    const size_t sample_rows = std::min(model.displayRowCount(), metrics.sample_rows);
    for (size_t r = 0; r < sample_rows; ++r)
    {
        const CellRef cell = model.peek(r, column);
        if (cell.state == CellState::Ready)
            width = std::max(width, metrics.measure(cellDisplayText(*cell.value)) + metrics.cell_padding);
    }
    return std::clamp(width, metrics.min_width, metrics.max_width);
}

std::vector<SortKey> nextSortCycle(const std::vector<SortKey> &sort, size_t column)
{
    if (sort.empty() || sort.front().column != column)
        return {{column, true}};
    if (sort.front().ascending)
        return {{column, false}};
    return {};
}

std::string rowCountText(const RowCount &count)
{
    switch (count.kind)
    {
    case RowCount::Kind::Exact:
        return std::to_string(count.value) + (count.value == 1 ? " row" : " rows");
    case RowCount::Kind::AtLeast:
        return "≥" + std::to_string(count.value) + " rows";
    case RowCount::Kind::Unknown:
        break;
    }
    return "? rows";
}

std::string filteredRowCountText(const RowCount &rows, const RowCount &unfiltered, bool filtered)
{
    if (!filtered)
        return rowCountText(rows);
    std::string text = rowCountText(rows);
    if (unfiltered.isExact())
        text += " of " + std::to_string(unfiltered.value);
    else
        text += " (filtered)";
    return text;
}

const char *filterOpLabel(FilterOp op)
{
    switch (op)
    {
    case FilterOp::Equals:
        return "=";
    case FilterOp::NotEquals:
        return "≠";
    case FilterOp::Contains:
        return "contains";
    case FilterOp::NotContains:
        return "not contains";
    case FilterOp::Less:
        return "<";
    case FilterOp::LessEqual:
        return "≤";
    case FilterOp::Greater:
        return ">";
    case FilterOp::GreaterEqual:
        return "≥";
    case FilterOp::IsEmpty:
        return "is empty";
    case FilterOp::IsNotEmpty:
        return "is not empty";
    case FilterOp::Matches:
        return "matches";
    case FilterOp::NotMatches:
        return "not matches";
    case FilterOp::IsNumber:
        return "is a number";
    case FilterOp::IsDate:
        return "is a date";
    }
    return "?";
}

bool filterOpTakesValue(FilterOp op)
{
    return op != FilterOp::IsEmpty && op != FilterOp::IsNotEmpty && op != FilterOp::IsNumber && op != FilterOp::IsDate;
}

FilterCondition cellFilterCondition(size_t column, const CellValue &value, bool equal)
{
    std::string text = formatCell(value);
    if (text.empty())
        return {column, equal ? FilterOp::IsEmpty : FilterOp::IsNotEmpty, {}};
    return {column, equal ? FilterOp::Equals : FilterOp::NotEquals, std::move(text)};
}

void addFilterCondition(ViewSpec &spec, FilterCondition condition)
{
    if (!spec.filter)
        spec.filter = Filter{};
    auto &conditions = spec.filter->conditions;
    if (std::find(conditions.begin(), conditions.end(), condition) == conditions.end())
        conditions.push_back(std::move(condition));
}

void addNarrowingConditions(ViewSpec &spec, std::vector<FilterCondition> conditions)
{
    if (conditions.empty())
        return;
    if (!spec.filter)
        spec.filter = Filter{};
    auto &narrowing = spec.filter->narrowing;
    for (auto &condition : conditions)
    {
        if (std::find(narrowing.begin(), narrowing.end(), condition) == narrowing.end())
            narrowing.push_back(std::move(condition));
    }
}

bool filtersColumn(const ViewSpec &spec, size_t column)
{
    if (!spec.filter)
        return false;
    const auto uses = [column](const FilterCondition &c) { return c.column == column; };
    return std::any_of(spec.filter->conditions.begin(), spec.filter->conditions.end(), uses) ||
           std::any_of(spec.filter->narrowing.begin(), spec.filter->narrowing.end(), uses);
}

std::string filterSummary(const TableModel &model, const Filter &filter)
{
    std::string text;
    for (const FilterCondition &condition : filter.conditions)
    {
        if (!text.empty())
            text += filter.combine == Filter::Combine::And ? " and " : " or ";
        text += model.columnName(condition.column) + " " + filterOpLabel(condition.op);
        if (filterOpTakesValue(condition.op))
            text += " \"" + condition.value + "\"";
    }
    if (!filter.narrowing.empty())
    {
        Filter additional;
        additional.conditions = filter.narrowing;
        const std::string selection = filterSummary(model, additional);
        text = text.empty() ? "Additional narrowing: " + selection
                            : "(" + text + ") and (Additional narrowing: " + selection + ")";
    }
    return text;
}

std::string percentText(size_t count, size_t total)
{
    if (total == 0)
        return "0%";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f%%", 100.0 * static_cast<double>(count) / static_cast<double>(total));
    std::string text = buffer;
    // "100.0%" and "0.0%" read better without the decimal.
    if (text.ends_with(".0%"))
        text.erase(text.size() - 3, 2);
    return text;
}

namespace
{
    // formatCell() of a time, cut after `unit`. The year may have more or
    // fewer than four digits (10000, -221), so the parts are found from the
    // end of the date.
    std::string timeText(double micros, TimeUnit unit)
    {
        const std::string full = formatCell(DateTime{static_cast<int64_t>(std::llround(micros))});
        const size_t date = std::min(full.find(' '), full.size()); // "2024-03-05"
        switch (unit)
        {
        case TimeUnit::Year:
            return full.substr(0, date - 6);
        case TimeUnit::Month:
            return full.substr(0, date - 3);
        case TimeUnit::Day:
            return full.substr(0, date);
        case TimeUnit::Hour:
        case TimeUnit::Minute:
            return full.substr(0, date + 6);
        case TimeUnit::Second:
            break;
        }
        return full.substr(0, date + 9);
    }

    // A time bound of a filter: the start of the day for dates. A bound the
    // filters cannot read (a year past 9999 or before 0) lies beyond every
    // time they can, so it is left out.
    void addTimeBound(std::vector<FilterCondition> &conditions, size_t column, FilterOp op, double micros, bool dates)
    {
        std::string text = timeText(micros, dates ? TimeUnit::Day : TimeUnit::Second);
        if (parseTemporal(text))
            conditions.push_back({column, op, std::move(text)});
    }
} // namespace

std::string histogramEdgeText(const Histogram &histogram, double edge)
{
    if (histogram.kind != HistogramKind::Time)
    {
        const int digits = histogram.integers ? 0 : histogram.decimals;
        const int size = std::snprintf(nullptr, 0, "%.*f", digits, edge);
        if (size < 0)
            return formatCell(CellValue{edge});
        std::string text(static_cast<size_t>(size), '\0');
        std::snprintf(text.data(), text.size() + 1, "%.*f", digits, edge);
        return text == "-0" ? "0" : text;
    }
    return timeText(edge, histogram.unit);
}

std::string histogramBinLabel(const Histogram &histogram, size_t bin)
{
    if (bin + 1 >= histogram.edges.size())
        return {};
    const double lower = histogram.edges[bin];
    const double upper = histogram.edges[bin + 1];
    if (histogram.kind == HistogramKind::Time)
        return histogramEdgeText(histogram, lower);
    if (histogram.integers && upper - lower == 1)
        return histogramEdgeText(histogram, lower);
    return histogramEdgeText(histogram, lower) + " – " + histogramEdgeText(histogram, upper);
}

std::string histogramBinRange(const Histogram &histogram, size_t bin)
{
    if (bin + 1 >= histogram.edges.size())
        return {};
    const bool last = bin + 2 == histogram.edges.size() && histogram.kind == HistogramKind::Number;
    return "[" + histogramEdgeText(histogram, histogram.edges[bin]) + ", " +
           histogramEdgeText(histogram, histogram.edges[bin + 1]) + (last ? "]" : ")");
}

std::vector<FilterCondition> histogramBinConditions(const Histogram &histogram, size_t column, size_t bin)
{
    if (bin + 1 >= histogram.edges.size())
        return {};
    if (histogram.kind == HistogramKind::Time)
    {
        // Times as the filters read them: a date means the start of its day.
        // The last edge of times lies past every value.
        std::vector<FilterCondition> conditions{{column, FilterOp::IsDate, {}}};
        const bool dates = histogram.unit >= TimeUnit::Day;
        addTimeBound(conditions, column, FilterOp::GreaterEqual, histogram.edges[bin], dates);
        addTimeBound(conditions, column, FilterOp::Less, histogram.edges[bin + 1], dates);
        return conditions;
    }
    // Numbers can end on the last edge.
    const bool last = bin + 2 == histogram.edges.size();
    return {
        // Values of another kind (counted as others) can still pass the
        // comparisons: "15 kg" as text, a boolean or a date as a number.
        {column, FilterOp::IsNumber, {}},
        {column, FilterOp::GreaterEqual, histogramEdgeText(histogram, histogram.edges[bin])},
        {column, last ? FilterOp::LessEqual : FilterOp::Less, histogramEdgeText(histogram, histogram.edges[bin + 1])},
    };
}

std::string histogramSummary(const Histogram &histogram)
{
    auto counted = [](size_t n, const char *one, const char *many)
    { return std::to_string(n) + " " + (n == 1 ? one : many); };
    std::string text;
    if (histogram.kind == HistogramKind::None)
        text = counted(histogram.rows - histogram.empty, "value", "values") + ", neither numbers nor dates";
    else
    {
        text = histogram.kind == HistogramKind::Number ? counted(histogram.values, "number", "numbers")
               : histogram.dates                       ? counted(histogram.values, "date", "dates")
                                                       : counted(histogram.values, "time", "times");
        text += " in " + counted(histogram.binCount(), "bin", "bins");
        if (histogram.others > 0)
            text += ", " + counted(histogram.others, "other value", "other values");
    }
    if (histogram.empty > 0)
        text += ", " + std::to_string(histogram.empty) + " empty";
    return text;
}

std::string scatterValueText(const Histogram &axis, double coordinate)
{
    if (axis.kind == HistogramKind::Time)
    {
        if (axis.dates)
            return timeText(coordinate, TimeUnit::Day);
        return formatCell(DateTime{static_cast<int64_t>(std::llround(coordinate))});
    }
    // Whole numbers in full: the shortest form of 500000.0 is "5e+05".
    if ((axis.integers || coordinate == std::floor(coordinate)) && std::fabs(coordinate) < 9.007199254740992e15)
        return std::to_string(static_cast<int64_t>(std::llround(coordinate)));
    return formatCell(CellValue{coordinate});
}

std::string scatterCategoryText(const ScatterPlot &plot, size_t category)
{
    if (category < plot.categories.size())
        return formatCell(plot.categories[category]);
    return "(other)";
}

std::vector<FilterCondition> scatterCategoryConditions(const ScatterPlot &plot, size_t x_column, size_t y_column,
                                                       size_t category)
{
    if (category >= plot.categories.size() || !plot.plottable())
        return {};
    return {cellFilterCondition(x_column, plot.categories[category], true),
            {y_column, plot.y_bins.kind == HistogramKind::Time ? FilterOp::IsDate : FilterOp::IsNumber, {}}};
}

std::string boxPlotText(const ScatterPlot &plot, size_t category)
{
    if (category >= plot.boxes.size())
        return {};
    const BoxStats &box = plot.boxes[category];
    std::string text = scatterCategoryText(plot, category) + ": " + std::to_string(box.count) +
                       (box.count == 1 ? " row" : " rows");
    if (box.count == 0)
        return text;
    if (box.sampled())
        text += "; statistics estimated from a sample of " + std::to_string(box.sample_count) + " rows";
    const auto value = [&plot](double coordinate)
    { return scatterValueText(plot.y_axis, coordinate); };
    // The mean of numbers with two decimals more than the axis shows.
    std::string mean = value(box.mean);
    if (plot.y_axis.kind == HistogramKind::Number)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", plot.y_axis.decimals + 2, box.mean);
        mean = buffer;
        if (mean.find('.') != std::string::npos)
        {
            mean.erase(mean.find_last_not_of('0') + 1);
            if (mean.back() == '.')
                mean.pop_back();
        }
    }
    text += "; min " + value(box.min) + ", Q1 " + value(box.q1) + ", median " + value(box.median) + ", Q3 " +
            value(box.q3) + ", max " + value(box.max) + "; mean " + mean;
    if (box.outlier_count > 0)
        text += "; " + std::to_string(box.outlier_count) + (box.outlier_count == 1 ? " outlier" : " outliers");
    return text;
}

std::vector<FilterCondition> densityCellConditions(const ScatterPlot &plot, size_t x_column, size_t y_column,
                                                   size_t x_bin, size_t y_bin)
{
    if (plot.categorical())
    {
        std::vector<FilterCondition> conditions;
        if (x_bin < plot.categories.size())
            conditions.push_back(cellFilterCondition(x_column, plot.categories[x_bin], true));
        else
            return conditions; // "(other)" is not one value
        for (FilterCondition &condition : histogramBinConditions(plot.y_bins, y_column, y_bin))
            conditions.push_back(std::move(condition));
        return conditions;
    }
    std::vector<FilterCondition> conditions = histogramBinConditions(plot.x_bins, x_column, x_bin);
    for (FilterCondition &condition : histogramBinConditions(plot.y_bins, y_column, y_bin))
        conditions.push_back(std::move(condition));
    return conditions;
}

namespace
{
    // One axis of scatterRangeConditions().
    void addRangeConditions(std::vector<FilterCondition> &conditions, const Histogram &bins, size_t column,
                            double low, double high)
    {
        if (high < low)
            std::swap(low, high);
        if (bins.kind == HistogramKind::Time)
        {
            // Dates as the filters read them: the start of the day.
            constexpr double MICROS_PER_DAY = 86400e6;
            conditions.push_back({column, FilterOp::IsDate, {}});
            addTimeBound(conditions, column, FilterOp::GreaterEqual,
                         bins.dates ? std::ceil(low / MICROS_PER_DAY) * MICROS_PER_DAY : std::ceil(low / 1e6) * 1e6, bins.dates);
            addTimeBound(conditions, column, FilterOp::LessEqual, high, bins.dates);
            return;
        }
        // A digit finer than the bins; integers between the bounds.
        const int digits = bins.integers ? 0 : bins.decimals + 1;
        const double scale = std::pow(10.0, digits);
        auto text = [&](double value)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
            return std::string(std::string_view(buffer) == "-0" ? "0" : buffer);
        };
        conditions.push_back({column, FilterOp::IsNumber, {}});
        conditions.push_back({column, FilterOp::GreaterEqual, text(std::ceil(low * scale) / scale)});
        conditions.push_back({column, FilterOp::LessEqual, text(std::floor(high * scale) / scale)});
    }
} // namespace

std::vector<FilterCondition> scatterRangeConditions(const ScatterPlot &plot, size_t x_column, size_t y_column,
                                                    double x_low, double x_high, double y_low, double y_high)
{
    std::vector<FilterCondition> conditions;
    // Categories have no range.
    if (!plot.plottable() || plot.categorical())
        return conditions;
    addRangeConditions(conditions, plot.x_bins, x_column, x_low, x_high);
    addRangeConditions(conditions, plot.y_bins, y_column, y_low, y_high);
    return conditions;
}

std::string densityCellText(const ScatterPlot &plot, size_t x_bin, size_t y_bin)
{
    if (x_bin >= plot.xBinCount() || y_bin >= plot.y_bins.binCount())
        return {};
    const size_t count = plot.count(x_bin, y_bin);
    const std::string x = plot.categorical() ? scatterCategoryText(plot, x_bin) : "x " + histogramBinRange(plot.x_bins, x_bin);
    return x + " × y " + histogramBinRange(plot.y_bins, y_bin) + ": " + std::to_string(count) +
           (count == 1 ? " row (" : " rows (") + percentText(count, plot.pairs) + ")";
}

std::string scatterStatsText(const ScatterPlot &plot)
{
    const ScatterStats &stats = plot.stats;
    if (stats.n < 2)
        return {};
    auto format = [](const char *pattern, double value)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), pattern, value);
        return std::string(buffer);
    };
    const auto r = stats.correlation();
    if (!r)
        return "r undefined (a column is constant)";
    std::string text = format("r = %.4f", *r) + format(", R² = %.4f", *r * *r);
    const auto slope = stats.slope();
    const auto intercept = stats.intercept();
    if (slope && intercept && plot.x_bins.kind == HistogramKind::Number && plot.y_bins.kind == HistogramKind::Number)
    {
        text += format(", y = %.6g x", *slope) + (*intercept < 0 ? " - " : " + ") +
                format("%.6g", std::fabs(*intercept));
    }
    return text;
}

bool columnLooksPlottable(TableModel &model, size_t column, size_t first_row, size_t last_row)
{
    size_t plottable = 0;
    size_t others = 0;
    for (size_t row = first_row; row < last_row; ++row)
    {
        const CellRef cell = model.peek(row, column);
        if (cell.state == CellState::OutOfRange)
            break;
        if (cell.state != CellState::Ready || isEmptyCell(*cell.value))
            continue;
        if (histogramCoordinate(HistogramKind::Number, *cell.value) ||
            histogramCoordinate(HistogramKind::Time, *cell.value))
            ++plottable;
        else
            ++others;
    }
    return plottable > 0 && plottable >= others;
}

ScatterColumns defaultScatterColumns(TableModel &model, size_t column, const HiddenColumns &hidden, size_t first_row,
                                     size_t last_row)
{
    const size_t count = model.columnCount();
    std::vector<bool> candidates(count, false);
    for (size_t c = 0; c < count; ++c)
        candidates[c] = !hidden.contains(c) && columnLooksPlottable(model, c, first_row, last_row);
    // The candidate nearest to `from` (itself included unless excluded),
    // preferring the left one on a tie.
    auto nearest = [&](size_t from, std::optional<size_t> excluded) -> std::optional<size_t>
    {
        for (size_t distance = 0; distance < count; ++distance)
        {
            if (distance <= from && candidates[from - distance] && from - distance != excluded)
                return from - distance;
            if (from + distance < count && candidates[from + distance] && from + distance != excluded)
                return from + distance;
        }
        return std::nullopt;
    };
    if (column < count)
    {
        if (const auto y = nearest(column, std::nullopt))
        {
            std::optional<size_t> x;
            for (size_t c = *y; c-- > 0 && !x;)
            {
                if (candidates[c])
                    x = c;
            }
            if (!x)
                x = nearest(*y, *y);
            if (x)
                return {*x, *y};
        }
    }

    ScatterColumns columns{column, column};
    for (size_t c = column; c-- > 0;)
    {
        if (!hidden.contains(c))
        {
            columns.x = c;
            break;
        }
    }
    if (columns.x == columns.y && columns.y + 1 < count)
        ++columns.y;
    return columns;
}

double densityScale(size_t count, size_t most, bool log_scale)
{
    if (most == 0)
        return 0;
    if (log_scale)
        return std::log1p(static_cast<double>(count)) / std::log1p(static_cast<double>(most));
    return static_cast<double>(count) / static_cast<double>(most);
}

std::array<unsigned char, 3> densityColor(double t)
{
    static const std::array<std::array<unsigned char, 3>, 10> colors = {{
        {68, 1, 84},
        {72, 40, 120},
        {62, 74, 137},
        {49, 104, 142},
        {38, 130, 142},
        {31, 158, 137},
        {53, 183, 121},
        {109, 205, 89},
        {180, 222, 44},
        {253, 231, 37},
    }};
    t = std::clamp(std::isnan(t) ? 0.0 : t, 0.0, 1.0) * static_cast<double>(colors.size() - 1);
    const auto i = std::min(static_cast<size_t>(t), colors.size() - 2);
    const double f = t - static_cast<double>(i);
    std::array<unsigned char, 3> rgb{};
    for (size_t k = 0; k < 3; ++k)
        rgb[k] = static_cast<unsigned char>(std::lround(colors[i][k] * (1 - f) + colors[i + 1][k] * f));
    return rgb;
}

std::string scatterSummary(const ScatterPlot &plot, bool points)
{
    if (!plot.plottable())
    {
        if (plot.y_axis.kind == HistogramKind::None)
            return "The Y column holds no numbers or dates";
        return "The X column holds no values";
    }
    std::string text = std::to_string(plot.pairs) + (plot.pairs == 1 ? " row" : " rows") + " plotted";
    if (plot.categorical())
    {
        const size_t categories = plot.categories.size();
        text += plot.other_category ? " in the " + std::to_string(categories) + " most frequent values of X and (other)"
                                    : " in " + std::to_string(categories) + (categories == 1 ? " value" : " values") + " of X";
    }
    if (std::any_of(plot.boxes.begin(), plot.boxes.end(), [](const BoxStats &box) { return box.sampled(); }))
        text += ", box statistics estimated from samples";
    if (plot.others > 0)
        text += ", " + std::to_string(plot.others) + " skipped (empty or not numbers or dates)";
    if (points && plot.sampled())
        text += ", a sample of " + std::to_string(plot.points->size()) + " points";
    return text;
}

std::string memorySizeText(size_t bytes)
{
    constexpr double MB = 1024.0 * 1024.0;
    char buffer[32];
    if (bytes < (size_t{1} << 30))
        std::snprintf(buffer, sizeof(buffer), "%.0f MB", static_cast<double>(bytes) / MB);
    else
        std::snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / (1024.0 * MB));
    return buffer;
}

std::vector<StatsLine> columnStatsLines(const ColumnStats &stats)
{
    std::vector<StatsLine> lines;
    lines.push_back({"Rows", std::to_string(stats.rows)});
    lines.push_back({"Empty", std::to_string(stats.empty) + " (" + percentText(stats.empty, stats.rows) + ")"});
    lines.push_back({"Distinct", (stats.distinct_exact ? "" : "≥") + std::to_string(stats.distinct)});
    if (stats.numbers > 0)
        lines.push_back({"Numbers", std::to_string(stats.numbers) + " (" + percentText(stats.numbers, stats.rows) + ")"});
    if (stats.min)
        lines.push_back({"Min", cellDisplayText(*stats.min)});
    if (stats.max)
        lines.push_back({"Max", cellDisplayText(*stats.max)});
    if (stats.sum)
        lines.push_back({"Sum", formatCell(CellValue{*stats.sum})});
    if (stats.mean)
        lines.push_back({"Mean", formatCell(CellValue{*stats.mean})});
    if (stats.stddev)
        lines.push_back({"Std dev", formatCell(CellValue{*stats.stddev})});
    return lines;
}

namespace
{
    // ASCII only, unlike <cctype> in some locales.
    bool isDigits(std::string_view text)
    {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
                                            { return c >= '0' && c <= '9'; });
    }

    bool isLetters(std::string_view text)
    {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
                                            { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); });
    }

    // "1" -> 0; nullopt for 0 or a number too large for size_t.
    std::optional<size_t> oneBased(std::string_view digits)
    {
        size_t value = 0;
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (result.ec != std::errc() || result.ptr != digits.data() + digits.size() || value == 0)
            return std::nullopt;
        return value - 1;
    }

    // "A" -> 0, "Z" -> 25, "AA" -> 26: the inverse of columnLetter().
    std::optional<size_t> columnFromLetters(std::string_view letters)
    {
        // Three letters reach column 18278, far beyond any sheet.
        if (letters.empty() || letters.size() > 3 || !isLetters(letters))
            return std::nullopt;
        size_t value = 0;
        for (const char c : letters)
            value = value * 26 + static_cast<size_t>((c >= 'a' ? c - 'a' : c - 'A') + 1);
        return value - 1;
    }

    std::string lowerAscii(std::string_view text)
    {
        std::string result(text);
        for (auto &c : result)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return result;
    }
} // namespace

GoToResult parseGoTo(std::string_view text, const std::vector<std::string> &names, size_t column_count,
                     const RowCount &rows)
{
    const size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos)
        return {.error = "Enter a row number, a cell (B12 or R12C2) or a column"};
    text = text.substr(begin, text.find_last_not_of(" \t") - begin + 1);

    std::optional<size_t> row;
    std::optional<size_t> column;
    bool parsed = false;
    auto numberError = [](std::string_view digits) -> std::string
    {
        return digits.find_first_not_of('0') == std::string_view::npos ? "Rows and columns are numbered from 1"
                                                                         : "The number is too large";
    };
    auto findName = [&](bool fold) -> std::optional<size_t>
    {
        const std::string key = fold ? lowerAscii(text) : std::string(text);
        for (size_t c = 0; c < names.size(); ++c)
        {
            if ((fold ? lowerAscii(names[c]) : names[c]) == key)
                return c;
        }
        return std::nullopt;
    };

    if (isDigits(text))
    {
        row = oneBased(text);
        if (!row)
            return {.error = numberError(text)};
        parsed = true;
    }
    else if (auto name = findName(false); name || (name = findName(true)))
    {
        // Before letters, so that "id" finds the column "ID", not column 238.
        column = *name;
        parsed = true;
    }
    else
    {
        // R1C1: "R12C2" (also "r12c2").
        const size_t c_pos = text.find_first_of("Cc", 1);
        if ((text[0] == 'R' || text[0] == 'r') && c_pos != std::string_view::npos &&
            isDigits(text.substr(1, c_pos - 1)) && isDigits(text.substr(c_pos + 1)))
        {
            row = oneBased(text.substr(1, c_pos - 1));
            column = oneBased(text.substr(c_pos + 1));
            if (!row)
                return {.error = numberError(text.substr(1, c_pos - 1))};
            if (!column)
                return {.error = numberError(text.substr(c_pos + 1))};
            parsed = true;
        }
        else
        {
            // A1: letters, then an optional row number.
            const size_t digits = text.find_first_of("0123456789");
            const std::string_view letters = text.substr(0, digits);
            if (const auto letter_column = columnFromLetters(letters);
                letter_column && (digits == std::string_view::npos || isDigits(text.substr(digits))))
            {
                column = letter_column;
                if (digits != std::string_view::npos)
                {
                    row = oneBased(text.substr(digits));
                    if (!row)
                        return {.error = numberError(text.substr(digits))};
                }
                parsed = true;
            }
        }
    }
    if (!parsed)
        return {.error = "No row, cell or column \"" + std::string(text) + "\""};
    if (row && rows.isExact() && *row >= rows.value)
        return {.error = rows.value == 1 ? "There is only 1 row" : "There are only " + std::to_string(rows.value) + " rows"};
    if (column && *column >= column_count)
        return {.error = column_count == 1 ? "There is only 1 column"
                                           : "There are only " + std::to_string(column_count) + " columns"};
    return {.target = GoToTarget{row, column}};
}

GoToResult parseGoTo(std::string_view text, const TableModel &model)
{
    std::vector<std::string> names;
    const size_t column_count = model.columnCount();
    if (model.hasHeader())
    {
        names.reserve(column_count);
        for (size_t c = 0; c < column_count; ++c)
            names.push_back(model.columnName(c));
    }
    return parseGoTo(text, names, column_count, model.rowCount());
}
