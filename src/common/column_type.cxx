#include "column_type.hxx"

#include <array>
#include <cmath>
#include <utility>

namespace
{
    bool isWhole(double value)
    {
        return std::isfinite(value) && value == std::floor(value);
    }

    // Booleans as spreadsheets write them; "1" / "0" are numbers here.
    bool isBooleanText(const std::string &text)
    {
        std::string key;
        for (const char c : text)
        {
            if (c != ' ' && c != '\t')
                key.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
        return key == "true" || key == "false";
    }
} // namespace

void ColumnTypeCounter::add(const CellValue &value)
{
    if (isEmptyCell(value))
        return;
    if (std::holds_alternative<int64_t>(value))
    {
        ++numbers_;
        ++integers_;
    }
    else if (const auto *d = std::get_if<double>(&value))
    {
        ++numbers_;
        if (isWhole(*d))
            ++integers_;
    }
    else if (std::holds_alternative<bool>(value))
        ++booleans_;
    else if (std::holds_alternative<Date>(value))
        ++dates_;
    else if (std::holds_alternative<DateTime>(value))
        ++date_times_;
    else if (const auto *text = std::get_if<std::string>(&value))
    {
        if (const auto number = parseNumber(*text))
        {
            ++numbers_;
            // "3.0" is written as a decimal.
            if (isWhole(*number) && text->find_first_of(".eE") == std::string::npos)
                ++integers_;
        }
        else if (const auto temporal = parseTemporal(*text))
            ++(temporal->has_time ? date_times_ : dates_);
        else if (isBooleanText(*text))
            ++booleans_;
        else
            ++texts_;
    }
}

void ColumnTypeCounter::merge(const ColumnTypeCounter &other)
{
    numbers_ += other.numbers_;
    integers_ += other.integers_;
    dates_ += other.dates_;
    date_times_ += other.date_times_;
    booleans_ += other.booleans_;
    texts_ += other.texts_;
}

ColumnType ColumnTypeCounter::type() const
{
    ColumnType type;
    type.values = numbers_ + dates_ + date_times_ + booleans_ + texts_;
    if (type.values == 0)
        return type;
    // Dates with and without a time of day are one kind.
    const std::array<std::pair<ColumnKind, size_t>, 4> kinds = {{
        {ColumnKind::Number, numbers_},
        {date_times_ > 0 ? ColumnKind::DateTime : ColumnKind::Date, dates_ + date_times_},
        {ColumnKind::Boolean, booleans_},
        {ColumnKind::Text, texts_},
    }};
    type.kind = ColumnKind::Mixed;
    for (const auto &[kind, count] : kinds)
    {
        // A few stray values (a "-" or "N/A" among numbers) do not change it.
        if (count * 10 >= type.values * 9)
            type.kind = kind;
    }
    type.integers = type.kind == ColumnKind::Number && integers_ == numbers_;
    return type;
}

const char *columnKindMark(ColumnKind kind)
{
    switch (kind)
    {
    case ColumnKind::Number:
        return "#";
    case ColumnKind::Date:
    case ColumnKind::DateTime:
        return "@";
    case ColumnKind::Boolean:
        return "?";
    default:
        return "";
    }
}

std::string columnTypeText(const ColumnType &type)
{
    switch (type.kind)
    {
    case ColumnKind::Unknown:
        return "Unknown (no values yet)";
    case ColumnKind::Number:
        return type.integers ? "Number (whole numbers)" : "Number";
    case ColumnKind::Date:
        return "Date";
    case ColumnKind::DateTime:
        return "Date and time";
    case ColumnKind::Boolean:
        return "Boolean";
    case ColumnKind::Text:
        return "Text";
    case ColumnKind::Mixed:
        return "Mixed";
    }
    return {};
}
