#include "column_stats.hxx"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    std::optional<double> numberOf(const CellValue &value)
    {
        if (const auto *integer = std::get_if<int64_t>(&value))
            return static_cast<double>(*integer);
        if (const auto *number = std::get_if<double>(&value))
            return *number;
        return std::nullopt;
    }

    // A value in ColumnStats::top being ranked; text is compared without copies.
    struct Candidate
    {
        size_t count = 0;
        const std::string *text = nullptr;
        std::optional<double> number; // text that is a number
        const CellValue *other = nullptr;

        CellValue value() const { return text ? CellValue{*text} : *other; }
    };

    bool rankedBefore(const Candidate &a, const Candidate &b)
    {
        if (a.count != b.count)
            return a.count > b.count;
        if (a.text && b.text)
        {
            if (a.number && b.number)
                return compareCells(a.value(), b.value()) < 0;
            if (a.number || b.number)
                return a.number.has_value();
            return *a.text < *b.text;
        }
        return compareCells(a.value(), b.value()) < 0;
    }
} // namespace

void ColumnStatsAccumulator::add(const CellValue &value, size_t count)
{
    if (count == 0)
        return;
    rows_ += count;
    if (isEmptyCell(value))
    {
        empty_ += count;
        return;
    }
    const auto *text = std::get_if<std::string>(&value);
    const std::optional<double> number = text ? parseNumber(*text) : numberOf(value);
    if (number)
        addNumber(*number, count);
    if (text && !number)
    {
        if (!min_text_ || *text < *min_text_)
            min_text_ = *text;
        if (!max_text_ || *text > *max_text_)
            max_text_ = *text;
    }
    else
    {
        if (!min_value_ || compareCells(value, *min_value_) < 0)
            min_value_ = value;
        if (!max_value_ || compareCells(value, *max_value_) > 0)
            max_value_ = value;
    }
    if (text)
        countText(*text, count);
    else
        countOther(value, count);
}

void ColumnStatsAccumulator::addNumber(double number, size_t count)
{
    numbers_ += count;
    // Infinities and NaN (typed doubles) are counted apart: in the running
    // sums below, inf - inf would turn every later result into NaN.
    if (std::isnan(number))
    {
        nan_ += count;
        return;
    }
    if (std::isinf(number))
    {
        (number > 0 ? positive_infinity_ : negative_infinity_) += count;
        return;
    }
    // Merge `count` equal values (Chan et al.), keeping the square root of
    // the variance so that even constant values above sqrt(DBL_MAX) work.
    const double before = static_cast<double>(finite_);
    const double added = static_cast<double>(count);
    finite_ += count;
    if (before == 0)
        mean_ = number;
    else
    {
        const double old_weight = before / static_cast<double>(finite_);
        const double new_weight = added / static_cast<double>(finite_);
        const double scale = std::sqrt(old_weight * new_weight);
        const double difference = number - mean_;
        // Opposite extremes need scaling first; nearby values retain more
        // precision when subtracted before scaling.
        const double delta = std::isfinite(difference) ? difference * scale : number * scale - mean_ * scale;
        population_stddev_ = std::hypot(population_stddev_ * std::sqrt(old_weight), delta);
        mean_ = std::lerp(mean_, number, new_weight);
    }
    const double term = number * added;
    const double sum = sum_ + term;
    if (!std::isfinite(sum))
        sum_compensation_ = 0; // overflow must not introduce inf - inf
    else if (std::abs(sum_) >= std::abs(term))
        sum_compensation_ += (sum_ - sum) + term;
    else
        sum_compensation_ += (term - sum) + sum_;
    sum_ = sum;
}

void ColumnStatsAccumulator::countText(const std::string &text, size_t count)
{
    if (const auto it = text_counts_.find(text); it != text_counts_.end())
    {
        it->second += count;
        return;
    }
    if (text_counts_.size() + other_counts_.size() >= limits_.max_distinct)
    {
        distinct_exact_ = false;
        return;
    }
    text_counts_.emplace(text, count);
}

void ColumnStatsAccumulator::countOther(const CellValue &value, size_t count)
{
    // The type keeps 1 and 1.0, or 1 and TRUE, apart.
    std::string key = std::to_string(value.index()) + ':' + formatCell(value);
    if (const auto it = other_counts_.find(key); it != other_counts_.end())
    {
        it->second.count += count;
        return;
    }
    if (text_counts_.size() + other_counts_.size() >= limits_.max_distinct)
    {
        distinct_exact_ = false;
        return;
    }
    other_counts_.emplace(std::move(key), Entry{value, count});
}

ColumnStats ColumnStatsAccumulator::result() const
{
    ColumnStats stats;
    stats.rows = rows_;
    stats.empty = empty_;
    stats.distinct = text_counts_.size() + other_counts_.size();
    stats.distinct_exact = distinct_exact_;
    // Numbers (and dates, ...) sort before text.
    if (min_value_)
        stats.min = *min_value_;
    else if (min_text_)
        stats.min = CellValue{*min_text_};
    if (max_text_)
        stats.max = CellValue{*max_text_};
    else if (max_value_)
        stats.max = *max_value_;
    stats.numbers = numbers_;
    if (numbers_ > 0)
    {
        // What plain arithmetic gives with infinities and NaN.
        constexpr double inf = std::numeric_limits<double>::infinity();
        if (nan_ > 0 || (positive_infinity_ > 0 && negative_infinity_ > 0))
            stats.sum = stats.mean = std::numeric_limits<double>::quiet_NaN();
        else if (positive_infinity_ > 0 || negative_infinity_ > 0)
            stats.sum = stats.mean = positive_infinity_ > 0 ? inf : -inf;
        else
        {
            stats.sum = sum_ + sum_compensation_;
            stats.mean = mean_;
        }
    }
    if (numbers_ > 1)
        stats.stddev = finite_ == numbers_ ? population_stddev_ * std::sqrt(static_cast<double>(numbers_) /
                                                                          static_cast<double>(numbers_ - 1))
                                           : std::numeric_limits<double>::quiet_NaN();

    std::vector<Candidate> candidates;
    candidates.reserve(stats.distinct);
    for (const auto &[text, count] : text_counts_)
        candidates.push_back({.count = count, .text = &text, .number = parseNumber(text)});
    for (const auto &[key, entry] : other_counts_)
        candidates.push_back({.count = entry.count, .other = &entry.value});
    const size_t top = std::min(limits_.top_values, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(top), candidates.end(),
                      rankedBefore);
    stats.top.reserve(top);
    for (size_t i = 0; i < top; ++i)
        stats.top.push_back({candidates[i].value(), candidates[i].count});
    return stats;
}
