#ifndef _COLUMN_TYPE_HXX_
#define _COLUMN_TYPE_HXX_

#include <cstddef>
#include <string>

#include "table_types.hxx"

// What a column seems to hold, judged from a sample of its cells, for the
// marks in the column headers. Text cells count as what they read as, the
// way filters read them: numbers, ISO dates and date-times, TRUE / FALSE.
enum class ColumnKind
{
    Unknown, // no value seen yet
    Number,
    Date,
    DateTime, // dates with a time of day (some may have none)
    Boolean,
    Text,
    Mixed, // no kind makes up 90% of the values
};

struct ColumnType
{
    ColumnKind kind = ColumnKind::Unknown;
    bool integers = false; // Number: every value is a whole number
    size_t values = 0;     // non-empty values sampled
    bool operator==(const ColumnType &) const = default;
};

// Counts the kinds of a column's cells; empty cells are skipped.
class ColumnTypeCounter
{
public:
    void add(const CellValue &value);
    void merge(const ColumnTypeCounter &other);
    ColumnType type() const;

private:
    size_t numbers_ = 0;
    size_t integers_ = 0;
    size_t dates_ = 0;
    size_t date_times_ = 0;
    size_t booleans_ = 0;
    size_t texts_ = 0;
};

// The mark drawn before a column's name: "#" numbers, "@" dates and
// date-times, "?" booleans, nothing for text and the rest.
const char *columnKindMark(ColumnKind kind);
// "Number (whole numbers)", "Date", "Text", "Mixed", ...
std::string columnTypeText(const ColumnType &type);

#endif // _COLUMN_TYPE_HXX_
