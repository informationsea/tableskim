#include "table.hxx"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

#include <re2/re2.h>

#include "file_util.hxx"

// ===========================================================================
// Values
// ===========================================================================

namespace
{
    std::string formatDate(int32_t days)
    {
        const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                      static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
        return buf;
    }

    std::string formatDateTime(int64_t micros)
    {
        using namespace std::chrono;
        const sys_time<microseconds> tp{microseconds{micros}};
        const auto day = floor<days>(tp);
        const hh_mm_ss<microseconds> tod{tp - day};
        std::string result = formatDate(static_cast<int32_t>(day.time_since_epoch().count()));
        char buf[32];
        std::snprintf(buf, sizeof(buf), " %02d:%02d:%02d", static_cast<int>(tod.hours().count()),
                      static_cast<int>(tod.minutes().count()), static_cast<int>(tod.seconds().count()));
        result += buf;
        if (const auto frac = static_cast<long long>(tod.subseconds().count()); frac != 0)
        {
            // milliseconds when that is all the precision there is (e.g. Excel)
            if (frac % 1000 == 0)
                std::snprintf(buf, sizeof(buf), ".%03lld", frac / 1000);
            else
                std::snprintf(buf, sizeof(buf), ".%06lld", frac);
            result += buf;
        }
        return result;
    }

    std::string formatDouble(double value)
    {
        char buf[400];
        auto result = std::to_chars(buf, buf + sizeof(buf), value);
        // The shortest form of 500000.0 is "5e+05"; numbers of everyday
        // size are written out ("500000", "0.0001").
        const double magnitude = std::fabs(value);
        if (std::find(buf, result.ptr, 'e') != result.ptr && magnitude >= 1e-5 && magnitude < 1e16)
            result = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed);
        return std::string(buf, result.ptr);
    }
} // namespace

std::string formatCell(const CellValue &value)
{
    struct Visitor
    {
        std::string operator()(std::monostate) const { return {}; }
        std::string operator()(const std::string &v) const { return v; }
        std::string operator()(int64_t v) const { return std::to_string(v); }
        std::string operator()(double v) const { return formatDouble(v); }
        std::string operator()(bool v) const { return v ? "TRUE" : "FALSE"; }
        std::string operator()(Date v) const { return formatDate(v.days_since_epoch); }
        std::string operator()(DateTime v) const { return formatDateTime(v.micros_since_epoch); }
    };
    return std::visit(Visitor{}, value);
}

std::optional<double> parseNumber(const std::string &text)
{
    const char *begin = text.data();
    const char *end = text.data() + text.size();
    while (begin < end && (*begin == ' ' || *begin == '\t'))
        ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    if (begin < end && *begin == '+')
        ++begin;
    if (begin == end)
        return std::nullopt;
    double value = 0;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end)
        return std::nullopt;
    return value;
}

std::optional<TemporalValue> parseTemporal(std::string_view text)
{
    const size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos)
        return std::nullopt;
    text = text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
    size_t pos = 0;
    // Exactly `digits` digits.
    auto number = [&](size_t digits) -> std::optional<int>
    {
        if (pos + digits > text.size())
            return std::nullopt;
        int value = 0;
        for (size_t i = 0; i < digits; ++i)
        {
            const char c = text[pos + i];
            if (c < '0' || c > '9')
                return std::nullopt;
            value = value * 10 + (c - '0');
        }
        pos += digits;
        return value;
    };
    auto expect = [&](char c)
    {
        if (pos < text.size() && text[pos] == c)
        {
            ++pos;
            return true;
        }
        return false;
    };

    const auto year = number(4);
    if (!year || !expect('-'))
        return std::nullopt;
    const auto month = number(2);
    if (!month || !expect('-'))
        return std::nullopt;
    const auto day = number(2);
    if (!day)
        return std::nullopt;
    const std::chrono::year_month_day ymd{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                                          std::chrono::day{static_cast<unsigned>(*day)}};
    if (!ymd.ok())
        return std::nullopt;
    constexpr int64_t micros_per_day = 86400LL * 1000000;
    TemporalValue value{std::chrono::sys_days{ymd}.time_since_epoch().count() * micros_per_day, false};
    if (pos == text.size())
        return value;

    if (!expect(' ') && !expect('T'))
        return std::nullopt;
    const auto hours = number(2);
    if (!hours || *hours > 23 || !expect(':'))
        return std::nullopt;
    const auto minutes = number(2);
    if (!minutes || *minutes > 59)
        return std::nullopt;
    int64_t seconds = 0;
    int64_t fraction = 0;
    if (expect(':'))
    {
        const auto whole = number(2);
        if (!whole || *whole > 59)
            return std::nullopt;
        seconds = *whole;
        if (expect('.'))
        {
            // Microseconds; further digits are dropped.
            const size_t first = pos;
            int64_t scale = 100000;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
            {
                fraction += (text[pos] - '0') * scale;
                scale /= 10;
                ++pos;
            }
            if (pos == first)
                return std::nullopt;
        }
    }
    if (pos != text.size())
        return std::nullopt;
    value.micros += ((*hours * 60LL + *minutes) * 60 + seconds) * 1000000 + fraction;
    value.has_time = true;
    return value;
}

std::optional<bool> parseBoolean(std::string_view text)
{
    const size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos)
        return std::nullopt;
    text = text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
    std::string key;
    for (const char c : text)
        key.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    if (key == "true" || key == "yes" || key == "1")
        return true;
    if (key == "false" || key == "no" || key == "0")
        return false;
    return std::nullopt;
}

std::string columnLetter(size_t column)
{
    std::string result;
    size_t n = column + 1;
    while (n > 0)
    {
        --n;
        result.insert(result.begin(), static_cast<char>('A' + n % 26));
        n /= 26;
    }
    return result;
}

bool looksLikeHeaderRow(const TableRow &row, const TableRow *next_row)
{
    if (row.empty())
        return false;
    if (row.size() == 1 && next_row)
    {
        const auto non_empty = std::count_if(next_row->begin(), next_row->end(), [](const CellValue &cell)
                                             { return !formatCell(cell).empty(); });
        if (non_empty > 1)
            return false; // e.g. a report title above the table
    }
    std::set<std::string> seen;
    for (const auto &cell : row)
    {
        const auto *text = std::get_if<std::string>(&cell);
        if (!text || text->empty() || parseNumber(*text) || !seen.insert(*text).second)
            return false;
    }
    return true;
}

size_t RowBlock::approxBytes() const
{
    size_t bytes = sizeof(RowBlock) + rows.capacity() * sizeof(TableRow);
    for (const auto &row : rows)
    {
        bytes += row.capacity() * sizeof(CellValue);
        for (const auto &cell : row)
        {
            if (const auto *s = std::get_if<std::string>(&cell); s && s->capacity() > 15)
                bytes += s->capacity();
        }
    }
    return bytes;
}

// ===========================================================================
// Sort / filter / search helpers
// ===========================================================================

namespace
{
    const CellValue kMissing{};
    const auto STATS_PUBLISH_INTERVAL = std::chrono::milliseconds(200);

    const CellValue &cellAt(const TableRow &row, size_t column)
    {
        return column < row.size() ? row[column] : kMissing;
    }

    bool isNull(const CellValue &value)
    {
        if (std::holds_alternative<std::monostate>(value))
            return true;
        const auto *s = std::get_if<std::string>(&value);
        return s && s->empty();
    }

    using NumericValue = std::variant<int64_t, double>;

    std::optional<NumericValue> numericValue(const CellValue &value)
    {
        struct Visitor
        {
            std::optional<NumericValue> operator()(std::monostate) const { return std::nullopt; }
            std::optional<NumericValue> operator()(const std::string &v) const
            {
                const char *begin = v.data();
                const char *end = begin + v.size();
                while (begin < end && (*begin == ' ' || *begin == '\t'))
                    ++begin;
                while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
                    --end;
                if (begin < end && *begin == '+')
                    ++begin;
                int64_t integer = 0;
                const auto parsed = std::from_chars(begin, end, integer);
                if (parsed.ec == std::errc() && parsed.ptr == end)
                    return integer;
                if (const auto number = parseNumber(v))
                    return *number;
                return std::nullopt;
            }
            std::optional<NumericValue> operator()(int64_t v) const { return v; }
            std::optional<NumericValue> operator()(double v) const { return v; }
            std::optional<NumericValue> operator()(bool v) const { return int64_t{v ? 1 : 0}; }
            std::optional<NumericValue> operator()(Date v) const { return int64_t{v.days_since_epoch}; }
            std::optional<NumericValue> operator()(DateTime v) const { return v.micros_since_epoch; }
        };
        return std::visit(Visitor{}, value);
    }

    int compareDateToDateTime(Date date, DateTime date_time)
    {
        constexpr int64_t micros_per_day = 86400LL * 1000000;
        int64_t date_time_day = date_time.micros_since_epoch / micros_per_day;
        int64_t micros_in_day = date_time.micros_since_epoch % micros_per_day;
        if (micros_in_day < 0)
        {
            --date_time_day;
            micros_in_day += micros_per_day;
        }
        if (date.days_since_epoch != date_time_day)
            return date.days_since_epoch < date_time_day ? -1 : 1;
        return micros_in_day == 0 ? 0 : -1;
    }

    std::optional<int> compareTemporal(const CellValue &a, const CellValue &b)
    {
        if (const auto *a_date = std::get_if<Date>(&a))
        {
            if (const auto *b_date = std::get_if<Date>(&b))
            {
                if (a_date->days_since_epoch != b_date->days_since_epoch)
                    return a_date->days_since_epoch < b_date->days_since_epoch ? -1 : 1;
                return 0;
            }
            if (const auto *b_date_time = std::get_if<DateTime>(&b))
                return compareDateToDateTime(*a_date, *b_date_time);
        }
        if (const auto *a_date_time = std::get_if<DateTime>(&a))
        {
            if (const auto *b_date_time = std::get_if<DateTime>(&b))
            {
                if (a_date_time->micros_since_epoch != b_date_time->micros_since_epoch)
                    return a_date_time->micros_since_epoch < b_date_time->micros_since_epoch ? -1 : 1;
                return 0;
            }
            if (const auto *b_date = std::get_if<Date>(&b))
                return -compareDateToDateTime(*b_date, *a_date_time);
        }
        return std::nullopt;
    }

    // numericValue() for sorting: a date-time counts in days (its time as
    // the fraction), like a date. In microseconds it would sort after
    // numbers that its own date sorts before.
    std::optional<NumericValue> sortNumber(const CellValue &value)
    {
        if (const auto *time = std::get_if<DateTime>(&value))
            return static_cast<double>(time->micros_since_epoch) / (86400.0 * 1000000);
        return numericValue(value);
    }

    int compareIntegerToDouble(int64_t integer, double number)
    {
        // Check the cast bounds before truncating; converting the integer to
        // double instead would collapse distinct values above 2^53.
        if (std::isnan(number) || number >= 0x1p63)
            return -1;
        if (number < -0x1p63)
            return 1;
        const int64_t truncated = static_cast<int64_t>(number);
        if (integer != truncated)
            return integer < truncated ? -1 : 1;
        const double integral = static_cast<double>(truncated);
        return integral < number ? -1 : (integral > number ? 1 : 0);
    }

    int compareNumbers(const NumericValue &a, const NumericValue &b)
    {
        const auto *ai = std::get_if<int64_t>(&a);
        const auto *bi = std::get_if<int64_t>(&b);
        if (ai && bi)
            return *ai < *bi ? -1 : (*ai > *bi ? 1 : 0);
        if (ai)
            return compareIntegerToDouble(*ai, std::get<double>(b));
        if (bi)
            return -compareIntegerToDouble(*bi, std::get<double>(a));
        const double ad = std::get<double>(a), bd = std::get<double>(b);
        const bool a_nan = std::isnan(ad), b_nan = std::isnan(bd);
        if (a_nan || b_nan)
            return a_nan == b_nan ? 0 : (a_nan ? 1 : -1);
        return ad < bd ? -1 : (ad > bd ? 1 : 0);
    }
} // namespace

int compareCells(const CellValue &a, const CellValue &b)
{
    const bool a_null = isNull(a), b_null = isNull(b);
    if (a_null || b_null)
        return a_null == b_null ? 0 : (a_null ? 1 : -1);
    if (const auto temporal = compareTemporal(a, b))
        return *temporal;
    const auto na = sortNumber(a), nb = sortNumber(b);
    if (na && nb)
        return compareNumbers(*na, *nb);
    if (na || nb)
        return na ? -1 : 1;
    return formatCell(a).compare(formatCell(b));
}

CellValue editedCellValue(const std::string &text, const CellValue &original, CellEditKind kind)
{
    if (kind != CellEditKind::Typed)
        return text;
    if (text.empty())
        return {};
    if (std::holds_alternative<std::string>(original))
        return text;
    if (std::holds_alternative<bool>(original))
    {
        if (const auto value = parseBoolean(text))
            return *value;
        return text;
    }
    if (std::holds_alternative<Date>(original) || std::holds_alternative<DateTime>(original))
    {
        const auto value = parseTemporal(text);
        if (!value)
            return text;
        constexpr int64_t micros_per_day = 86400LL * 1000000;
        if (!value->has_time && std::holds_alternative<Date>(original))
            return Date{static_cast<int32_t>(value->micros / micros_per_day)};
        return DateTime{value->micros};
    }
    // Numbers and empty cells.
    const size_t begin = text.find_first_not_of(" \t");
    const size_t end = text.find_last_not_of(" \t");
    if (begin != std::string::npos)
    {
        std::string_view digits = std::string_view(text).substr(begin, end - begin + 1);
        if (!digits.empty() && digits.front() == '+')
            digits.remove_prefix(1);
        int64_t integer = 0;
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), integer);
        if (!digits.empty() && result.ec == std::errc() && result.ptr == digits.data() + digits.size())
            return integer;
    }
    if (const auto number = parseNumber(text); number && std::isfinite(*number))
        return *number;
    return text;
}

bool isEmptyCell(const CellValue &value)
{
    return isNull(value);
}

namespace
{

    std::string foldCase(std::string text)
    {
        for (auto &c : text)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return text;
    }

    constexpr int64_t MICROS_PER_DAY = 86400LL * 1000000;

    // Filters are evaluated cell by cell, so their patterns are compiled once.
    // Each thread (worker, SQLite function) has its own cache.
    const re2::RE2 *filterRegex(const std::string &pattern)
    {
        thread_local std::map<std::string, std::unique_ptr<re2::RE2>> cache;
        if (const auto it = cache.find(pattern); it != cache.end())
            return it->second->ok() ? it->second.get() : nullptr;
        if (cache.size() >= 64)
            cache.clear();
        re2::RE2::Options options;
        options.set_case_sensitive(false);
        options.set_log_errors(false);
        auto regex = std::make_unique<re2::RE2>(pattern, options);
        const re2::RE2 *result = regex->ok() ? regex.get() : nullptr;
        cache.emplace(pattern, std::move(regex));
        return result;
    }

    // The instant of a temporal cell: a date (its midnight), a date-time, or
    // text holding one.
    std::optional<int64_t> cellInstant(const CellValue &cell)
    {
        if (const auto *date = std::get_if<Date>(&cell))
            return date->days_since_epoch * MICROS_PER_DAY;
        if (const auto *date_time = std::get_if<DateTime>(&cell))
            return date_time->micros_since_epoch;
        if (const auto *text = std::get_if<std::string>(&cell))
        {
            if (const auto value = parseTemporal(*text))
                return value->micros;
        }
        return std::nullopt;
    }

    // FilterOp::IsNumber, as histograms classify cells.
    bool isNumberCell(const CellValue &cell)
    {
        if (std::holds_alternative<int64_t>(cell))
            return true;
        if (const auto *real = std::get_if<double>(&cell))
            return std::isfinite(*real);
        if (const auto *text = std::get_if<std::string>(&cell))
        {
            const auto number = parseNumber(*text);
            return number && std::isfinite(*number);
        }
        return false;
    }

    // FilterOp::IsDate: numeric text is a number even if it reads as a date.
    bool isDateCell(const CellValue &cell)
    {
        if (std::holds_alternative<Date>(cell) || std::holds_alternative<DateTime>(cell))
            return true;
        const auto *text = std::get_if<std::string>(&cell);
        return text && !parseNumber(*text) && parseTemporal(*text);
    }

    bool matchesCondition(const CellValue &cell, const FilterCondition &condition)
    {
        switch (condition.op)
        {
        case FilterOp::IsEmpty:
            return isNull(cell);
        case FilterOp::IsNotEmpty:
            return !isNull(cell);
        case FilterOp::IsNumber:
            return isNumberCell(cell);
        case FilterOp::IsDate:
            return isDateCell(cell);
        case FilterOp::Contains:
            return foldCase(formatCell(cell)).find(foldCase(condition.value)) != std::string::npos;
        case FilterOp::NotContains:
            return foldCase(formatCell(cell)).find(foldCase(condition.value)) == std::string::npos;
        case FilterOp::Matches:
        case FilterOp::NotMatches:
        {
            // An invalid pattern matches nothing (filterError() reports it).
            const re2::RE2 *regex = filterRegex(condition.value);
            if (!regex)
                return false;
            return re2::RE2::PartialMatch(formatCell(cell), *regex) == (condition.op == FilterOp::Matches);
        }
        default:
            break;
        }

        // A date covers [start, end): its whole day; a date-time, one instant.
        // The value first: it fails fast, unlike every text cell.
        if (const auto value = parseTemporal(condition.value))
        {
            if (const auto instant = cellInstant(cell))
            {
                const int64_t start = value->micros;
                const int64_t end = start + (value->has_time ? 1 : MICROS_PER_DAY);
                switch (condition.op)
                {
                case FilterOp::Equals:
                    return *instant >= start && *instant < end;
                case FilterOp::NotEquals:
                    return *instant < start || *instant >= end;
                case FilterOp::Less:
                    return *instant < start;
                case FilterOp::LessEqual:
                    return *instant < end;
                case FilterOp::Greater:
                    return *instant >= end;
                case FilterOp::GreaterEqual:
                    return *instant >= start;
                default:
                    return false;
                }
            }
        }

        int cmp;
        const auto *boolean = std::get_if<bool>(&cell);
        const auto boolean_value = boolean ? parseBoolean(condition.value) : std::nullopt;
        if (boolean_value)
            cmp = static_cast<int>(*boolean) - static_cast<int>(*boolean_value);
        else
        {
            const auto na = numericValue(cell);
            const auto nb = numericValue(CellValue{condition.value});
            if (na && nb)
                cmp = compareNumbers(*na, *nb);
            else
                cmp = formatCell(cell).compare(condition.value);
        }

        switch (condition.op)
        {
        case FilterOp::Equals:
            return cmp == 0;
        case FilterOp::NotEquals:
            return cmp != 0;
        case FilterOp::Less:
            return cmp < 0;
        case FilterOp::LessEqual:
            return cmp <= 0;
        case FilterOp::Greater:
            return cmp > 0;
        case FilterOp::GreaterEqual:
            return cmp >= 0;
        default:
            return false;
        }
    }

    bool matchesCondition(const TableRow &row, const FilterCondition &condition)
    {
        return matchesCondition(cellAt(row, condition.column), condition);
    }

    bool matchesFilter(const TableRow &row, const std::optional<Filter> &filter)
    {
        if (!filter || filter->conditions.empty())
            return true;
        if (filter->combine == Filter::Combine::And)
        {
            return std::all_of(filter->conditions.begin(), filter->conditions.end(),
                               [&](const FilterCondition &c)
                               { return matchesCondition(row, c); });
        }
        return std::any_of(filter->conditions.begin(), filter->conditions.end(),
                           [&](const FilterCondition &c)
                           { return matchesCondition(row, c); });
    }

    // `regex` is the compiled spec.text for regex searches; otherwise spec.text
    // is already folded for a case-insensitive search.
    bool matchesSearch(const CellValue &cell, const SearchSpec &spec, const re2::RE2 *regex)
    {
        const std::string text = formatCell(cell);
        if (regex)
            return re2::RE2::PartialMatch(text, *regex);
        if (spec.case_sensitive)
            return text.find(spec.text) != std::string::npos;
        return foldCase(text).find(spec.text) != std::string::npos;
    }

    std::atomic<uint64_t> g_priority_clock{0};
} // namespace

bool cellMatchesCondition(const CellValue &cell, const FilterCondition &condition)
{
    return matchesCondition(cell, condition);
}

std::optional<std::string> filterError(const Filter &filter)
{
    for (const FilterCondition &condition : filter.conditions)
    {
        if (condition.op != FilterOp::Matches && condition.op != FilterOp::NotMatches)
            continue;
        re2::RE2::Options options;
        options.set_log_errors(false);
        const re2::RE2 regex(condition.value, options);
        if (!regex.ok())
            return "Invalid regular expression \"" + condition.value + "\": " + regex.error();
    }
    return std::nullopt;
}

namespace
{
    // The rows of a result sheet (TableBook::addResultSheet()). Sequential, so
    // that the model keeps every row and sorts and filters them in memory.
    class ResultSheetSource : public SheetSource
    {
    public:
        explicit ResultSheetSource(std::shared_ptr<const ResultSheet> sheet) : sheet_(std::move(sheet)) {}

        SourceCapabilities capabilities() const override { return {}; }
        std::vector<ColumnInfo> columns() override { return sheet_->columns; }
        RowCount rowCount() override { return {sheet_->rows.size(), RowCount::Kind::Exact}; }
        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token) override
        {
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            const size_t end = std::min(sheet_->rows.size(), first_row + std::min(max_rows, sheet_->rows.size()));
            for (size_t row = first_row; row < end; ++row)
                block->rows.push_back(sheet_->rows[row]);
            return block;
        }

    private:
        std::shared_ptr<const ResultSheet> sheet_;
    };
} // namespace

// ===========================================================================
// Scheduler: the worker loop shared by all sheets of a book
// ===========================================================================

namespace detail
{
    class Scheduler
    {
    private:
        std::mutex mutex_;
        std::condition_variable_any cv_;
        bool wake_pending_ = false;
        std::vector<std::weak_ptr<TableModel>> models_;
        std::deque<std::function<void(DataSource &, std::stop_token)>> tasks_;

    public:
        void post(std::function<void(DataSource &, std::stop_token)> task)
        {
            {
                std::lock_guard lock(mutex_);
                tasks_.push_back(std::move(task));
                wake_pending_ = true;
            }
            cv_.notify_one();
        }

        void add(std::weak_ptr<TableModel> model)
        {
            {
                std::lock_guard lock(mutex_);
                models_.push_back(std::move(model));
                wake_pending_ = true;
            }
            cv_.notify_one();
        }

        void wake()
        {
            {
                std::lock_guard lock(mutex_);
                wake_pending_ = true;
            }
            cv_.notify_one();
        }

        void run(std::stop_token stop, DataSource &data_source)
        {
            while (!stop.stop_requested())
            {
                std::vector<std::shared_ptr<TableModel>> live;
                std::deque<std::function<void(DataSource &, std::stop_token)>> tasks;
                {
                    std::lock_guard lock(mutex_);
                    wake_pending_ = false;
                    tasks.swap(tasks_);
                    std::erase_if(models_, [](const auto &weak)
                                  { return weak.expired(); });
                    for (const auto &weak : models_)
                    {
                        if (auto model = weak.lock())
                            live.push_back(std::move(model));
                    }
                }
                for (auto &task : tasks)
                {
                    if (stop.stop_requested())
                        return;
                    task(data_source, stop);
                }
                // The sheet whose viewport was requested most recently goes first.
                std::stable_sort(live.begin(), live.end(), [](const auto &a, const auto &b)
                                 { return a->priority() > b->priority(); });

                bool worked = false;
                for (const auto &model : live)
                {
                    if (stop.stop_requested())
                        return;
                    if (model->step(data_source, stop))
                    {
                        worked = true;
                        break;
                    }
                }
                live.clear();

                if (!worked)
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, stop, [this]
                             { return wake_pending_; });
                }
            }
        }
    };

    // The state of a TableModel::readView(), shared by the model and the
    // reader. `spec` and `next_row` belong to the model (under its mutex).
    class ViewScan
    {
    public:
        ViewSpec spec;
        size_t max_queued_rows = 0; // read ahead of the reader
        size_t next_row = 0;

        std::mutex mutex;
        std::condition_variable_any cv;
        std::deque<ViewRows> chunks;
        size_t queued_rows = 0;
        bool done = false;
        std::optional<std::string> error;

        bool wantsRows()
        {
            std::lock_guard lock(mutex);
            return !done && !error && queued_rows < max_queued_rows;
        }

        void push(ViewRows rows, bool last)
        {
            {
                std::lock_guard lock(mutex);
                queued_rows += rows.rows.size();
                if (!rows.rows.empty())
                    chunks.push_back(std::move(rows));
                done = last;
            }
            cv.notify_all();
        }

        void fail(std::string message)
        {
            {
                std::lock_guard lock(mutex);
                if (!done && !error)
                    error = std::move(message);
            }
            cv.notify_all();
        }
    };
} // namespace detail

// ===========================================================================
// TableModel: UI-facing side
// ===========================================================================

TableModel::TableModel(std::string name, Options options, std::weak_ptr<detail::Scheduler> scheduler)
    : name_(std::move(name)), options_(options), scheduler_(std::move(scheduler)),
      next_memory_checkpoint_(nextMemoryCheckpoint(options_, 0))
{
    priority_ = ++g_priority_clock;
}

TableModel::~TableModel() = default;

void TableModel::wakeWorker()
{
    if (auto scheduler = scheduler_.lock())
        scheduler->wake();
}

CellRef TableModel::peek(size_t view_row, size_t column)
{
    bool wake = false;
    CellRef result;
    {
        std::lock_guard lock(mutex_);
        if (state_ == ModelState::Error)
            return {CellState::Error, nullptr, nullptr};

        size_t source_row = view_row;
        if (view_rows_)
        {
            if (view_row >= view_rows_->size())
                return {CellState::OutOfRange, nullptr, nullptr};
            source_row = (*view_rows_)[view_row];
        }
        else if (source_row_count_.isExact() && source_row >= source_row_count_.value)
        {
            return {CellState::OutOfRange, nullptr, nullptr};
        }

        const size_t block_index = source_row / options_.block_rows;
        auto it = blocks_.find(block_index);
        if (it == blocks_.end())
        {
            const bool in_viewport = viewport_ && view_row >= viewport_->first && view_row <= viewport_->second;
            if (!in_viewport && adhoc_block_ != block_index)
            {
                adhoc_block_ = block_index;
                wake = true;
            }
            result = {CellState::Loading, nullptr, nullptr};
        }
        else
        {
            it->second.last_used = ++use_clock_;
            const RowBlockPtr &block = it->second.block;
            const size_t offset = source_row - block->first_row;
            if (offset >= block->rows.size())
                return {CellState::OutOfRange, nullptr, nullptr};
            const TableRow &row = block->rows[offset];
            if (column >= row.size())
                return {CellState::Empty, block, nullptr};
            result = {CellState::Ready, block, &row[column]};
        }
    }
    if (wake)
        wakeWorker();
    return result;
}

void TableModel::requestRange(size_t first, size_t last)
{
    if (last < first)
        std::swap(first, last);
    {
        std::lock_guard lock(mutex_);
        if (viewport_ && viewport_->first == first && viewport_->second == last)
            return;
        viewport_ = {first, last};
    }
    priority_ = ++g_priority_clock;
    wakeWorker();
}

ModelState TableModel::state() const
{
    std::lock_guard lock(mutex_);
    return state_;
}

std::string TableModel::errorMessage() const
{
    std::lock_guard lock(mutex_);
    return error_message_;
}

std::optional<std::string> TableModel::takeViewError()
{
    std::lock_guard lock(mutex_);
    return std::exchange(view_error_, std::nullopt);
}

RowCount TableModel::rowCount() const
{
    std::lock_guard lock(mutex_);
    if (view_rows_)
        return {view_rows_->size(), RowCount::Kind::Exact};
    return source_row_count_;
}

RowCount TableModel::unfilteredRowCount() const
{
    std::lock_guard lock(mutex_);
    // In-memory views leave source_row_count_ alone.
    if (!capabilities_.query_pushdown || !view_.filtered())
        return source_row_count_;
    return unfiltered_row_count_;
}

size_t TableModel::displayRowCount() const
{
    std::lock_guard lock(mutex_);
    if (view_rows_)
        return view_rows_->size();
    if (source_row_count_.isExact())
        return source_row_count_.value;
    if (state_ != ModelState::Ready)
        return 0;
    return source_row_count_.value + options_.block_rows;
}

size_t TableModel::columnCount() const
{
    std::lock_guard lock(mutex_);
    return std::max(column_count_, columns_.size());
}

std::string TableModel::columnName(size_t column) const
{
    std::lock_guard lock(mutex_);
    if (column < columns_.size() && !columns_[column].name.empty())
        return columns_[column].name;
    return columnLetter(column);
}

bool TableModel::hasHeader() const
{
    std::lock_guard lock(mutex_);
    return !columns_.empty();
}

std::vector<CellRange> TableModel::mergedCells() const
{
    std::lock_guard lock(mutex_);
    return merged_cells_;
}

std::vector<ColumnType> TableModel::columnTypes() const
{
    std::lock_guard lock(mutex_);
    std::vector<ColumnType> types(std::max(column_count_, columns_.size()));
    for (size_t c = 0; c < types.size() && c < type_counters_.size(); ++c)
        types[c] = type_counters_[c].type();
    return types;
}

Subscription TableModel::subscribe(Listener listener)
{
    auto entry = std::make_shared<ListenerEntry>(std::move(listener));
    {
        std::lock_guard lock(listeners_mutex_);
        listeners_.push_back(entry);
    }
    return Subscription([weak = weak_from_this(), entry = std::move(entry)]
                        {
        if (auto self = weak.lock())
        {
            {
                std::lock_guard lock(self->listeners_mutex_);
                std::erase(self->listeners_, entry);
            }
            std::unique_lock lock(entry->mutex);
            entry->active = false;
            if (entry->running_thread != std::this_thread::get_id())
                entry->cv.wait(lock, [&] { return entry->in_flight == 0; });
        } });
}

void TableModel::emit(const std::vector<TableEvent> &events)
{
    if (events.empty())
        return;

    std::vector<std::shared_ptr<ListenerEntry>> listeners;
    {
        std::lock_guard lock(listeners_mutex_);
        listeners = listeners_;
    }
    for (const auto &event : events)
    {
        for (const auto &entry : listeners)
        {
            {
                std::lock_guard lock(entry->mutex);
                if (!entry->active)
                    continue;
                ++entry->in_flight;
                entry->running_thread = std::this_thread::get_id();
            }
            try
            {
                entry->listener(event);
            }
            catch (...)
            {
                std::lock_guard lock(entry->mutex);
                --entry->in_flight;
                entry->running_thread = {};
                entry->cv.notify_all();
                throw;
            }
            {
                std::lock_guard lock(entry->mutex);
                --entry->in_flight;
                entry->running_thread = {};
                entry->cv.notify_all();
            }
        }
    }
}

ViewCapabilities TableModel::viewCapabilities() const
{
    std::lock_guard lock(mutex_);
    if (state_ != ModelState::Ready)
        return {false, false, false};
    // Without pushdown, every row is read into memory first.
    return {true, true, true};
}

void TableModel::setView(ViewSpec spec)
{
    {
        std::lock_guard lock(mutex_);
        if (!pending_view_ && spec == view_)
            return;
        pending_view_ = std::move(spec);
    }
    wakeWorker();
}

ViewSpec TableModel::view() const
{
    std::lock_guard lock(mutex_);
    return view_;
}

ViewSpec TableModel::requestedView() const
{
    std::lock_guard lock(mutex_);
    return pending_view_ ? *pending_view_ : view_;
}

void TableModel::startSearch(SearchSpec spec)
{
    if (spec.text.empty())
    {
        cancelSearch();
        return;
    }
    std::shared_ptr<const re2::RE2> regex;
    std::string error;
    if (spec.regex)
    {
        re2::RE2::Options re_options;
        re_options.set_case_sensitive(spec.case_sensitive);
        re_options.set_log_errors(false);
        regex = std::make_shared<const re2::RE2>(spec.text, re_options);
        if (!regex->ok())
        {
            error = "Invalid regular expression: " + regex->error();
            regex.reset();
        }
    }
    else if (!spec.case_sensitive)
    {
        spec.text = foldCase(spec.text);
    }
    {
        std::lock_guard lock(mutex_);
        search_ = {};
        search_.active = true;
        search_next_row_ = 0;
        ++search_generation_;
        if (!error.empty())
        {
            search_spec_.reset();
            search_regex_.reset();
            search_.done = true;
            search_.error = std::move(error);
            return;
        }
        search_spec_ = std::move(spec);
        search_regex_ = std::move(regex);
    }
    wakeWorker();
}

void TableModel::cancelSearch()
{
    std::lock_guard lock(mutex_);
    search_spec_.reset();
    search_regex_.reset();
    search_ = {};
    search_next_row_ = 0;
    ++search_generation_;
}

bool searchHitBefore(const CellPosition &a, const CellPosition &b)
{
    return a.row < b.row || (a.row == b.row && a.col < b.col);
}

std::optional<size_t> searchHitIndex(const SearchState &state, CellPosition position)
{
    const auto it = std::lower_bound(state.hits.begin(), state.hits.end(), position, searchHitBefore);
    if (it == state.hits.end() || !(*it == position))
        return std::nullopt;
    return static_cast<size_t>(it - state.hits.begin());
}

SearchState TableModel::searchState() const
{
    std::lock_guard lock(mutex_);
    SearchState state = search_;
    state.generation = search_generation_;
    return state;
}

// A pushdown source counts the values itself (e.g. one GROUP BY), which is
// much faster than reading every row page by page.
bool TableModel::stepStatsPushdown(std::unique_lock<std::mutex> &lock, std::stop_token stop,
                                   std::vector<TableEvent> &events)
{
    stats_pushdown_tried_ = true;
    const size_t column = stats_.column;
    const std::shared_ptr<ColumnStatsAccumulator> accumulator = stats_accumulator_;
    const uint64_t generation = stats_generation_;
    lock.unlock();

    std::optional<ColumnStats> result;
    try
    {
        if (const auto distinct = source_->columnValueCounts(
                column, [&accumulator](const CellValue &value, size_t count)
                { accumulator->add(value, count); }, stop))
        {
            result = accumulator->result();
            // Exact even when the accumulator stopped counting new values.
            result->distinct = *distinct;
            result->distinct_exact = true;
        }
    }
    catch (const std::exception &)
    {
        // E.g. the filter fails on some value: scanning reads the rows, which
        // reports the error as reading the sheet does.
    }

    lock.lock();
    if (generation != stats_generation_)
        return true;
    if (!result)
    {
        // The source may have added some values before it gave up.
        stats_accumulator_ = std::make_shared<ColumnStatsAccumulator>(options_.stats_limits);
        return true;
    }
    stats_.stats = std::move(*result);
    stats_.scanned_rows = stats_.stats.rows;
    stats_.done = true;
    events.push_back({TableEvent::Kind::StatsProgress, 0, stats_.scanned_rows});
    return true;
}

void TableModel::startColumnStats(size_t column)
{
    {
        std::lock_guard lock(mutex_);
        stats_.column = column;
        restartStatsLocked();
    }
    wakeWorker();
}

void TableModel::restartStatsLocked()
{
    const size_t column = stats_.column;
    stats_ = {};
    stats_.active = true;
    stats_.column = column;
    stats_next_row_ = 0;
    stats_pushdown_tried_ = false;
    ++stats_generation_;
    // A new accumulator: the worker may still be adding to the old one.
    stats_accumulator_ = std::make_shared<ColumnStatsAccumulator>(options_.stats_limits);
    stats_published_ = std::chrono::steady_clock::now();
}

void TableModel::cancelColumnStats()
{
    std::lock_guard lock(mutex_);
    stats_accumulator_.reset();
    stats_ = {};
    stats_next_row_ = 0;
    ++stats_generation_;
}

std::optional<TableModel::MemoryCheckpoint> TableModel::memoryCheckpoint() const
{
    std::lock_guard lock(mutex_);
    if (!memory_paused_)
        return std::nullopt;
    return MemoryCheckpoint{cache_bytes_, sequential_rows_};
}

size_t TableModel::nextMemoryCheckpoint(const Options &options, size_t bytes)
{
    for (const size_t checkpoint : options.memory_checkpoints)
    {
        if (checkpoint > bytes)
            return checkpoint;
    }
    if (options.memory_checkpoint_interval == 0)
        return 0;
    return (bytes / options.memory_checkpoint_interval + 1) * options.memory_checkpoint_interval;
}

void TableModel::continueReading()
{
    {
        std::lock_guard lock(mutex_);
        if (!memory_paused_)
            return;
        memory_paused_ = false;
        next_memory_checkpoint_ = nextMemoryCheckpoint(options_, cache_bytes_);
    }
    wakeWorker();
}

void TableModel::stopReading()
{
    {
        std::lock_guard lock(mutex_);
        if (!memory_paused_)
            return;
        memory_paused_ = false;
        stop_reading_pending_ = true;
    }
    wakeWorker();
}

bool TableModel::readingStopped() const
{
    std::lock_guard lock(mutex_);
    return reading_stopped_;
}

std::unique_ptr<ViewReader> TableModel::readView()
{
    auto scan = std::make_shared<detail::ViewScan>();
    {
        std::lock_guard lock(mutex_);
        failScanLocked("Another reader started reading the view");
        scan->spec = pending_view_ ? *pending_view_ : view_;
        scan->max_queued_rows = 2 * std::max(options_.scan_read_rows, options_.block_rows);
        if (state_ == ModelState::Error)
            scan->fail(error_message_);
        else if (detached_)
            scan->fail("The file was closed"); // no worker would ever fill it
        else
            scan_ = scan;
    }
    wakeWorker();
    return std::make_unique<ViewReader>(shared_from_this(), std::move(scan));
}

ViewReader::ViewReader(std::shared_ptr<TableModel> model, std::shared_ptr<detail::ViewScan> scan)
    : model_(std::move(model)), scan_(std::move(scan))
{
}

ViewReader::~ViewReader()
{
    std::lock_guard lock(model_->mutex_);
    if (model_->scan_ == scan_)
        model_->scan_.reset();
}

std::optional<ViewRows> ViewReader::next(std::stop_token stop)
{
    std::unique_lock lock(scan_->mutex);
    scan_->cv.wait(lock, stop, [this]
                   { return !scan_->chunks.empty() || scan_->done || scan_->error; });
    if (stop.stop_requested())
        return std::nullopt;
    if (!scan_->chunks.empty())
    {
        ViewRows rows = std::move(scan_->chunks.front());
        scan_->chunks.pop_front();
        scan_->queued_rows -= rows.rows.size();
        lock.unlock();
        // The worker waits for room in the queue.
        model_->wakeWorker();
        return rows;
    }
    if (scan_->error)
        throw std::runtime_error(*scan_->error);
    return ViewRows{};
}

ColumnStatsState TableModel::columnStatsState() const
{
    std::lock_guard lock(mutex_);
    ColumnStatsState state = stats_;
    state.generation = stats_generation_;
    return state;
}

// ===========================================================================
// TableModel: editing
// ===========================================================================

CellEditKind TableModel::editKind() const
{
    std::lock_guard lock(mutex_);
    if (state_ != ModelState::Ready || capabilities_.random_access)
        return CellEditKind::None;
    return capabilities_.edits;
}

bool TableModel::editMode() const
{
    std::lock_guard lock(mutex_);
    return editing_ && state_ == ModelState::Ready && !capabilities_.random_access &&
           capabilities_.edits != CellEditKind::None;
}

std::optional<size_t> TableModel::sourceRowOfLocked(size_t view_row) const
{
    if (!view_rows_)
        return view_row;
    if (view_row >= view_rows_->size())
        return std::nullopt;
    return (*view_rows_)[view_row];
}

std::optional<size_t> TableModel::viewRowOfLocked(size_t source_row) const
{
    if (!view_rows_)
        return source_row;
    const auto it = std::find(view_rows_->begin(), view_rows_->end(), source_row);
    if (it == view_rows_->end())
        return std::nullopt;
    return static_cast<size_t>(it - view_rows_->begin());
}

bool TableModel::cellEditableLocked(size_t source_row, size_t column) const
{
    // Random-access sources may evict the edited blocks.
    if (!editing_ || state_ != ModelState::Ready || capabilities_.edits == CellEditKind::None ||
        capabilities_.random_access || column >= std::max(column_count_, columns_.size()))
        return false;
    const auto it = blocks_.find(source_row / options_.block_rows);
    if (it == blocks_.end())
        return false;
    const RowBlock &block = *it->second.block;
    const size_t offset = source_row - block.first_row;
    if (offset >= block.rows.size() ||
        std::binary_search(block.locked_cells.begin(), block.locked_cells.end(), std::pair{offset, column}))
        return false;
    for (const CellRange &range : merged_cells_)
    {
        if (source_row >= range.row && source_row - range.row < range.rows && column >= range.col &&
            column - range.col < range.cols)
            return source_row == range.row && column == range.col;
    }
    return true;
}

bool TableModel::cellEditable(size_t view_row, size_t column) const
{
    std::lock_guard lock(mutex_);
    const auto source_row = sourceRowOfLocked(view_row);
    return source_row && cellEditableLocked(*source_row, column);
}

void TableModel::applyChangesLocked(const std::vector<CellChange> &changes, bool forward)
{
    // Blocks are shared with readers, so each one changed is copied once.
    std::map<size_t, std::shared_ptr<RowBlock>> copies;
    auto apply = [&](const CellChange &change)
    {
        const size_t index = change.row / options_.block_rows;
        const auto cached = blocks_.find(index);
        if (cached == blocks_.end())
            return;
        auto &copy = copies[index];
        if (!copy)
            copy = std::make_shared<RowBlock>(*cached->second.block);
        TableRow &row = copy->rows[change.row - copy->first_row];
        if (change.column >= row.size())
            row.resize(change.column + 1);
        row[change.column] = forward ? change.after : change.before;
        // A ragged row lengthened by an edit gets its length back once the
        // cells added are empty again.
        const size_t original_size = original_rows_.at(change.row).size();
        while (row.size() > original_size && std::holds_alternative<std::monostate>(row.back()))
            row.pop_back();
    };
    if (forward)
        std::for_each(changes.begin(), changes.end(), apply);
    else
        std::for_each(changes.rbegin(), changes.rend(), apply);
    for (auto &[index, copy] : copies)
    {
        CachedBlock &cached = blocks_[index];
        cache_bytes_ -= std::min(cache_bytes_, cached.block->approxBytes());
        cache_bytes_ += copy->approxBytes();
        cached.block = std::move(copy);
    }
}

void TableModel::restartSearchLocked()
{
    if (!search_spec_)
        return;
    search_ = {};
    search_.active = true;
    search_next_row_ = 0;
    ++search_generation_;
}

void TableModel::cellsChangedLocked()
{
    restartSearchLocked();
    if (stats_accumulator_)
        restartStatsLocked();
    if (histogram_.active)
        restartHistogramLocked();
    if (scatter_.active)
        restartScatterLocked();
    failScanLocked("The table was edited while its rows were being read");
    // Listeners run on the worker thread.
    if (auto scheduler = scheduler_.lock())
    {
        scheduler->post([weak = weak_from_this()](DataSource &, std::stop_token)
                        {
            if (auto self = weak.lock())
                self->emit({{TableEvent::Kind::CellsChanged}}); });
    }
}

size_t TableModel::setCells(const std::vector<CellText> &cells)
{
    size_t changed = 0;
    {
        std::lock_guard lock(mutex_);
        std::vector<CellChange> changes;
        // The values set so far: the same cell may come twice.
        std::map<std::pair<size_t, size_t>, CellValue> values;
        for (const CellText &cell : cells)
        {
            const auto source_row = sourceRowOfLocked(cell.row);
            if (!source_row || !cellEditableLocked(*source_row, cell.column))
                continue;
            const TableRow &row = *sourceRowLocked(*source_row);
            const auto set = values.find({*source_row, cell.column});
            CellValue before = set != values.end() ? set->second : cellAt(row, cell.column);
            CellValue after = editedCellValue(cell.text, before, capabilities_.edits);
            if (after == before || (isEmptyCell(after) && isEmptyCell(before)))
                continue;
            original_rows_.try_emplace(*source_row, row);
            values[{*source_row, cell.column}] = after;
            changes.push_back({*source_row, cell.column, std::move(before), std::move(after)});
        }
        if (changes.empty())
            return 0;
        changed = changes.size();
        applyChangesLocked(changes, true);
        edits_.resize(edit_pos_);
        edits_.push_back(std::move(changes));
        ++edit_pos_;
        cellsChangedLocked();
    }
    wakeWorker();
    return changed;
}

bool TableModel::stepEdits(bool forward, CellPosition *cell)
{
    {
        std::lock_guard lock(mutex_);
        if (!editing_ || (forward ? edit_pos_ >= edits_.size() : edit_pos_ == 0))
            return false;
        const std::vector<CellChange> &changes = edits_[forward ? edit_pos_ : edit_pos_ - 1];
        applyChangesLocked(changes, forward);
        if (forward)
            ++edit_pos_;
        else
            --edit_pos_;
        if (cell && !changes.empty())
        {
            if (const auto view_row = viewRowOfLocked(changes.front().row))
                *cell = {*view_row, changes.front().column};
        }
        cellsChangedLocked();
    }
    wakeWorker();
    return true;
}

bool TableModel::undo(CellPosition *cell)
{
    return stepEdits(false, cell);
}

bool TableModel::redo(CellPosition *cell)
{
    return stepEdits(true, cell);
}

bool TableModel::canUndo() const
{
    std::lock_guard lock(mutex_);
    return editing_ && edit_pos_ > 0;
}

bool TableModel::canRedo() const
{
    std::lock_guard lock(mutex_);
    return editing_ && edit_pos_ < edits_.size();
}

bool TableModel::modified() const
{
    std::lock_guard lock(mutex_);
    if (edit_pos_ == 0)
        return false;
    for (const auto &[source_row, original] : original_rows_)
    {
        const TableRow *row = sourceRowLocked(source_row);
        if (row && *row != original)
            return true;
    }
    return false;
}

bool TableModel::cellModified(size_t view_row, size_t column) const
{
    std::lock_guard lock(mutex_);
    const auto source_row = sourceRowOfLocked(view_row);
    if (!source_row)
        return false;
    const auto original = original_rows_.find(*source_row);
    if (original == original_rows_.end())
        return false;
    const TableRow *row = sourceRowLocked(*source_row);
    return row && cellAt(*row, column) != cellAt(original->second, column);
}

void TableModel::setEditing(bool editing)
{
    std::lock_guard lock(mutex_);
    editing_ = editing;
}

void TableModel::discardEdits()
{
    std::lock_guard lock(mutex_);
    if (edits_.empty())
        return;
    while (edit_pos_ > 0)
        applyChangesLocked(edits_[--edit_pos_], false);
    edits_.clear();
    cellsChangedLocked();
}

SheetEdits TableModel::sheetEdits() const
{
    std::lock_guard lock(mutex_);
    SheetEdits edits;
    edits.sheet = name_;
    for (const auto &[source_row, original] : original_rows_)
    {
        const TableRow *row = sourceRowLocked(source_row);
        if (row && *row != original)
            edits.rows.push_back({source_row, original, *row});
    }
    return edits;
}

// ===========================================================================
// TableModel: worker side
// ===========================================================================

void TableModel::detachSource()
{
    source_.reset();
    std::lock_guard lock(mutex_);
    detached_ = true;
    failScanLocked("The file was closed");
}

void TableModel::retire()
{
    source_.reset();
    std::lock_guard lock(mutex_);
    state_ = ModelState::Error;
    error_message_ = "The sheet was replaced";
    failScanLocked(error_message_);
}

AppendResult TableModel::readAppendedRows(std::stop_token stop)
{
    {
        std::lock_guard lock(mutex_);
        // Rows still being read may include the new ones; a table cut short
        // (stopReading()) stays so.
        if (!source_ || state_ != ModelState::Ready || !eof_ || reading_stopped_)
            return AppendResult::None;
        if (capabilities_.random_access)
            return AppendResult::Changed;
    }
    const AppendResult result = source_->readAppended(stop);
    if (result != AppendResult::Appended)
        return result;

    std::vector<TableEvent> events;
    const size_t B = options_.block_rows;
    std::unique_lock lock(mutex_);
    const size_t first_new = sequential_rows_;
    const size_t old_column_count = std::max(column_count_, columns_.size());
    while (!stop.stop_requested())
    {
        // The last block may be partly filled: the new rows go on in a copy
        // of it, since readers share it.
        const size_t start = sequential_rows_;
        const size_t block_index = start / B;
        const auto it = blocks_.find(block_index);
        const RowBlockPtr old = it != blocks_.end() ? it->second.block : nullptr;
        const size_t want = (block_index + 1) * B - start;
        lock.unlock();
        RowBlockPtr more = source_->read(start, want, stop);
        lock.lock();
        if (!more || more->rows.empty())
            break;
        auto merged = std::make_shared<RowBlock>();
        merged->first_row = block_index * B;
        if (old)
        {
            merged->rows = old->rows;
            merged->locked_cells = old->locked_cells;
            cache_bytes_ -= old->approxBytes();
        }
        for (const TableRow &row : more->rows)
        {
            column_count_ = std::max(column_count_, row.size());
            merged->rows.push_back(row);
        }
        cache_bytes_ += merged->approxBytes();
        blocks_[block_index] = {std::move(merged), ++use_clock_};
        sequential_rows_ += more->rows.size();
        if (more->rows.size() < want)
            break;
    }
    if (sequential_rows_ > first_new)
    {
        source_row_count_ = {sequential_rows_, RowCount::Kind::Exact};
        events.push_back({TableEvent::Kind::RowCountChanged});
        events.push_back({TableEvent::Kind::RowsLoaded, first_new, sequential_rows_});
        if (std::max(column_count_, columns_.size()) != old_column_count)
            events.push_back({TableEvent::Kind::ColumnsChanged});
        // A sort or filter takes the new rows in when applied again.
        if (!view_.empty() && !pending_view_)
            pending_view_ = view_;
        restartSearchLocked();
        if (stats_accumulator_)
            restartStatsLocked();
        if (histogram_.active)
            restartHistogramLocked();
        if (scatter_.active)
            restartScatterLocked();
    }
    lock.unlock();
    emit(events);
    return AppendResult::Appended;
}

bool TableModel::step(DataSource &data_source, std::stop_token stop)
{
    std::vector<TableEvent> events;
    bool worked = false;
    // The view a pushdown source was running when it threw, if any.
    std::optional<ViewSpec> failed_view;
    try
    {
        std::unique_lock lock(mutex_);
        if (state_ == ModelState::Error)
            return false;
        if (state_ == ModelState::Opening)
        {
            lock.unlock();
            worked = stepOpen(data_source, events);
        }
        else if (stop_reading_pending_)
        {
            stop_reading_pending_ = false;
            // As if the file ended after the rows read so far.
            reading_stopped_ = true;
            eof_ = true;
            source_row_count_ = {sequential_rows_, RowCount::Kind::Exact};
            events.push_back({TableEvent::Kind::RowCountChanged});
            worked = true;
        }
        else if (capabilities_.query_pushdown)
        {
            // Rows of the old view would be discarded, and the view to apply
            // may be the fallback after the old one failed, so apply it first.
            if (pending_view_)
            {
                failed_view = *pending_view_;
                worked = stepApplyView(lock, stop, events);
            }
            else
            {
                failed_view = view_;
                if (auto block_index = nextBlockToLoad())
                    worked = loadBlock(*block_index, lock, stop, events);
                else if (scanWantsRowsLocked())
                    worked = stepScan(lock, stop, events);
                else if (search_spec_ && !search_.done)
                    worked = stepSearch(lock, stop, events);
                else if (stats_accumulator_ && !stats_.done)
                    worked = stepStats(lock, stop, events);
                else if (histogram_job_)
                    worked = stepHistogram(lock, stop, events);
                else if (scatter_job_)
                    worked = stepScatter(lock, stop, events);
                else
                    worked = stepCount(lock, stop, events);
            }
        }
        else if (auto block_index = nextBlockToLoad())
            worked = loadBlock(*block_index, lock, stop, events);
        else if (pending_view_)
            worked = stepApplyView(lock, stop, events);
        else if (scanWantsRowsLocked())
            worked = stepScan(lock, stop, events);
        else if (search_spec_ && !search_.done)
            worked = stepSearch(lock, stop, events);
        else if (stats_accumulator_ && !stats_.done)
            worked = stepStats(lock, stop, events);
        else if (histogram_job_)
            worked = stepHistogram(lock, stop, events);
        else if (scatter_job_)
            worked = stepScatter(lock, stop, events);
        else
            worked = stepCount(lock, stop, events);
    }
    catch (const std::exception &e)
    {
        std::lock_guard lock(mutex_);
        failScanLocked(e.what());
        if (failed_view && !failed_view->empty())
        {
            // A pushed-down sort/filter may fail only when its query runs
            // (e.g. a cast error in SQL). Fall back to the plain table rather
            // than breaking the sheet, so that the user can change the view.
            // Failing again on the plain table makes the error final.
            if (!pending_view_ || *pending_view_ == *failed_view)
                pending_view_ = ViewSpec{};
            view_error_ = e.what();
        }
        else
        {
            state_ = ModelState::Error;
            error_message_ = e.what();
        }
        events.push_back({TableEvent::Kind::Error, 0, 0, e.what()});
        worked = true;
    }
    emit(events);
    return worked;
}

bool TableModel::stepOpen(DataSource &data_source, std::vector<TableEvent> &events)
{
    source_ = preset_source_ ? std::move(preset_source_) : data_source.openSheet(name_);
    if (!source_)
        throw std::runtime_error("No such sheet: " + name_);
    const auto capabilities = source_->capabilities();
    auto columns = source_->columns();
    const auto row_count = source_->rowCount();
    auto merged = source_->mergedCells();

    std::lock_guard lock(mutex_);
    capabilities_ = capabilities;
    columns_ = std::move(columns);
    column_count_ = columns_.size();
    source_row_count_ = row_count;
    merged_cells_ = std::move(merged);
    state_ = ModelState::Ready;
    events.push_back({TableEvent::Kind::Opened});
    events.push_back({TableEvent::Kind::RowCountChanged});
    events.push_back({TableEvent::Kind::ColumnsChanged});
    return true;
}

std::optional<size_t> TableModel::nextBlockToLoad() const
{
    if (view_rows_)
        return std::nullopt; // in-memory views only exist when every row is cached

    const size_t B = options_.block_rows;
    const auto [first, last] = viewport_.value_or(std::pair<size_t, size_t>{0, B - 1});

    auto missing = [&](size_t block_index)
    {
        const size_t first_row = block_index * B;
        if (source_row_count_.isExact() && first_row >= source_row_count_.value)
            return false;
        if (eof_ && first_row >= sequential_rows_)
            return false;
        return !blocks_.contains(block_index);
    };
    auto next = [&](size_t block_index) -> size_t
    {
        // Sequential sources can only continue where they stopped.
        return capabilities_.random_access ? block_index : sequential_rows_ / B;
    };

    for (size_t b = first / B; b <= last / B; ++b)
    {
        if (missing(b))
            return next(b);
    }
    if (adhoc_block_ && missing(*adhoc_block_))
        return next(*adhoc_block_);

    const size_t span = std::max(last - first + 1, B);
    const size_t prefetch_last = last > std::numeric_limits<size_t>::max() - span ? last : last + span;
    for (size_t b = last / B + 1; b <= prefetch_last / B; ++b)
    {
        if (missing(b))
            return next(b);
    }
    for (size_t b = (first - std::min(first, span)) / B; b < first / B; ++b)
    {
        if (missing(b))
            return next(b);
    }
    return std::nullopt;
}

bool TableModel::loadBlock(size_t block_index, std::unique_lock<std::mutex> &lock, std::stop_token stop,
                           std::vector<TableEvent> &events)
{
    if (memory_paused_)
        return false; // until continueReading() or stopReading()
    const size_t B = options_.block_rows;
    const size_t first_row = block_index * B;

    lock.unlock();
    RowBlockPtr block = source_->read(first_row, B, stop);
    if (!block)
    {
        auto empty = std::make_shared<RowBlock>();
        empty->first_row = first_row;
        block = std::move(empty);
    }
    auto columns = source_->columns();
    // Counted before taking the lock; only the worker changes typed_rows_.
    std::vector<ColumnTypeCounter> type_counters;
    const size_t typed_rows = std::min(block->rows.size(), options_.type_sample_rows - std::min(typed_rows_, options_.type_sample_rows));
    for (size_t r = 0; r < typed_rows; ++r)
    {
        const TableRow &row = block->rows[r];
        if (type_counters.size() < row.size())
            type_counters.resize(row.size());
        for (size_t c = 0; c < row.size(); ++c)
            type_counters[c].add(row[c]);
    }
    lock.lock();

    if (stop.stop_requested())
        return true;

    if (typed_rows > 0)
    {
        typed_rows_ += typed_rows;
        if (type_counters_.size() < type_counters.size())
            type_counters_.resize(type_counters.size());
        for (size_t c = 0; c < type_counters.size(); ++c)
            type_counters_[c].merge(type_counters[c]);
    }

    const size_t n = block->rows.size();
    size_t widest = 0;
    for (const auto &row : block->rows)
        widest = std::max(widest, row.size());

    cache_bytes_ += block->approxBytes();
    blocks_[block_index] = {block, ++use_clock_};
    if (adhoc_block_ == block_index)
        adhoc_block_.reset();

    const size_t old_column_count = std::max(column_count_, columns_.size());
    columns_ = std::move(columns);
    column_count_ = std::max(column_count_, widest);
    if (std::max(column_count_, columns_.size()) != old_column_count)
        events.push_back({TableEvent::Kind::ColumnsChanged});

    const RowCount old_count = source_row_count_;
    if (!capabilities_.random_access)
    {
        sequential_rows_ += n;
        if (n < B)
        {
            eof_ = true;
            source_row_count_ = {sequential_rows_, RowCount::Kind::Exact};
        }
        else if (!source_row_count_.isExact())
        {
            source_row_count_ = {sequential_rows_, RowCount::Kind::AtLeast};
        }
    }
    else if (!source_row_count_.isExact())
    {
        // An empty block right where the known rows end (they fill whole
        // blocks) ends the table; further on it only bounds it. Otherwise
        // an in-memory view, which waits for an exact count, would load
        // empty blocks forever until countRowsSlow() gives one, if ever.
        if (n < B && (n > 0 || first_row <= source_row_count_.value))
            source_row_count_ = {first_row + n, RowCount::Kind::Exact};
        else if (first_row + n > source_row_count_.value)
            source_row_count_ = {first_row + n, RowCount::Kind::AtLeast};
    }
    if (!(source_row_count_ == old_count))
        events.push_back({TableEvent::Kind::RowCountChanged});
    if (n > 0)
        events.push_back({TableEvent::Kind::RowsLoaded, first_row, first_row + n});

    if (capabilities_.random_access)
        evictLocked(block_index);
    else if (next_memory_checkpoint_ > 0 && !eof_ && cache_bytes_ >= next_memory_checkpoint_)
    {
        memory_paused_ = true;
        events.push_back({TableEvent::Kind::MemoryCheckpoint, 0, sequential_rows_});
    }
    return true;
}

void TableModel::evictLocked(std::optional<size_t> keep_block)
{
    const bool building_view = pending_view_ && !pending_view_->empty() && !capabilities_.query_pushdown;
    if (cache_bytes_ <= options_.cache_bytes || view_rows_ || building_view)
        return;
    const size_t B = options_.block_rows;
    const auto [first, last] = viewport_.value_or(std::pair<size_t, size_t>{0, B - 1});
    const size_t span = std::max(last - first + 1, B);
    const size_t keep_first = (first - std::min(first, span)) / B;
    const size_t keep_last = (last + span) / B;

    while (cache_bytes_ > options_.cache_bytes)
    {
        auto victim = blocks_.end();
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it)
        {
            if (keep_block && it->first == *keep_block)
                continue;
            if (it->first >= keep_first && it->first <= keep_last)
                continue;
            if (victim == blocks_.end() || it->second.last_used < victim->second.last_used)
                victim = it;
        }
        if (victim == blocks_.end())
            break;
        cache_bytes_ -= std::min(cache_bytes_, victim->second.block->approxBytes());
        blocks_.erase(victim);
    }
}

bool TableModel::allRowsCachedLocked() const
{
    if (!source_row_count_.isExact())
        return false;
    const size_t B = options_.block_rows;
    for (size_t b = 0; b * B < source_row_count_.value; ++b)
    {
        if (!blocks_.contains(b))
            return false;
    }
    return true;
}

const TableRow *TableModel::sourceRowLocked(size_t source_row, RowBlockPtr *keep_alive) const
{
    auto it = blocks_.find(source_row / options_.block_rows);
    if (it == blocks_.end())
        return nullptr;
    const RowBlockPtr &block = it->second.block;
    const size_t offset = source_row - block->first_row;
    if (offset >= block->rows.size())
        return nullptr;
    if (keep_alive)
        *keep_alive = block;
    return &block->rows[offset];
}

bool TableModel::stepApplyView(std::unique_lock<std::mutex> &lock, std::stop_token stop,
                               std::vector<TableEvent> &events)
{
    const ViewSpec spec = *pending_view_;

    auto commit = [&](std::shared_ptr<const std::vector<size_t>> rows)
    {
        if (pending_view_ && *pending_view_ == spec)
            pending_view_.reset();
        view_ = spec;
        view_rows_ = std::move(rows);
        if (capabilities_.random_access)
            evictLocked();
        restartSearchLocked();
        if (stats_accumulator_)
            restartStatsLocked();
        if (histogram_.active)
            restartHistogramLocked();
        if (scatter_.active)
            restartScatterLocked();
        // Rows already read belong to the old view.
        if (scan_ && (scan_->next_row > 0 || !(scan_->spec == spec)))
            failScanLocked("The view changed while its rows were being read");
        events.push_back({TableEvent::Kind::ViewChanged});
        events.push_back({TableEvent::Kind::RowCountChanged});
    };

    if (capabilities_.query_pushdown)
    {
        lock.unlock();
        source_->applyView(spec);
        const RowCount count = source_->rowCount();
        lock.lock();
        blocks_.clear();
        cache_bytes_ = 0;
        adhoc_block_.reset();
        if (!view_.filtered())
            unfiltered_row_count_ = source_row_count_;
        source_row_count_ = count;
        count_attempted_ = false;
        commit(nullptr);
        return true;
    }

    if (spec.empty())
    {
        commit(nullptr);
        return true;
    }

    if (!allRowsCachedLocked())
    {
        const size_t B = options_.block_rows;
        for (size_t b = 0;; ++b)
        {
            if (!blocks_.contains(b))
                return loadBlock(capabilities_.random_access ? b : sequential_rows_ / B, lock, stop, events);
        }
    }

    // Snapshot the (immutable) blocks and compute the permutation without the lock.
    std::vector<RowBlockPtr> snapshot;
    for (const auto &[index, cached] : blocks_)
        snapshot.push_back(cached.block);
    lock.unlock();

    std::vector<std::pair<size_t, const TableRow *>> rows;
    for (const auto &block : snapshot)
    {
        for (size_t i = 0; i < block->rows.size(); ++i)
        {
            if (matchesFilter(block->rows[i], spec.filter))
                rows.emplace_back(block->first_row + i, &block->rows[i]);
        }
    }
    if (!spec.sort.empty())
    {
        std::stable_sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b)
                         {
            for (const auto &key : spec.sort)
            {
                const int cmp = compareCells(cellAt(*a.second, key.column), cellAt(*b.second, key.column));
                if (cmp != 0)
                {
                    const bool a_null = isNull(cellAt(*a.second, key.column));
                    const bool b_null = isNull(cellAt(*b.second, key.column));
                    // keep empty cells at the bottom in both directions
                    if (a_null || b_null)
                        return b_null;
                    return key.ascending ? cmp < 0 : cmp > 0;
                }
            }
            return false; });
    }
    auto order = std::make_shared<std::vector<size_t>>();
    order->reserve(rows.size());
    for (const auto &[source_row, row] : rows)
        order->push_back(source_row);

    lock.lock();
    if (!pending_view_ || !(*pending_view_ == spec))
        return true; // superseded by a newer setView(); the next step handles it
    commit(std::move(order));
    return true;
}

std::optional<size_t> TableModel::scanBlockToLoadLocked(size_t start) const
{
    if (view_rows_ || (source_row_count_.isExact() && start >= source_row_count_.value))
        return std::nullopt;
    const size_t B = options_.block_rows;
    if (blocks_.contains(start / B))
        return std::nullopt;
    return capabilities_.random_access ? start / B : sequential_rows_ / B;
}

// Collects up to one block worth of view rows from `start`, keeping their
// blocks alive. The caller loads scanBlockToLoadLocked(start) first.
TableModel::ScanChunk TableModel::scanChunkLocked(size_t start) const
{
    const size_t B = options_.block_rows;
    std::optional<size_t> total;
    if (view_rows_)
        total = view_rows_->size();
    else if (source_row_count_.isExact())
        total = source_row_count_.value;

    ScanChunk chunk;
    size_t end = start + B;
    if (total)
        end = std::min(end, std::max(*total, start));
    if (!view_rows_)
        end = std::min(end, (start / B + 1) * B); // stay within the cached block
    for (size_t view_row = start; view_row < end; ++view_row)
    {
        const size_t source_row = view_rows_ ? (*view_rows_)[view_row] : view_row;
        RowBlockPtr block;
        const TableRow *row = sourceRowLocked(source_row, &block);
        if (!row)
        {
            end = view_row;
            break;
        }
        chunk.keep_alive.push_back(std::move(block));
        chunk.rows.emplace_back(view_row, row);
    }
    chunk.end = end;
    // A short block at the end of a sequential source means we reached EOF.
    chunk.last = (total && end >= *total) || (!view_rows_ && end == start);
    return chunk;
}

bool TableModel::stepSearch(std::unique_lock<std::mutex> &lock, std::stop_token stop,
                            std::vector<TableEvent> &events)
{
    const size_t start = search_next_row_;
    if (const auto block_index = scanBlockToLoadLocked(start))
        return loadBlock(*block_index, lock, stop, events);
    const ScanChunk chunk = scanChunkLocked(start);
    const SearchSpec spec = *search_spec_;
    const std::shared_ptr<const re2::RE2> regex = search_regex_;
    const uint64_t generation = search_generation_;
    lock.unlock();

    std::vector<CellPosition> hits;
    for (const auto &[view_row, row] : chunk.rows)
    {
        if (spec.column)
        {
            if (*spec.column < row->size() && matchesSearch((*row)[*spec.column], spec, regex.get()))
                hits.push_back({view_row, *spec.column});
            continue;
        }
        for (size_t c = 0; c < row->size(); ++c)
        {
            if (matchesSearch((*row)[c], spec, regex.get()))
                hits.push_back({view_row, c});
        }
    }

    lock.lock();
    if (generation != search_generation_)
        return true;
    for (const auto &hit : hits)
    {
        if (search_.hits.size() >= options_.max_search_hits)
        {
            search_.truncated = true;
            search_.done = true;
            break;
        }
        search_.hits.push_back(hit);
    }
    search_.scanned_rows += chunk.end - start;
    search_next_row_ = chunk.end;
    if (chunk.last)
        search_.done = true;
    if (capabilities_.random_access)
        evictLocked();
    events.push_back({TableEvent::Kind::SearchProgress, start, chunk.end});
    return true;
}

bool TableModel::stepStats(std::unique_lock<std::mutex> &lock, std::stop_token stop,
                           std::vector<TableEvent> &events)
{
    if (capabilities_.query_pushdown && !stats_pushdown_tried_)
        return stepStatsPushdown(lock, stop, events);
    const size_t start = stats_next_row_;
    if (const auto block_index = scanBlockToLoadLocked(start))
        return loadBlock(*block_index, lock, stop, events);
    const ScanChunk chunk = scanChunkLocked(start);
    const size_t column = stats_.column;
    const std::shared_ptr<ColumnStatsAccumulator> accumulator = stats_accumulator_;
    const uint64_t generation = stats_generation_;
    const auto published = stats_published_;
    lock.unlock();

    for (const auto &[view_row, row] : chunk.rows)
        accumulator->add(cellAt(*row, column));
    // Ranking the most frequent values takes a while with many distinct
    // values, so partial results are published only now and then.
    const auto now = std::chrono::steady_clock::now();
    std::optional<ColumnStats> result;
    if (chunk.last || now - published >= STATS_PUBLISH_INTERVAL)
        result = accumulator->result();

    lock.lock();
    if (generation != stats_generation_)
        return true;
    stats_.scanned_rows += chunk.end - start;
    stats_next_row_ = chunk.end;
    if (result)
    {
        stats_.stats = std::move(*result);
        stats_published_ = now;
    }
    if (chunk.last)
        stats_.done = true;
    if (capabilities_.random_access)
        evictLocked();
    events.push_back({TableEvent::Kind::StatsProgress, start, chunk.end});
    return true;
}

bool TableModel::scanWantsRowsLocked() const
{
    return scan_ && scan_->wantsRows();
}

void TableModel::failScanLocked(const std::string &message)
{
    if (scan_)
        std::exchange(scan_, nullptr)->fail(message);
}

bool TableModel::stepScan(std::unique_lock<std::mutex> &lock, std::stop_token stop,
                          std::vector<TableEvent> &events)
{
    if (!(view_ == scan_->spec))
    {
        // The view to read failed to apply, or another one replaced it.
        failScanLocked("The view changed while its rows were being read");
        return true;
    }
    const size_t start = scan_->next_row;
    if (capabilities_.random_access && !view_rows_)
    {
        // Read past the cache in large blocks: a source that pages with a
        // query per read (DuckDB) is much faster this way.
        const std::shared_ptr<detail::ViewScan> scan = scan_;
        lock.unlock();
        const RowBlockPtr block = source_->read(start, options_.scan_read_rows, stop);
        lock.lock();
        if (scan_ != scan || stop.stop_requested())
            return true; // the reader gave up, or the view changed (see commit())
        ViewRows rows;
        rows.rows.reserve(block->rows.size());
        for (const auto &row : block->rows)
            rows.rows.push_back(&row);
        rows.keep_alive.push_back(block);
        scan_->next_row = start + block->rows.size();
        if (block->rows.size() < options_.scan_read_rows)
            std::exchange(scan_, nullptr)->push(std::move(rows), true);
        else
            scan_->push(std::move(rows), false);
        return true;
    }
    if (const auto block_index = scanBlockToLoadLocked(start))
        return loadBlock(*block_index, lock, stop, events);
    ScanChunk chunk = scanChunkLocked(start);
    ViewRows rows;
    rows.rows.reserve(chunk.rows.size());
    for (size_t i = 0; i < chunk.rows.size(); ++i)
    {
        rows.rows.push_back(chunk.rows[i].second);
        if (rows.keep_alive.empty() || rows.keep_alive.back() != chunk.keep_alive[i])
            rows.keep_alive.push_back(std::move(chunk.keep_alive[i]));
    }
    scan_->next_row = chunk.end;
    if (chunk.last)
        std::exchange(scan_, nullptr)->push(std::move(rows), true);
    else
        scan_->push(std::move(rows), false);
    if (capabilities_.random_access)
        evictLocked();
    return true;
}

// The first pass finds the range of the values, the second counts them into
// bins. Each tries the source's GROUP BY first, then scans the rows.
bool TableModel::stepHistogram(std::unique_lock<std::mutex> &lock, std::stop_token stop,
                               std::vector<TableEvent> &events)
{
    HistogramJob &job = *histogram_job_;
    const size_t column = histogram_.column;
    const bool counting = histogram_.counting;
    const std::shared_ptr<HistogramRange> range = job.range;
    const std::shared_ptr<HistogramCounter> counter = job.counter;
    const uint64_t generation = histogram_generation_;
    auto add = [&](const CellValue &value, size_t count)
    {
        if (counting)
            counter->add(value, count);
        else
            range->add(value, count);
    };

    if (capabilities_.query_pushdown && !job.pushdown_tried)
    {
        job.pushdown_tried = true;
        lock.unlock();
        bool counted = false;
        try
        {
            counted = source_->columnValueCounts(column, add, stop).has_value();
        }
        catch (const std::exception &)
        {
            // Scanning reports the error as reading the sheet does.
        }
        lock.lock();
        if (generation != histogram_generation_)
            return true;
        if (!counted)
        {
            // The source may have added some values before it gave up.
            if (counting)
                histogram_job_->counter = std::make_shared<HistogramCounter>(histogram_.histogram);
            else
                histogram_job_->range = std::make_shared<HistogramRange>(options_.stats_limits);
            return true;
        }
        histogram_.scanned_rows = counting ? counter->result().rows : histogram_.scanned_rows;
        finishHistogramPassLocked(events);
        return true;
    }

    const size_t start = job.next_row;
    if (const auto block_index = scanBlockToLoadLocked(start))
        return loadBlock(*block_index, lock, stop, events);
    const ScanChunk chunk = scanChunkLocked(start);
    const auto published = histogram_published_;
    lock.unlock();

    for (const auto &[view_row, row] : chunk.rows)
        add(cellAt(*row, column), 1);
    const auto now = std::chrono::steady_clock::now();

    lock.lock();
    if (generation != histogram_generation_)
        return true;
    histogram_.scanned_rows += chunk.end - start;
    histogram_job_->next_row = chunk.end;
    if (capabilities_.random_access)
        evictLocked();
    if (chunk.last)
        finishHistogramPassLocked(events);
    else
    {
        // The counts so far, now and then.
        if (counting && now - published >= STATS_PUBLISH_INTERVAL)
        {
            histogram_.histogram = counter->result();
            histogram_published_ = now;
        }
        events.push_back({TableEvent::Kind::HistogramProgress, start, chunk.end});
    }
    return true;
}

void TableModel::finishHistogramPassLocked(std::vector<TableEvent> &events)
{
    HistogramJob &job = *histogram_job_;
    if (histogram_.counting)
    {
        histogram_.histogram = job.counter->result();
        histogram_.done = true;
        histogram_job_.reset();
    }
    else
    {
        histogram_.histogram = job.range->layout(histogram_.target_bins);
        if (histogram_.histogram.binCount() == 0)
        {
            // Neither numbers nor times: the most frequent values are all.
            histogram_.done = true;
            histogram_job_.reset();
        }
        else
        {
            histogram_.counting = true;
            histogram_.scanned_rows = 0;
            job.next_row = 0;
            job.pushdown_tried = false;
            job.range.reset();
            job.counter = std::make_shared<HistogramCounter>(histogram_.histogram);
            histogram_published_ = std::chrono::steady_clock::now();
        }
    }
    events.push_back({TableEvent::Kind::HistogramProgress, 0, histogram_.scanned_rows});
}

void TableModel::startHistogram(size_t column, size_t target_bins)
{
    {
        std::lock_guard lock(mutex_);
        histogram_.column = column;
        histogram_.target_bins = target_bins;
        restartHistogramLocked();
    }
    wakeWorker();
}

void TableModel::restartHistogramLocked()
{
    const size_t column = histogram_.column;
    const size_t target_bins = histogram_.target_bins;
    histogram_ = {};
    histogram_.active = true;
    histogram_.column = column;
    histogram_.target_bins = target_bins;
    ++histogram_generation_;
    // New accumulators: the worker may still be adding to the old ones.
    histogram_job_ = HistogramJob{};
    histogram_job_->range = std::make_shared<HistogramRange>(options_.stats_limits);
}

void TableModel::cancelHistogram()
{
    std::lock_guard lock(mutex_);
    histogram_job_.reset();
    histogram_ = {};
    ++histogram_generation_;
}

HistogramState TableModel::histogramState() const
{
    std::lock_guard lock(mutex_);
    HistogramState state = histogram_;
    state.generation = histogram_generation_;
    return state;
}

// Like the histogram: the first pass finds the ranges of the columns (by the
// source's GROUP BY if it can), the second counts the rows and samples the
// points. The second always scans, as the points need their view rows.
bool TableModel::stepScatter(std::unique_lock<std::mutex> &lock, std::stop_token stop,
                             std::vector<TableEvent> &events)
{
    ScatterJob &job = *scatter_job_;
    const size_t x_column = scatter_.x_column;
    const size_t y_column = scatter_.y_column;
    const bool counting = scatter_.counting;
    const std::shared_ptr<ScatterRange> range = job.range;
    const std::shared_ptr<ScatterCounter> counter = job.counter;
    const uint64_t generation = scatter_generation_;

    if (!counting && capabilities_.query_pushdown && !job.pushdown_tried)
    {
        job.pushdown_tried = true;
        lock.unlock();
        bool counted = false;
        try
        {
            counted = source_->columnValueCounts(x_column, [&](const CellValue &value, size_t count)
                                                 { range->x().add(value, count); }, stop)
                          .has_value() &&
                      source_->columnValueCounts(y_column, [&](const CellValue &value, size_t count)
                                                 { range->y().add(value, count); }, stop)
                          .has_value();
        }
        catch (const std::exception &)
        {
            // Scanning reports the error as reading the sheet does.
        }
        lock.lock();
        if (generation != scatter_generation_)
            return true;
        if (!counted)
        {
            // The source may have added some values before it gave up.
            scatter_job_->range = std::make_shared<ScatterRange>(options_.stats_limits);
            return true;
        }
        finishScatterPassLocked(events);
        return true;
    }

    const size_t start = job.next_row;
    if (const auto block_index = scanBlockToLoadLocked(start))
        return loadBlock(*block_index, lock, stop, events);
    const ScanChunk chunk = scanChunkLocked(start);
    const auto published = scatter_published_;
    lock.unlock();

    for (const auto &[view_row, row] : chunk.rows)
    {
        if (counting)
            counter->add(cellAt(*row, x_column), cellAt(*row, y_column), view_row);
        else
            range->add(cellAt(*row, x_column), cellAt(*row, y_column));
    }
    const auto now = std::chrono::steady_clock::now();
    // Copied outside the lock: the points take a while.
    std::optional<ScatterPlot> partial;
    if (counting && !chunk.last && now - published >= STATS_PUBLISH_INTERVAL)
        partial = counter->result();

    lock.lock();
    if (generation != scatter_generation_)
        return true;
    scatter_.scanned_rows += chunk.end - start;
    scatter_job_->next_row = chunk.end;
    if (capabilities_.random_access)
        evictLocked();
    if (chunk.last)
        finishScatterPassLocked(events);
    else
    {
        // The counts so far, now and then.
        if (partial)
        {
            scatter_.plot = std::move(*partial);
            scatter_published_ = now;
        }
        events.push_back({TableEvent::Kind::ScatterProgress, start, chunk.end});
    }
    return true;
}

void TableModel::finishScatterPassLocked(std::vector<TableEvent> &events)
{
    ScatterJob &job = *scatter_job_;
    if (scatter_.counting)
    {
        scatter_.plot = job.counter->result();
        scatter_.done = true;
        scatter_job_.reset();
    }
    else
    {
        scatter_.plot = job.range->layout(scatter_.options);
        if (!scatter_.plot.plottable())
        {
            // A column of neither numbers nor times: nothing to plot.
            scatter_.done = true;
            scatter_job_.reset();
        }
        else
        {
            scatter_.counting = true;
            scatter_.scanned_rows = 0;
            job.next_row = 0;
            job.range.reset();
            job.counter = std::make_shared<ScatterCounter>(scatter_.plot, scatter_.options.max_points);
            scatter_published_ = std::chrono::steady_clock::now();
        }
    }
    events.push_back({TableEvent::Kind::ScatterProgress, 0, scatter_.scanned_rows});
}

void TableModel::startScatter(size_t x_column, size_t y_column, ScatterOptions options)
{
    {
        std::lock_guard lock(mutex_);
        scatter_.x_column = x_column;
        scatter_.y_column = y_column;
        scatter_.options = options;
        restartScatterLocked();
    }
    wakeWorker();
}

void TableModel::restartScatterLocked()
{
    const size_t x_column = scatter_.x_column;
    const size_t y_column = scatter_.y_column;
    const ScatterOptions options = scatter_.options;
    scatter_ = {};
    scatter_.active = true;
    scatter_.x_column = x_column;
    scatter_.y_column = y_column;
    scatter_.options = options;
    ++scatter_generation_;
    // New accumulators: the worker may still be adding to the old ones.
    scatter_job_ = ScatterJob{};
    scatter_job_->range = std::make_shared<ScatterRange>(options_.stats_limits);
}

void TableModel::cancelScatter()
{
    std::lock_guard lock(mutex_);
    scatter_job_.reset();
    scatter_ = {};
    ++scatter_generation_;
}

ScatterState TableModel::scatterState() const
{
    std::lock_guard lock(mutex_);
    ScatterState state = scatter_;
    state.generation = scatter_generation_;
    return state;
}

bool TableModel::stepCount(std::unique_lock<std::mutex> &lock, std::stop_token stop,
                           std::vector<TableEvent> &events)
{
    if (!capabilities_.random_access || source_row_count_.isExact() || count_attempted_ || view_rows_)
        return false;
    count_attempted_ = true;
    lock.unlock();
    const auto count = source_->countRowsSlow(stop);
    lock.lock();
    if (count && !stop.stop_requested())
    {
        source_row_count_ = {*count, RowCount::Kind::Exact};
        events.push_back({TableEvent::Kind::RowCountChanged});
    }
    return true;
}

// ===========================================================================
// TableBook
// ===========================================================================

std::shared_ptr<TableBook> TableBook::open(const std::filesystem::path &path, const OpenOptions &options,
                                           const BackendRegistry &registry, TableModel::Options model_options)
{
    // Before reading: a change made meanwhile must not go unnoticed.
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    const bool stamped = readFileStamp(path, mtime, size);
    auto book = std::make_shared<TableBook>(registry.open(path, options), model_options);
    // Read only by saveEdits() tasks, posted after this.
    book->path_ = path;
    book->file_mtime_ = mtime;
    book->file_size_ = size;
    book->has_file_stamp_ = stamped;
    return book;
}

TableBook::TableBook(std::unique_ptr<DataSource> data_source, TableModel::Options model_options)
    : data_source_(std::move(data_source)),
      model_options_(model_options),
      scheduler_(std::make_shared<detail::Scheduler>())
{
    if (!data_source_)
        throw std::invalid_argument("TableBook: data source is null");
    if (model_options_.block_rows == 0)
        throw std::invalid_argument("TableBook: block_rows must be greater than zero");
    // Read here: once the worker starts, only it touches the data source.
    sheet_names_ = data_source_->sheetNames();
    uses_header_option_ = data_source_->usesHeaderOption();
    is_text_format_ = data_source_->isTextFormat();
    if (!is_text_format_)
    {
        model_options_.memory_checkpoints.clear();
        model_options_.memory_checkpoint_interval = 0;
    }
    skips_meta_lines_ = data_source_->skipsMetaLines();
    text_encoding_ = data_source_->textEncoding();
    supports_sql_ = data_source_->supportsSql();
    supports_editing_ = data_source_->supportsEditing();
    reads_appended_rows_ = data_source_->readsAppendedRows();
    worker_ = std::jthread([scheduler = scheduler_, data_source = data_source_.get()](std::stop_token stop)
                           { scheduler->run(stop, *data_source); });
}

TableBook::~TableBook()
{
    worker_.request_stop();
    if (worker_.joinable())
        worker_.join();
    // Models may outlive the book (the UI may still hold them); they keep
    // their cached rows but must not touch the data source any more.
    std::lock_guard lock(models_mutex_);
    for (auto &[name, model] : models_)
        model->detachSource();
}

std::vector<std::string> TableBook::sheetNames() const
{
    std::lock_guard lock(models_mutex_);
    return sheet_names_;
}

std::shared_ptr<TableModel> TableBook::sheet(const std::string &sheet_name)
{
    std::lock_guard lock(models_mutex_);
    if (std::find(sheet_names_.begin(), sheet_names_.end(), sheet_name) == sheet_names_.end())
        return nullptr;
    auto &model = models_[sheet_name];
    if (!model)
    {
        model = std::shared_ptr<TableModel>(new TableModel(sheet_name, model_options_, scheduler_));
        if (const auto it = result_sheets_.find(sheet_name); it != result_sheets_.end())
            model->preset_source_ = std::make_unique<ResultSheetSource>(it->second);
        model->setEditing(editing_);
        scheduler_->add(model);
    }
    return model;
}

void TableBook::executeSql(std::string sql, std::function<void(const SqlResult &)> done)
{
    scheduler_->post([this, sql = std::move(sql), done = std::move(done)](DataSource &data_source, std::stop_token stop)
                     {
        SqlResult result;
        try
        {
            const SqlOutcome outcome = data_source.executeSql(sql, stop);
            result = {.ok = true, .sheet = outcome.sheet, .message = outcome.message};
            if (!outcome.sheet.empty())
            {
                // Read here, on the worker, which alone touches the data source.
                auto names = data_source.sheetNames();
                std::lock_guard lock(models_mutex_);
                // Result sheets stay, after the file's.
                for (const auto &name : sheet_names_)
                {
                    if (result_sheets_.contains(name))
                        names.push_back(name);
                }
                sheet_names_ = std::move(names);
                if (auto it = models_.find(outcome.sheet); it != models_.end())
                {
                    it->second->retire();
                    models_.erase(it);
                }
            }
        }
        catch (const std::exception &e)
        {
            result = {.ok = false, .message = e.what()};
        }
        if (done)
            done(result); });
}

void TableBook::loadSqlSchema(std::function<void(std::vector<SqlTable>)> done)
{
    scheduler_->post([done = std::move(done)](DataSource &data_source, std::stop_token)
                     {
        std::vector<SqlTable> schema;
        try
        {
            schema = data_source.sqlSchema();
        }
        catch (const std::exception &)
        {
            // Completion goes without names.
        }
        done(std::move(schema)); });
}

std::string TableBook::addResultSheet(const std::string &name, std::shared_ptr<const ResultSheet> rows)
{
    std::shared_ptr<TableModel> replaced;
    std::string sheet = name;
    {
        std::lock_guard lock(models_mutex_);
        const auto listed = [this](const std::string &candidate)
        { return std::find(sheet_names_.begin(), sheet_names_.end(), candidate) != sheet_names_.end(); };
        for (int n = 2; listed(sheet) && !result_sheets_.contains(sheet); ++n)
            sheet = name + " (" + std::to_string(n) + ")";
        if (!listed(sheet))
            sheet_names_.push_back(sheet);
        result_sheets_[sheet] = std::move(rows);
        if (auto it = models_.find(sheet); it != models_.end())
        {
            replaced = std::move(it->second);
            models_.erase(it);
        }
    }
    // Only the worker may drop a model's source.
    if (replaced)
        scheduler_->post([replaced](DataSource &, std::stop_token)
                         { replaced->retire(); });
    return sheet;
}

void TableBook::readAppended(std::function<void(AppendResult)> done)
{
    scheduler_->post([this, done = std::move(done)](DataSource &, std::stop_token stop)
                     {
        std::vector<std::shared_ptr<TableModel>> models;
        {
            std::lock_guard lock(models_mutex_);
            for (const auto &[name, model] : models_)
            {
                if (!result_sheets_.contains(name))
                    models.push_back(model);
            }
        }
        AppendResult result = AppendResult::None;
        for (const auto &model : models)
        {
            AppendResult sheet = AppendResult::Changed;
            try
            {
                sheet = model->readAppendedRows(stop);
            }
            catch (const std::exception &)
            {
                // E.g. a half-written row in an encoding: reading the file again tells.
            }
            if (sheet == AppendResult::Changed)
                result = AppendResult::Changed;
            else if (sheet == AppendResult::Appended && result == AppendResult::None)
                result = AppendResult::Appended;
        }
        // Saving checks the file against what was read.
        if (result == AppendResult::Appended && has_file_stamp_)
            has_file_stamp_ = readFileStamp(path_, file_mtime_, file_size_);
        if (done)
            done(result); });
}

void TableBook::setEditing(bool editing)
{
    std::lock_guard lock(models_mutex_);
    editing_ = editing && supports_editing_;
    for (auto &[name, model] : models_)
        model->setEditing(editing_);
}

bool TableBook::editing() const
{
    std::lock_guard lock(models_mutex_);
    return editing_;
}

bool TableBook::modified() const
{
    std::lock_guard lock(models_mutex_);
    return std::any_of(models_.begin(), models_.end(), [](const auto &entry)
                       { return entry.second->modified(); });
}

void TableBook::discardEdits()
{
    std::lock_guard lock(models_mutex_);
    for (auto &[name, model] : models_)
        model->discardEdits();
}

namespace
{
    // The file a symbolic link points to: saving replaces that, not the link.
    std::filesystem::path resolveSaveTarget(const std::filesystem::path &target)
    {
        std::error_code error;
        if (!std::filesystem::is_symlink(target, error))
            return target;
        const auto resolved = std::filesystem::canonical(target, error);
        return error ? target : resolved;
    }

    // A file name next to `target` that is not taken yet.
    std::filesystem::path temporarySavePath(const std::filesystem::path &target)
    {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const uint64_t tag = (static_cast<uint64_t>(random()) << 32) ^ random();
            std::filesystem::path name(".");
            name += target.filename();
            name += ".tableskim-save-" + std::to_string(tag);
            const auto path = target.parent_path() / name;
            std::error_code error;
            if (!std::filesystem::exists(path, error) && !error)
                return path;
        }
        throw std::runtime_error("Cannot find a name for a temporary file");
    }
} // namespace

void TableBook::saveEdits(std::filesystem::path target, std::function<void(const SaveResult &)> done)
{
    scheduler_->post([this, target = resolveSaveTarget(target), done = std::move(done)](DataSource &data_source, std::stop_token stop)
                     {
        SaveResult result;
        std::filesystem::path written;
        try
        {
            if (!supports_editing_)
                throw std::runtime_error("This file cannot be edited");
            std::filesystem::file_time_type mtime{};
            std::uintmax_t size = 0;
            if (has_file_stamp_ && (!readFileStamp(path_, mtime, size) || mtime != file_mtime_ || size != file_size_))
                throw std::runtime_error("The file has been changed by another program since it was opened");
            std::vector<SheetEdits> edits;
            {
                std::lock_guard lock(models_mutex_);
                for (const auto &[name, model] : models_)
                {
                    SheetEdits sheet = model->sheetEdits();
                    if (!sheet.rows.empty())
                        edits.push_back(std::move(sheet));
                }
            }
            written = temporarySavePath(target);
            data_source.saveEdits(edits, written, stop);
            if (stop.stop_requested())
                throw std::runtime_error("The file was closed");
            result = {.ok = true, .written = written};
        }
        catch (const std::exception &e)
        {
            if (!written.empty())
            {
                std::error_code ignored;
                std::filesystem::remove(written, ignored);
            }
            result = {.ok = false, .message = e.what()};
        }
        if (done)
            done(result); });
}

void commitSave(const std::filesystem::path &written, const std::filesystem::path &target)
{
    const std::filesystem::path resolved = resolveSaveTarget(target);
    // A new file gets the default permissions; keep the old file's.
    std::error_code error;
    const auto status = std::filesystem::status(resolved, error);
    if (!error && std::filesystem::exists(status))
        std::filesystem::permissions(written, status.permissions(), error);
    std::filesystem::rename(written, resolved);
}

SqlOutcome DataSource::executeSql(const std::string &, std::stop_token)
{
    throw std::runtime_error("SQL is not supported for this file");
}

void DataSource::saveEdits(const std::vector<SheetEdits> &, const std::filesystem::path &, std::stop_token)
{
    throw std::runtime_error("This file cannot be edited");
}

// ===========================================================================
// BackendRegistry
// ===========================================================================

void BackendRegistry::add(BackendFactory factory)
{
    factories_.push_back(std::move(factory));
}

std::unique_ptr<DataSource> BackendRegistry::open(const std::filesystem::path &path, const OpenOptions &options) const
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot open file: " + path.string());
    std::vector<std::byte> head(4096);
    stream.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(stream.gcount()));

    const BackendFactory *best = nullptr;
    int best_score = 0;
    for (const auto &factory : factories_)
    {
        const int score = factory.probe(path, head);
        if (score > best_score)
        {
            best = &factory;
            best_score = score;
        }
    }
    if (!best)
        throw std::runtime_error("Unsupported file type: " + path.string());
    return best->open(path, options);
}
