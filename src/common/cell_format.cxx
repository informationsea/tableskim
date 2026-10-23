#include "cell_format.hxx"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

std::string cellDisplayText(const CellValue &value, size_t max_bytes)
{
    std::string text = formatCell(value);
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
                    std::replace_if(cell_text.begin(), cell_text.end(), [](char c)
                                    { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
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

bool filtersColumn(const ViewSpec &spec, size_t column)
{
    if (!spec.filter)
        return false;
    const auto &conditions = spec.filter->conditions;
    return std::any_of(conditions.begin(), conditions.end(), [column](const FilterCondition &c)
                       { return c.column == column; });
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
    const auto micros = static_cast<int64_t>(std::llround(edge));
    const std::string text = formatCell(DateTime{micros}); // "2024-03-05 13:00:05"
    switch (histogram.unit)
    {
    case TimeUnit::Year:
        return text.substr(0, 4);
    case TimeUnit::Month:
        return text.substr(0, 7);
    case TimeUnit::Day:
        return text.substr(0, 10);
    case TimeUnit::Hour:
    case TimeUnit::Minute:
        return text.substr(0, 16);
    case TimeUnit::Second:
        break;
    }
    return text.substr(0, 19);
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
    // Times as the filters read them: a date means the start of its day.
    auto text = [&](double edge)
    {
        if (histogram.kind != HistogramKind::Time)
            return histogramEdgeText(histogram, edge);
        const auto micros = static_cast<int64_t>(std::llround(edge));
        const std::string full = formatCell(DateTime{micros});
        return histogram.unit >= TimeUnit::Day ? full.substr(0, 10) : full.substr(0, 19);
    };
    // The last edge of times lies past every value; numbers can end on it.
    const bool last = bin + 2 == histogram.edges.size() && histogram.kind == HistogramKind::Number;
    return {
        // Values of another kind (counted as others) can still pass the
        // comparisons: "15 kg" as text, a boolean or a date as a number.
        {column, histogram.kind == HistogramKind::Time ? FilterOp::IsDate : FilterOp::IsNumber, {}},
        {column, FilterOp::GreaterEqual, text(histogram.edges[bin])},
        {column, last ? FilterOp::LessEqual : FilterOp::Less, text(histogram.edges[bin + 1])},
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
