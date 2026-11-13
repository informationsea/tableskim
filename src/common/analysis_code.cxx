#include "analysis_code.hxx"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>

#include "decompress.hxx"
#include "file_util.hxx"
#include "table.hxx"

// The code favours what each library does by itself over TableSkim's exact
// rules, so that it stays short. Known differences: the libraries read the
// types of a column from its values (a text column stays text, so "> 5" on
// it compares text), and in R and polars a comparison with an empty cell
// drops the row (except "not equal to"), where TableSkim compares "".

namespace
{
    bool isPython(CodeLanguage language) { return language != CodeLanguage::R; }

    // A string literal: double quotes, or single ones for text holding double
    // quotes only (SQL), as both languages allow.
    std::string literal(std::string_view text)
    {
        const bool single = text.find('"') != std::string_view::npos && text.find('\'') == std::string_view::npos;
        const char quote = single ? '\'' : '"';
        std::string out(1, quote);
        for (const char c : text)
        {
            switch (c)
            {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c == quote)
                {
                    out += '\\';
                    out += c;
                }
                else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
                {
                    char escaped[8];
                    std::snprintf(escaped, sizeof escaped, "\\x%02x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += escaped;
                }
                else
                    out += c;
            }
        }
        out += quote;
        return out;
    }

    std::string literalList(const std::vector<std::string> &items, CodeLanguage language)
    {
        std::string out = isPython(language) ? "[" : "c(";
        for (size_t i = 0; i < items.size(); ++i)
            out += (i ? ", " : "") + literal(items[i]);
        out += isPython(language) ? "]" : ")";
        return out;
    }

    // An R name as code: as it is if it is syntactic, else in backticks.
    std::string rName(const std::string &name)
    {
        static const std::set<std::string> reserved = {
            "if",  "else", "repeat", "while", "function", "for", "next", "break", "TRUE", "FALSE", "NULL",
            "Inf", "NaN",  "NA",     "NA_integer_", "NA_real_", "NA_character_", "in",
        };
        bool syntactic = !name.empty() && !reserved.contains(name);
        for (size_t i = 0; syntactic && i < name.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            const bool digit = c >= '0' && c <= '9';
            if (i == 0)
                syntactic = letter || (c == '.' && !(name.size() > 1 && name[1] >= '0' && name[1] <= '9'));
            else
                syntactic = letter || digit || c == '.' || c == '_';
        }
        if (syntactic)
            return name;
        std::string out = "`";
        for (const char c : name)
        {
            if (c == '`' || c == '\\')
                out += '\\';
            out += c;
        }
        return out + "`";
    }

    // `name(arg, arg)`, or one argument per line if that is long.
    std::string call(const std::string &name, const std::vector<std::string> &args, CodeLanguage language,
                     const std::string &indent = "")
    {
        std::string line = name + "(";
        for (size_t i = 0; i < args.size(); ++i)
            line += (i ? ", " : "") + args[i];
        line += ")";
        if (indent.size() + line.size() <= 88 || args.size() < 2)
            return line;
        const std::string inner = indent + (isPython(language) ? "    " : "  ");
        std::string out = name + "(\n";
        for (size_t i = 0; i < args.size(); ++i)
            out += inner + args[i] + (i + 1 < args.size() || isPython(language) ? ",\n" : "\n");
        return out + indent + ")";
    }

    std::string sqlIdentifier(std::string_view name)
    {
        std::string out = "\"";
        for (const char c : name)
        {
            if (c == '"')
                out += '"';
            out += c;
        }
        return out + "\"";
    }

    // SELECT * FROM the table; DuckDB names tables outside the main schema
    // "schema.table".
    std::string selectAll(const CodeSource &source)
    {
        std::string from;
        const size_t dot = source.table.find('.');
        if (source.format == CodeFormat::DuckDb && dot != std::string::npos)
            from = sqlIdentifier(std::string_view(source.table).substr(0, dot)) + "." +
                   sqlIdentifier(std::string_view(source.table).substr(dot + 1));
        else
            from = sqlIdentifier(source.table);
        return "SELECT * FROM " + from;
    }

    std::string asciiLower(std::string text)
    {
        for (char &c : text)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return text;
    }

    std::string compressionExtension(const std::string &path)
    {
        const std::filesystem::path file = utf8Path(path);
        return stripCompressionExtension(file) == file ? std::string() : lowerExtension(file);
    }

    bool isTemporal(ColumnKind kind) { return kind == ColumnKind::Date || kind == ColumnKind::DateTime; }
    // Kinds a library reads as numbers (Unknown: nothing tells otherwise).
    bool isNumeric(ColumnKind kind) { return kind == ColumnKind::Number || kind == ColumnKind::Unknown; }
    bool isText(ColumnKind kind) { return kind == ColumnKind::Text || kind == ColumnKind::Mixed; }

    // ------------------------------------------------------------------
    // The value a filter compares with, as the column's type reads it.
    // ------------------------------------------------------------------

    struct CivilTime
    {
        int64_t year;
        unsigned month, day, hour, minute, second, micros;
    };

    CivilTime civilTime(int64_t micros_since_epoch)
    {
        constexpr int64_t MICROS_PER_DAY = int64_t{86400} * 1000000;
        int64_t days = micros_since_epoch / MICROS_PER_DAY;
        int64_t rest = micros_since_epoch % MICROS_PER_DAY;
        if (rest < 0)
        {
            rest += MICROS_PER_DAY;
            --days;
        }
        // Howard Hinnant's civil_from_days().
        days += 719468;
        const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
        const auto doe = static_cast<unsigned>(days - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned day = doy - (153 * mp + 2) / 5 + 1;
        const unsigned month = mp < 10 ? mp + 3 : mp - 9;
        const int64_t year = static_cast<int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
        const auto seconds = static_cast<unsigned>(rest / 1000000);
        return {year, month, day, seconds / 3600, seconds / 60 % 60, seconds % 60, static_cast<unsigned>(rest % 1000000)};
    }

    struct FilterValue
    {
        enum class Kind
        {
            Number,
            Date,
            DateTime,
            Boolean,
            Text,
        };
        Kind kind = Kind::Text;
        std::string number; // Number: as code
        CivilTime time{};   // Date, DateTime
        bool boolean = false;
    };

    FilterValue filterValue(ColumnKind kind, const std::string &value)
    {
        FilterValue out;
        if (isNumeric(kind))
        {
            if (const auto number = parseNumber(value); number && std::isfinite(*number))
            {
                out.kind = FilterValue::Kind::Number;
                // Whole numbers without an exponent (1000000, not 1e+06).
                if (*number == std::floor(*number) && std::fabs(*number) < 1e15)
                    out.number = std::to_string(static_cast<int64_t>(*number));
                else
                    out.number = shortestDoubleText(*number);
                return out;
            }
        }
        if (isTemporal(kind))
        {
            if (const auto temporal = parseTemporal(value))
            {
                out.kind = temporal->has_time ? FilterValue::Kind::DateTime : FilterValue::Kind::Date;
                out.time = civilTime(temporal->micros);
                return out;
            }
        }
        if (kind == ColumnKind::Boolean)
        {
            if (const auto boolean = parseBoolean(value))
            {
                out.kind = FilterValue::Kind::Boolean;
                out.boolean = *boolean;
                return out;
            }
        }
        return out;
    }

    std::string isoDate(const CivilTime &t)
    {
        char text[32];
        std::snprintf(text, sizeof text, "%04lld-%02u-%02u", static_cast<long long>(t.year), t.month, t.day);
        return text;
    }

    std::string isoDateTime(const CivilTime &t)
    {
        char text[48];
        std::snprintf(text, sizeof text, "%s %02u:%02u:%02u", isoDate(t).c_str(), t.hour, t.minute, t.second);
        std::string out = text;
        if (t.micros)
        {
            std::snprintf(text, sizeof text, ".%06u", t.micros);
            out += text;
        }
        return out;
    }

    const char *comparison(FilterOp op)
    {
        switch (op)
        {
        case FilterOp::Equals:
            return "==";
        case FilterOp::NotEquals:
            return "!=";
        case FilterOp::Less:
            return "<";
        case FilterOp::LessEqual:
            return "<=";
        case FilterOp::Greater:
            return ">";
        case FilterOp::GreaterEqual:
            return ">=";
        default:
            return "==";
        }
    }

    // ------------------------------------------------------------------
    // The code of each language
    // ------------------------------------------------------------------

    class Writer
    {
    public:
        Writer(const AnalysisCodeSpec &spec, CodeLanguage language) : spec_(spec), source_(spec.source), language_(language) {}
        virtual ~Writer() = default;

        std::string write()
        {
            std::string body;
            body += load();
            body += prepareColumns();
            if (spec_.view.filtered())
                body += "\n" + filter(*spec_.view.filter);
            if (std::holds_alternative<std::monostate>(spec_.analysis))
            {
                if (!spec_.view.sort.empty())
                    body += "\n" + sort(spec_.view.sort);
                std::vector<size_t> hidden;
                for (const size_t column : spec_.hidden)
                {
                    if (column < source_.columns.size())
                        hidden.push_back(column);
                }
                if (!hidden.empty())
                    body += "\n" + drop(hidden);
                body += "\n" + show();
            }
            else if (const auto *group_by = std::get_if<GroupBySpec>(&spec_.analysis))
                body += "\n" + groupBy(*group_by);
            else if (const auto *histogram = std::get_if<HistogramCodeSpec>(&spec_.analysis))
                body += "\n" + this->histogram(*histogram);
            else if (const auto *scatter = std::get_if<ScatterCodeSpec>(&spec_.analysis))
                body += "\n" + this->scatter(*scatter);
            return header() + "\n" + imports() + "\n" + body;
        }

    protected:
        const AnalysisCodeSpec &spec_;
        const CodeSource &source_;
        CodeLanguage language_;
        std::set<std::string> imports_;  // whole lines, sorted
        std::set<std::string> plotnine_; // Polars: names imported from plotnine

        std::string name(size_t column) const { return source_.columnName(column); }
        ColumnKind kind(size_t column) const { return source_.kind(column); }

        // Generated columns must not hide a grouping key or an input used
        // later in an R summarise(), which evaluates its arguments in order.
        std::string unusedName(const std::string &base) const
        {
            std::string candidate = base;
            for (size_t suffix = 2; std::find(source_.columns.begin(), source_.columns.end(), candidate) !=
                                    source_.columns.end(); ++suffix)
                candidate = base + "_" + std::to_string(suffix);
            return candidate;
        }

        std::string header() const
        {
            std::string what = "The view";
            if (std::holds_alternative<GroupBySpec>(spec_.analysis))
                what = "Group By";
            else if (std::holds_alternative<HistogramCodeSpec>(spec_.analysis))
                what = "Histogram";
            else if (std::holds_alternative<ScatterCodeSpec>(spec_.analysis))
                what = "Scatter plot";
            std::string file = utf8String(utf8Path(source_.path).filename());
            if (!source_.table.empty())
                file += " (" + source_.table + ")";
            // Line breaks in a name would end the comment.
            std::replace(file.begin(), file.end(), '\n', ' ');
            std::replace(file.begin(), file.end(), '\r', ' ');
            return "# " + what + " of " + file + ", as shown by TableSkim\n# " + codeLanguageName(language_) + "\n";
        }

        // Columns that filters or the analysis use.
        std::vector<size_t> usedColumns() const
        {
            std::set<size_t> used;
            if (spec_.view.filter)
            {
                for (const auto *group : {&spec_.view.filter->conditions, &spec_.view.filter->narrowing})
                {
                    for (const FilterCondition &condition : *group)
                        used.insert(condition.column);
                }
            }
            if (const auto *group_by = std::get_if<GroupBySpec>(&spec_.analysis))
            {
                used.insert(group_by->keys.begin(), group_by->keys.end());
                used.insert(group_by->values.begin(), group_by->values.end());
            }
            else if (const auto *histogram = std::get_if<HistogramCodeSpec>(&spec_.analysis))
            {
                used.insert(histogram->column);
                if (histogram->group_column)
                    used.insert(*histogram->group_column);
            }
            else if (const auto *scatter = std::get_if<ScatterCodeSpec>(&spec_.analysis))
            {
                used.insert({scatter->x_column, scatter->y_column});
                if (scatter->color_column)
                    used.insert(*scatter->color_column);
            }
            return {used.begin(), used.end()};
        }

        std::string filter(const Filter &filter)
        {
            std::vector<std::string> terms;
            const bool any = filter.combine == Filter::Combine::Or && filter.conditions.size() > 1;
            if (any)
            {
                std::vector<std::string> alternatives;
                for (const FilterCondition &c : filter.conditions)
                    alternatives.push_back(condition(c));
                terms.push_back(anyOf(alternatives));
            }
            else
            {
                for (const FilterCondition &c : filter.conditions)
                    terms.push_back(condition(c));
            }
            for (const FilterCondition &c : filter.narrowing)
                terms.push_back(condition(c));
            return allOf(terms);
        }

        // The rows where `condition` holds.
        std::string condition(const FilterCondition &condition)
        {
            const ColumnKind column_kind = kind(condition.column);
            switch (condition.op)
            {
            case FilterOp::Contains:
            case FilterOp::NotContains:
                return contains(condition.column, condition.value, condition.op == FilterOp::NotContains);
            case FilterOp::Matches:
            case FilterOp::NotMatches:
                return matches(condition.column, condition.value, condition.op == FilterOp::NotMatches);
            case FilterOp::IsEmpty:
            case FilterOp::IsNotEmpty:
                return empty(condition.column, condition.op == FilterOp::IsNotEmpty);
            case FilterOp::IsNumber:
                return isNumber(condition.column);
            case FilterOp::IsDate:
                return isDate(condition.column);
            default:
                break;
            }
            return compare(condition.column, condition.op, filterValue(column_kind, condition.value), condition.value);
        }

        virtual std::string load() = 0;
        virtual std::string prepareColumns() { return {}; }
        virtual std::string imports() = 0;
        virtual std::string anyOf(const std::vector<std::string> &terms) = 0;
        virtual std::string allOf(const std::vector<std::string> &terms) = 0;
        virtual std::string compare(size_t column, FilterOp op, const FilterValue &value, const std::string &text) = 0;
        virtual std::string contains(size_t column, const std::string &text, bool negated) = 0;
        virtual std::string matches(size_t column, const std::string &pattern, bool negated) = 0;
        virtual std::string empty(size_t column, bool negated) = 0;
        virtual std::string isNumber(size_t column) = 0;
        virtual std::string isDate(size_t column) = 0;
        virtual std::string sort(const std::vector<SortKey> &keys) = 0;
        virtual std::string drop(const std::vector<size_t> &columns) = 0;
        virtual std::string show() = 0;
        virtual std::string groupBy(const GroupBySpec &spec) = 0;
        virtual std::string histogram(const HistogramCodeSpec &spec) = 0;
        virtual std::string scatter(const ScatterCodeSpec &spec) = 0;

        // Value columns of a Group By whose numeric aggregates are left out
        // (sums of text), as a comment; empty if none.
        std::string numericAggregatesNote(const GroupBySpec &spec) const
        {
            std::vector<std::string> names;
            for (const size_t column : spec.values)
            {
                if (!numericAggregates(column) &&
                    std::any_of(spec.aggregates.begin(), spec.aggregates.end(), isNumericAggregate))
                    names.push_back(name(column));
            }
            if (names.empty())
                return {};
            std::string note = "# Sums, means and standard deviations are left out for";
            for (size_t i = 0; i < names.size(); ++i)
                note += (i ? ", " : " ") + names[i];
            // Line breaks in a name would end the comment: the rest of the
            // name would be run as code (Python ends a comment at a CR too).
            std::replace(note.begin(), note.end(), '\n', ' ');
            std::replace(note.begin(), note.end(), '\r', ' ');
            return note + " (not numbers)\n";
        }

        static bool isNumericAggregate(Aggregate aggregate)
        {
            return aggregate == Aggregate::Sum || aggregate == Aggregate::Mean || aggregate == Aggregate::StdDev;
        }
        bool numericAggregates(size_t column) const
        {
            const ColumnKind column_kind = kind(column);
            return isNumeric(column_kind) || column_kind == ColumnKind::Boolean;
        }
        // The aggregates of a value column, as TableSkim names them.
        std::vector<std::pair<Aggregate, std::string>> aggregatesOf(const GroupBySpec &spec, size_t column) const
        {
            std::vector<std::pair<Aggregate, std::string>> out;
            for (const Aggregate aggregate : spec.aggregates)
            {
                if (isNumericAggregate(aggregate) && !numericAggregates(column))
                    continue;
                out.emplace_back(aggregate, unusedName(name(column) + " " + aggregateName(aggregate)));
            }
            return out;
        }

        bool categoricalX(const ScatterCodeSpec &spec) const
        {
            const ColumnKind x = kind(spec.x_column);
            return isText(x) || x == ColumnKind::Boolean;
        }
        bool barChart(const HistogramCodeSpec &spec) const
        {
            const ColumnKind column_kind = kind(spec.column);
            return isText(column_kind) || column_kind == ColumnKind::Boolean;
        }
    };

    // ---------------------------------------------------------------- R

    class RWriter : public Writer
    {
    public:
        using Writer::Writer;

    private:
        std::string col(size_t column) const { return rName(name(column)); }
        std::string text(size_t column) const
        {
            return isText(kind(column)) ? col(column) : "as.character(" + col(column) + ")";
        }

        std::string imports() override
        {
            std::string out = "library(tidyverse)\n";
            for (const std::string &line : imports_)
                out += line + "\n";
            return out;
        }

        std::string load() override
        {
            std::string out = "path <- " + literal(source_.path) + "\n";
            const std::vector<std::string> names = source_.columns;
            switch (source_.format)
            {
            case CodeFormat::Text:
            {
                std::string function = "read_delim";
                std::vector<std::string> args = {"path"};
                if (source_.delimiter == ',')
                    function = "read_csv";
                else if (source_.delimiter == '\t')
                    function = "read_tsv";
                else if (source_.delimiter)
                    args.push_back("delim = " + literal(std::string(1, *source_.delimiter)));
                if (!source_.header)
                    args.push_back("col_names = " + literalList(names, language_));
                if (source_.skip_meta_lines)
                    args.push_back("comment = \"##\"");
                if (source_.encoding != TextEncoding::Utf8)
                    args.push_back("locale = locale(encoding = " + literal(encodingName()) + ")");
                out += "df <- " + call(function, args, language_) + "\n";
                break;
            }
            case CodeFormat::Excel:
            {
                imports_.insert("library(readxl)");
                std::vector<std::string> args = {"path"};
                if (!source_.table.empty())
                    args.push_back("sheet = " + literal(source_.table));
                if (!source_.header)
                    args.push_back("col_names = " + literalList(names, language_));
                out += "df <- " + call("read_excel", args, language_) + "\n";
                break;
            }
            case CodeFormat::Ods:
            {
                imports_.insert("library(readODS)");
                std::vector<std::string> args = {"path"};
                if (!source_.table.empty())
                    args.push_back("sheet = " + literal(source_.table));
                // read_ods() takes no names: only whether the first row has them.
                if (!source_.header)
                    args.push_back("col_names = FALSE");
                out += "df <- " + call("read_ods", args, language_) + "\n";
                if (!source_.header)
                    out += "names(df) <- " + literalList(names, language_) + "\n";
                break;
            }
            case CodeFormat::Parquet:
                imports_.insert("library(arrow)");
                out += "df <- read_parquet(path)\n";
                break;
            case CodeFormat::Json:
                imports_.insert("library(jsonlite)");
                out += "df <- as_tibble(fromJSON(path))\n";
                break;
            case CodeFormat::NdJson:
                imports_.insert("library(jsonlite)");
                out += "df <- as_tibble(stream_in(file(path), verbose = FALSE))\n";
                break;
            case CodeFormat::Sqlite:
            case CodeFormat::DuckDb:
                imports_.insert("library(DBI)");
                if (source_.format == CodeFormat::Sqlite)
                    out += "con <- dbConnect(RSQLite::SQLite(), path, flags = RSQLite::SQLITE_RO)\n";
                else
                    out += "con <- dbConnect(duckdb::duckdb(), path, read_only = TRUE)\n";
                out += "df <- as_tibble(dbGetQuery(con, " + literal(selectAll(source_)) + "))\n";
                out += source_.format == CodeFormat::Sqlite ? "dbDisconnect(con)\n" : "dbDisconnect(con, shutdown = TRUE)\n";
                break;
            }
            return out;
        }

        std::string encodingName() const
        {
            switch (source_.encoding)
            {
            case TextEncoding::Utf16LE:
                return "UTF-16LE";
            case TextEncoding::Utf16BE:
                return "UTF-16BE";
            case TextEncoding::Cp932:
                return "CP932";
            default:
                return "UTF-8";
            }
        }

        std::string anyOf(const std::vector<std::string> &terms) override
        {
            std::string out;
            for (size_t i = 0; i < terms.size(); ++i)
                out += (i ? " |\n    " : "") + terms[i];
            return out;
        }

        std::string allOf(const std::vector<std::string> &terms) override
        {
            return "df <- df |>\n  " + call("filter", terms, language_, "  ") + "\n";
        }

        std::string compare(size_t column, FilterOp op, const FilterValue &value, const std::string &text) override
        {
            std::string lhs = col(column);
            std::string rhs;
            switch (value.kind)
            {
            case FilterValue::Kind::Number:
                rhs = value.number;
                break;
            case FilterValue::Kind::Date:
                if (kind(column) == ColumnKind::DateTime)
                    lhs = "as.Date(" + lhs + ")";
                rhs = "as.Date(\"" + isoDate(value.time) + "\")";
                break;
            case FilterValue::Kind::DateTime:
                rhs = "as.POSIXct(\"" + isoDateTime(value.time) + "\", tz = \"UTC\")";
                break;
            case FilterValue::Kind::Boolean:
                rhs = value.boolean ? "TRUE" : "FALSE";
                break;
            case FilterValue::Kind::Text:
                lhs = this->text(column);
                rhs = literal(text);
                break;
            }
            const std::string expression = lhs + " " + comparison(op) + " " + rhs;
            // Like TableSkim, an empty cell is not equal to a value.
            if (op == FilterOp::NotEquals)
                return "(is.na(" + col(column) + ") | " + expression + ")";
            return expression;
        }

        std::string detect(size_t column, const std::string &pattern, bool negated)
        {
            if (negated)
                return "!str_detect(replace_na(" + text(column) + ", \"\"), " + pattern + ")";
            return "str_detect(" + text(column) + ", " + pattern + ")";
        }

        std::string contains(size_t column, const std::string &value, bool negated) override
        {
            return detect(column, "fixed(" + literal(value) + ", ignore_case = TRUE)", negated);
        }

        std::string matches(size_t column, const std::string &pattern, bool negated) override
        {
            return detect(column, "regex(" + literal(pattern) + ", ignore_case = TRUE)", negated);
        }

        std::string empty(size_t column, bool negated) override
        {
            if (!isText(kind(column)))
                return (negated ? "!is.na(" : "is.na(") + col(column) + ")";
            if (negated)
                return "(!is.na(" + col(column) + ") & " + col(column) + " != \"\")";
            return "(is.na(" + col(column) + ") | " + col(column) + " == \"\")";
        }

        std::string isNumber(size_t column) override
        {
            if (kind(column) == ColumnKind::Number)
                return "is.finite(" + col(column) + ")";
            return "!is.na(suppressWarnings(as.numeric(" + col(column) + ")))";
        }

        std::string isDate(size_t column) override
        {
            if (isTemporal(kind(column)))
                return "!is.na(" + col(column) + ")";
            return "!is.na(ymd_hms(" + col(column) + ", truncated = 3, quiet = TRUE))";
        }

        std::string sort(const std::vector<SortKey> &keys) override
        {
            std::vector<std::string> args;
            for (const SortKey &key : keys)
                args.push_back(key.ascending ? col(key.column) : "desc(" + col(key.column) + ")");
            return "df <- df |>\n  " + call("arrange", args, language_, "  ") + "\n";
        }

        std::string drop(const std::vector<size_t> &columns) override
        {
            std::vector<std::string> args;
            for (const size_t column : columns)
                args.push_back(col(column));
            return "df <- df |>\n  select(-" + (args.size() == 1 ? args[0] : call("c", args, language_, "  ")) + ")\n";
        }

        std::string show() override { return "print(df)\n"; }

        std::string groupBy(const GroupBySpec &spec) override
        {
            std::string out = numericAggregatesNote(spec);
            out += "result <- df |>\n";
            if (!spec.keys.empty())
            {
                std::vector<std::string> keys;
                for (const size_t column : spec.keys)
                    keys.push_back(col(column));
                out += "  " + call("group_by", keys, language_, "  ") + " |>\n";
            }
            std::vector<std::string> args = {rName(unusedName("count")) + " = n()"};
            for (const size_t column : spec.values)
            {
                for (const auto &[aggregate, label] : aggregatesOf(spec, column))
                    args.push_back(rName(label) + " = " + aggregateCall(aggregate, col(column)));
            }
            if (!spec.keys.empty())
                args.push_back(".groups = \"drop\"");
            out += "  summarise(\n";
            for (size_t i = 0; i < args.size(); ++i)
                out += "    " + args[i] + (i + 1 < args.size() ? ",\n" : "\n");
            out += "  )\n";
            return out + "print(result)\n";
        }

        static std::string aggregateCall(Aggregate aggregate, const std::string &column)
        {
            switch (aggregate)
            {
            case Aggregate::Count:
                return "sum(!is.na(" + column + "))";
            case Aggregate::Distinct:
                return "n_distinct(" + column + ", na.rm = TRUE)";
            case Aggregate::Sum:
                return "sum(" + column + ", na.rm = TRUE)";
            case Aggregate::Mean:
                return "mean(" + column + ", na.rm = TRUE)";
            case Aggregate::Min:
                return "min(" + column + ", na.rm = TRUE)";
            case Aggregate::Max:
                return "max(" + column + ", na.rm = TRUE)";
            case Aggregate::StdDev:
                return "sd(" + column + ", na.rm = TRUE)";
            }
            return column;
        }

        std::string plot(const std::string &data, const std::vector<std::string> &aes,
                         const std::vector<std::string> &layers)
        {
            std::string out = "p <- ggplot(" + data + ", " + call("aes", aes, language_) + ")";
            for (const std::string &layer : layers)
                out += " +\n  " + layer;
            return out + "\nprint(p)\n# ggsave(\"plot.png\", p)\n";
        }

        std::string histogram(const HistogramCodeSpec &spec) override
        {
            if (barChart(spec))
            {
                if (spec.group_column)
                {
                    const std::string count = unusedName("count");
                    std::string keys = col(spec.column);
                    if (*spec.group_column != spec.column)
                        keys += ", " + col(*spec.group_column);
                    const std::string out =
                        "top <- df |>\n  filter(!is.na(" + col(spec.column) + ")) |>\n  count(" +
                        col(spec.column) + ", sort = TRUE) |>\n  slice_head(n = " + std::to_string(spec.bins) +
                        ") |>\n  select(" + col(spec.column) + ")\n" +
                        "counts <- df |>\n  semi_join(top, by = " + literal(name(spec.column)) + ") |>\n  count(" +
                        keys + ", name = " + literal(count) + ")\n";
                    return out + plot("counts",
                                      {"x = fct_reorder(as.character(" + col(spec.column) + "), " + rName(count) +
                                           ", .fun = sum, .desc = TRUE)",
                                       "y = " + rName(count), "fill = factor(" + col(*spec.group_column) + ")"},
                                      {"geom_col()", "labs(x = " + literal(name(spec.column)) +
                                                         ", y = \"rows\", fill = " + literal(name(*spec.group_column)) + ")"});
                }
                // The most frequent values, as TableSkim lists them.
                std::string out = "counts <- df |>\n  filter(!is.na(" + col(spec.column) + ")) |>\n  count(" +
                                  col(spec.column) + ", sort = TRUE) |>\n  slice_head(n = " +
                                  std::to_string(spec.bins) + ")\n";
                return out + plot("counts", {"x = fct_inorder(as.character(" + col(spec.column) + "))", "y = n"},
                                  {"geom_col()", "labs(x = " + literal(name(spec.column)) + ", y = \"rows\")"});
            }
            std::vector<std::string> aes = {"x = " + col(spec.column)};
            std::string labels = "labs(y = \"rows\")";
            if (spec.group_column)
            {
                aes.push_back("fill = factor(" + col(*spec.group_column) + ")");
                labels = "labs(y = \"rows\", fill = " + literal(name(*spec.group_column)) + ")";
            }
            return plot("df", aes, {"geom_histogram(bins = " + std::to_string(spec.bins) + ")", labels});
        }

        std::string scatter(const ScatterCodeSpec &spec) override
        {
            std::vector<std::string> aes = {"x = " + col(spec.x_column), "y = " + col(spec.y_column)};
            if (categoricalX(spec))
                return plot("df", aes, {"geom_boxplot()"});
            std::vector<std::string> layers = {"geom_point(alpha = 0.5)"};
            if (spec.color_column)
            {
                aes.push_back("colour = factor(" + col(*spec.color_column) + ")");
                layers.push_back("labs(colour = " + literal(name(*spec.color_column)) + ")");
            }
            return plot("df", aes, layers);
        }
    };

    // ---------------------------------------------------------------- Python

    // What the two Python writers share: the reading of the file.
    class PythonWriter : public Writer
    {
    public:
        using Writer::Writer;

    protected:
        std::string module() const { return language_ == CodeLanguage::Polars ? "pl" : "pd"; }

        std::string imports() override
        {
            std::vector<std::string> standard, third_party;
            for (const std::string &line : imports_)
            {
                const bool stdlib = line == "import bz2" || line == "import gzip" || line == "import io" ||
                                    line == "import lzma" || line == "import sqlite3";
                (stdlib ? standard : third_party).push_back(line);
            }
            if (language_ == CodeLanguage::Polars)
                third_party.push_back("import polars as pl");
            else
                third_party.push_back("import pandas as pd");
            if (!plotnine_.empty())
            {
                std::string line = "from plotnine import ";
                bool first = true;
                for (const std::string &name : plotnine_)
                {
                    line += (first ? "" : ", ") + name;
                    first = false;
                }
                third_party.push_back(line);
            }
            std::sort(third_party.begin(), third_party.end(), [](const std::string &a, const std::string &b)
                      {
                // "import x" before "from x import"
                const bool from_a = a.starts_with("from ");
                const bool from_b = b.starts_with("from ");
                return from_a != from_b ? !from_a : a < b; });
            std::string out;
            for (const std::string &line : standard)
                out += line + "\n";
            if (!standard.empty())
                out += "\n";
            for (const std::string &line : third_party)
                out += line + "\n";
            return out;
        }

        std::string pythonEncoding() const
        {
            switch (source_.encoding)
            {
            case TextEncoding::Utf16LE:
                return "utf-16-le";
            case TextEncoding::Utf16BE:
                return "utf-16-be";
            case TextEncoding::Cp932:
                return "cp932";
            default:
                return "utf-8";
            }
        }

        // The function that opens the compressed file, if it is.
        std::string opener()
        {
            const std::string extension = compressionExtension(source_.path);
            if (extension == ".gz" || extension == ".gzip" || extension == ".bgz")
            {
                imports_.insert("import gzip");
                return "gzip.open";
            }
            if (extension == ".bz2")
            {
                imports_.insert("import bz2");
                return "bz2.open";
            }
            if (extension == ".xz")
            {
                imports_.insert("import lzma");
                return "lzma.open";
            }
            if (extension == ".zst")
            {
                imports_.insert("import zstandard");
                return "zstandard.open";
            }
            return {};
        }

        // Lines starting with "##" are left out before the parser sees them:
        // `text` then holds the rest.
        std::string metaLineFilter()
        {
            imports_.insert("import io");
            std::string open = opener();
            std::string args = "path, \"rt\"";
            if (open.empty())
            {
                open = "open";
                args = "path";
            }
            // Always: text mode would use the locale's encoding otherwise,
            // which is not UTF-8 on Windows.
            args += ", encoding=" + literal(pythonEncoding());
            return "with " + open + "(" + args + ") as file:\n" +
                   "    text = \"\".join(line for line in file if not line.startswith(\"##\"))\n";
        }

        std::string databaseLoad(const std::string &read)
        {
            std::string out;
            if (source_.format == CodeFormat::Sqlite)
            {
                imports_.insert("import sqlite3");
                out += "con = sqlite3.connect(path)\n";
            }
            else
            {
                imports_.insert("import duckdb");
                out += "con = duckdb.connect(path, read_only=True)\n";
            }
            return out + "df = " + read + "\ncon.close()\n";
        }

        std::string col(size_t column) const { return literal(name(column)); }

        std::string namesList() const { return literalList(source_.columns, language_); }

        std::string anyOf(const std::vector<std::string> &terms) override
        {
            std::string out;
            for (size_t i = 0; i < terms.size(); ++i)
                out += (i ? "\n    | (" : "(") + terms[i] + ")";
            return out;
        }

        std::string show() override { return "print(df)\n"; }
    };

    class PolarsWriter : public PythonWriter
    {
    public:
        using PythonWriter::PythonWriter;

    private:
        std::string expr(size_t column) const { return "pl.col(" + col(column) + ")"; }
        std::string text(size_t column) const
        {
            return isText(kind(column)) ? expr(column) : expr(column) + ".cast(pl.String)";
        }

        std::string load() override
        {
            std::string out = "path = " + literal(source_.path) + "\n";
            bool rename = false;
            switch (source_.format)
            {
            case CodeFormat::Text:
            {
                std::vector<std::string> args;
                const std::string extension = compressionExtension(source_.path);
                // polars reads gzip and zstd files by itself, but decodes
                // other encodings than UTF-8 before decompressing.
                const bool decompress = extension == ".bz2" || extension == ".xz" ||
                                        (!extension.empty() && source_.encoding != TextEncoding::Utf8);
                if (source_.skip_meta_lines)
                {
                    out += "# Lines starting with ## are skipped.\n";
                    args.push_back("comment_prefix=\"##\"");
                }
                if (decompress)
                {
                    const std::string open = opener();
                    out += "with " + open + "(path) as file:\n    data = file.read()\n";
                }
                args.insert(args.begin(), decompress ? "data" : "path");
                if (!source_.delimiter)
                    out += "# Set the separator TableSkim guessed if it is not a comma.\n";
                else if (*source_.delimiter != ',')
                    args.push_back("separator=" + literal(std::string(1, *source_.delimiter)));
                if (!source_.header)
                {
                    args.push_back("has_header=False");
                    args.push_back("new_columns=" + namesList());
                }
                if (source_.encoding != TextEncoding::Utf8)
                    args.push_back("encoding=" + literal(pythonEncoding()));
                args.push_back("try_parse_dates=True");
                args.push_back("infer_schema_length=10000");
                out += "df = " + call("pl.read_csv", args, language_) + "\n";
                break;
            }
            case CodeFormat::Excel:
            case CodeFormat::Ods: // read alike, by its contents
            {
                std::vector<std::string> args = {"path"};
                if (!source_.table.empty())
                    args.push_back("sheet_name=" + literal(source_.table));
                if (!source_.header)
                {
                    args.push_back("has_header=False");
                    rename = true;
                }
                out += "df = " + call("pl.read_excel", args, language_) + "\n";
                break;
            }
            case CodeFormat::Parquet:
                out += "df = pl.read_parquet(path)\n";
                break;
            case CodeFormat::Json:
                out += "df = pl.read_json(path)\n";
                break;
            case CodeFormat::NdJson:
                out += "df = pl.read_ndjson(path)\n";
                break;
            case CodeFormat::Sqlite:
                out += databaseLoad("pl.read_database(" + literal(selectAll(source_)) + ", con)");
                break;
            case CodeFormat::DuckDb:
                out += databaseLoad("con.sql(" + literal(selectAll(source_)) + ").pl()");
                break;
            }
            if (rename)
                out += "df.columns = " + namesList() + "\n";
            return out;
        }

        std::string allOf(const std::vector<std::string> &terms) override
        {
            return "df = " + call("df.filter", terms, language_) + "\n";
        }

        std::string compare(size_t column, FilterOp op, const FilterValue &value, const std::string &text) override
        {
            std::string lhs = expr(column);
            std::string rhs;
            switch (value.kind)
            {
            case FilterValue::Kind::Number:
                rhs = value.number;
                break;
            case FilterValue::Kind::Date:
                if (kind(column) == ColumnKind::DateTime)
                    lhs += ".dt.date()";
                rhs = "pl.date(" + std::to_string(value.time.year) + ", " + std::to_string(value.time.month) + ", " +
                      std::to_string(value.time.day) + ")";
                break;
            case FilterValue::Kind::DateTime:
                rhs = "pl.datetime(" + std::to_string(value.time.year) + ", " + std::to_string(value.time.month) +
                      ", " + std::to_string(value.time.day) + ", " + std::to_string(value.time.hour) + ", " +
                      std::to_string(value.time.minute) + ", " + std::to_string(value.time.second) +
                      (value.time.micros ? ", " + std::to_string(value.time.micros) : std::string()) + ")";
                break;
            case FilterValue::Kind::Boolean:
                rhs = value.boolean ? "True" : "False";
                break;
            case FilterValue::Kind::Text:
                lhs = this->text(column);
                rhs = literal(text);
                break;
            }
            // Like TableSkim, an empty cell is not equal to a value.
            if (op == FilterOp::NotEquals)
                return lhs + ".ne_missing(" + rhs + ")";
            return lhs + " " + comparison(op) + " " + rhs;
        }

        std::string contains(size_t column, const std::string &value, bool negated) override
        {
            // to_lowercase() folds every letter, so the text looked for
            // must be folded alike: letters outside ASCII by Python.
            std::string needle = literal(asciiLower(value));
            if (std::any_of(value.begin(), value.end(), [](char c)
                            { return static_cast<unsigned char>(c) >= 0x80; }))
                needle += ".lower()";
            const std::string detect = ".str.to_lowercase().str.contains(" + needle + ", literal=True)";
            if (negated)
                return "~" + text(column) + ".fill_null(\"\")" + detect;
            return text(column) + detect;
        }

        std::string matches(size_t column, const std::string &pattern, bool negated) override
        {
            const std::string detect = ".str.contains(" + literal("(?i)" + pattern) + ")";
            if (negated)
                return "~" + text(column) + ".fill_null(\"\")" + detect;
            return text(column) + detect;
        }

        std::string empty(size_t column, bool negated) override
        {
            if (!isText(kind(column)))
                return expr(column) + (negated ? ".is_not_null()" : ".is_null()");
            if (negated)
                return expr(column) + ".is_not_null() & (" + expr(column) + " != \"\")";
            return expr(column) + ".is_null() | (" + expr(column) + " == \"\")";
        }

        std::string isNumber(size_t column) override
        {
            if (kind(column) == ColumnKind::Number)
                return expr(column) + ".is_finite()";
            return expr(column) + ".cast(pl.Float64, strict=False).is_not_null()";
        }

        std::string isDate(size_t column) override
        {
            if (isTemporal(kind(column)))
                return expr(column) + ".is_not_null()";
            return text(column) + ".str.to_datetime(strict=False).is_not_null()";
        }

        std::string sort(const std::vector<SortKey> &keys) override
        {
            std::vector<std::string> names, descending;
            for (const SortKey &key : keys)
            {
                names.push_back(name(key.column));
                descending.push_back(key.ascending ? "False" : "True");
            }
            std::vector<std::string> args;
            if (keys.size() == 1)
            {
                args.push_back(col(keys[0].column));
                if (!keys[0].ascending)
                    args.push_back("descending=True");
            }
            else
            {
                args.push_back(literalList(names, language_));
                std::string list = "[";
                for (size_t i = 0; i < descending.size(); ++i)
                    list += (i ? ", " : "") + descending[i];
                args.push_back("descending=" + list + "]");
            }
            args.push_back("nulls_last=True");
            return "df = " + call("df.sort", args, language_) + "\n";
        }

        std::string drop(const std::vector<size_t> &columns) override
        {
            std::vector<std::string> names;
            for (const size_t column : columns)
                names.push_back(name(column));
            return "df = df.drop(" + literalList(names, language_) + ")\n";
        }

        std::string groupBy(const GroupBySpec &spec) override
        {
            std::string out = numericAggregatesNote(spec);
            std::vector<std::string> args = {"pl.len().alias(" + literal(unusedName("count")) + ")"};
            for (const size_t column : spec.values)
            {
                for (const auto &[aggregate, label] : aggregatesOf(spec, column))
                    args.push_back(expr(column) + aggregateCall(aggregate) + ".alias(" + literal(label) + ")");
            }
            if (spec.keys.empty())
                return out + "result = " + call("df.select", args, language_) + "\nprint(result)\n";
            std::vector<std::string> keys;
            for (const size_t column : spec.keys)
                keys.push_back(name(column));
            const std::string key_list = literalList(keys, language_);
            out += "result = (\n    df.group_by(" + key_list + ")\n    .agg(\n";
            for (const std::string &arg : args)
                out += "        " + arg + ",\n";
            out += "    )\n    .sort(" + key_list + ", nulls_last=True)\n)\n";
            return out + "print(result)\n";
        }

        static std::string aggregateCall(Aggregate aggregate)
        {
            switch (aggregate)
            {
            case Aggregate::Count:
                return ".count()";
            case Aggregate::Distinct:
                return ".drop_nulls().n_unique()";
            case Aggregate::Sum:
                return ".sum()";
            case Aggregate::Mean:
                return ".mean()";
            case Aggregate::Min:
                return ".min()";
            case Aggregate::Max:
                return ".max()";
            case Aggregate::StdDev:
                return ".std()";
            }
            return {};
        }

        // plotnine tells groups apart by colour only for discrete values.
        std::string discreteName(size_t column) const
        {
            return isText(kind(column)) ? name(column) : unusedName(name(column) + " group");
        }

        std::string discrete(size_t column) const
        {
            if (isText(kind(column)))
                return {};
            return "df = df.with_columns(" + expr(column) + ".cast(pl.String).alias(" +
                   literal(discreteName(column)) + "))\n";
        }

        std::string plot(const std::string &data, const std::vector<std::string> &aes,
                         const std::vector<std::string> &layers)
        {
            plotnine_.insert({"aes", "ggplot"});
            for (const std::string &layer : layers)
                plotnine_.insert(layer.substr(0, layer.find('(')));
            std::string out = "p = (\n    ggplot(" + data + ", " + call("aes", aes, language_, "    ") + ")";
            for (const std::string &layer : layers)
                out += "\n    + " + layer;
            return out + "\n)\np.show()\n# p.save(\"plot.png\")\n";
        }

        std::string histogram(const HistogramCodeSpec &spec) override
        {
            if (barChart(spec))
            {
                // The most frequent values, as TableSkim lists them.
                const std::string count = unusedName("count");
                const std::string count_arg = count == "count" ? "" : ", name=" + literal(count);
                std::string out = "counts = df[" + col(spec.column) +
                                  "].cast(pl.String).drop_nulls().value_counts(sort=True" + count_arg + ").head(" +
                                  std::to_string(spec.bins) + ")\n";
                if (spec.group_column)
                {
                    std::vector<std::string> keys = {name(spec.column)};
                    std::string casts = expr(spec.column) + ".cast(pl.String)";
                    if (*spec.group_column != spec.column)
                    {
                        keys.push_back(name(*spec.group_column));
                        casts += ", " + expr(*spec.group_column) + ".cast(pl.String)";
                    }
                    out += "top = counts[" + col(spec.column) + "].to_list()\n" +
                           "counts = (\n    df.with_columns(" + casts + ")\n    .filter(" + expr(spec.column) +
                           ".is_in(top))\n    .group_by(" + literalList(keys, language_) + ")\n    .agg(pl.len().alias(" +
                           literal(count) + "))\n)\n";
                    return out + plot("counts", {"x=" + col(spec.column), "y=" + literal(count),
                                                  "fill=" + col(*spec.group_column)},
                                      {"geom_col()", "scale_x_discrete(limits=top)", "labs(y=\"rows\")"});
                }
                return out + plot("counts", {"x=" + col(spec.column), "y=" + literal(count)},
                                  {"geom_col()", "scale_x_discrete(limits=counts[" + col(spec.column) + "].to_list())",
                                   "labs(y=\"rows\")"});
            }
            std::vector<std::string> aes = {"x=" + col(spec.column)};
            std::string out;
            std::string labels = "labs(y=\"rows\")";
            if (spec.group_column)
            {
                out += discrete(*spec.group_column);
                aes.push_back("fill=" + literal(discreteName(*spec.group_column)));
                labels = "labs(y=\"rows\", fill=" + col(*spec.group_column) + ")";
            }
            return out + plot("df", aes, {"geom_histogram(bins=" + std::to_string(spec.bins) + ")", labels});
        }

        std::string scatter(const ScatterCodeSpec &spec) override
        {
            std::vector<std::string> aes = {"x=" + col(spec.x_column), "y=" + col(spec.y_column)};
            if (categoricalX(spec))
                return plot("df", aes, {"geom_boxplot()"});
            std::string out;
            std::vector<std::string> layers = {"geom_point(alpha=0.5)"};
            if (spec.color_column)
            {
                out += discrete(*spec.color_column);
                aes.push_back("color=" + literal(discreteName(*spec.color_column)));
                layers.push_back("labs(color=" + col(*spec.color_column) + ")");
            }
            return out + plot("df", aes, layers);
        }
    };

    class PandasWriter : public PythonWriter
    {
    public:
        using PythonWriter::PythonWriter;

    private:
        std::string series(size_t column) const { return "df[" + col(column) + "]"; }
        std::string text(size_t column) const
        {
            return isText(kind(column)) ? series(column) : series(column) + ".astype(str)";
        }

        std::string load() override
        {
            std::string out = "path = " + literal(source_.path) + "\n";
            switch (source_.format)
            {
            case CodeFormat::Text:
            {
                std::vector<std::string> args = {"path"};
                if (source_.skip_meta_lines)
                {
                    out += metaLineFilter();
                    args[0] = "io.StringIO(text)";
                }
                if (!source_.delimiter)
                {
                    args.push_back("sep=None");
                    args.push_back("engine=\"python\"");
                }
                else if (*source_.delimiter != ',')
                    args.push_back("sep=" + literal(std::string(1, *source_.delimiter)));
                if (!source_.header)
                {
                    args.push_back("header=None");
                    args.push_back("names=" + namesList());
                }
                if (source_.encoding != TextEncoding::Utf8 && !source_.skip_meta_lines)
                    args.push_back("encoding=" + literal(pythonEncoding()));
                const std::string extension = compressionExtension(source_.path);
                if (!source_.skip_meta_lines && (extension == ".bgz" || extension == ".gzip"))
                    args.push_back("compression=\"gzip\"");
                out += "df = " + call("pd.read_csv", args, language_) + "\n";
                break;
            }
            case CodeFormat::Excel:
            case CodeFormat::Ods:
            {
                std::vector<std::string> args = {"path"};
                // Named: pandas tells the engine by the extension otherwise.
                if (source_.format == CodeFormat::Ods)
                    args.push_back("engine=\"odf\"");
                if (!source_.table.empty())
                    args.push_back("sheet_name=" + literal(source_.table));
                if (!source_.header)
                {
                    args.push_back("header=None");
                    args.push_back("names=" + namesList());
                }
                out += "df = " + call("pd.read_excel", args, language_) + "\n";
                break;
            }
            case CodeFormat::Parquet:
                out += "df = pd.read_parquet(path)\n";
                break;
            case CodeFormat::Json:
                out += "df = pd.read_json(path)\n";
                break;
            case CodeFormat::NdJson:
                out += "df = pd.read_json(path, lines=True)\n";
                break;
            case CodeFormat::Sqlite:
                out += databaseLoad("pd.read_sql_query(" + literal(selectAll(source_)) + ", con)");
                break;
            case CodeFormat::DuckDb:
                out += databaseLoad("con.sql(" + literal(selectAll(source_)) + ").df()");
                break;
            }
            return out;
        }

        // pandas reads dates in text as text.
        std::string prepareColumns() override
        {
            std::string out;
            for (const size_t column : usedColumns())
            {
                if (isTemporal(kind(column)))
                    out += series(column) + " = pd.to_datetime(" + series(column) + ", format=\"ISO8601\")\n";
            }
            return out;
        }

        std::string allOf(const std::vector<std::string> &terms) override
        {
            if (terms.size() == 1)
                return "df = df[" + terms[0] + "]\n";
            std::string out = "df = df[\n";
            for (size_t i = 0; i < terms.size(); ++i)
                out += std::string(i ? "    & (" : "    (") + terms[i] + ")\n";
            return out + "]\n";
        }

        std::string compare(size_t column, FilterOp op, const FilterValue &value, const std::string &text) override
        {
            std::string lhs = series(column);
            std::string rhs;
            switch (value.kind)
            {
            case FilterValue::Kind::Number:
                rhs = value.number;
                break;
            case FilterValue::Kind::Date:
                if (kind(column) == ColumnKind::DateTime)
                    lhs += ".dt.normalize()";
                rhs = "pd.Timestamp(\"" + isoDate(value.time) + "\")";
                break;
            case FilterValue::Kind::DateTime:
                rhs = "pd.Timestamp(\"" + isoDateTime(value.time) + "\")";
                break;
            case FilterValue::Kind::Boolean:
                rhs = value.boolean ? "True" : "False";
                break;
            case FilterValue::Kind::Text:
                // TableSkim compares an empty cell as "".
                lhs = isText(kind(column)) ? series(column) + ".fillna(\"\")" : this->text(column);
                rhs = literal(text);
                break;
            }
            return lhs + " " + comparison(op) + " " + rhs;
        }

        std::string contains(size_t column, const std::string &value, bool negated) override
        {
            return (negated ? "~" : "") + text(column) + ".str.contains(" + literal(value) +
                   ", case=False, regex=False, na=False)";
        }

        std::string matches(size_t column, const std::string &pattern, bool negated) override
        {
            return (negated ? "~" : "") + text(column) + ".str.contains(" + literal(pattern) + ", case=False, na=False)";
        }

        std::string empty(size_t column, bool negated) override
        {
            if (!isText(kind(column)))
                return series(column) + (negated ? ".notna()" : ".isna()");
            if (negated)
                return series(column) + ".notna() & (" + series(column) + " != \"\")";
            return series(column) + ".isna() | (" + series(column) + " == \"\")";
        }

        std::string isNumber(size_t column) override
        {
            return "pd.to_numeric(" + series(column) + ", errors=\"coerce\").notna()";
        }

        std::string isDate(size_t column) override
        {
            if (isTemporal(kind(column)))
                return series(column) + ".notna()";
            return "pd.to_datetime(" + series(column) + ", errors=\"coerce\").notna()";
        }

        std::string sort(const std::vector<SortKey> &keys) override
        {
            std::vector<std::string> args;
            if (keys.size() == 1)
            {
                args.push_back(col(keys[0].column));
                if (!keys[0].ascending)
                    args.push_back("ascending=False");
            }
            else
            {
                std::vector<std::string> names;
                std::string ascending = "[";
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    names.push_back(name(keys[i].column));
                    ascending += std::string(i ? ", " : "") + (keys[i].ascending ? "True" : "False");
                }
                args.push_back(literalList(names, language_));
                args.push_back("ascending=" + ascending + "]");
            }
            args.push_back("na_position=\"last\"");
            return "df = " + call("df.sort_values", args, language_) + "\n";
        }

        std::string drop(const std::vector<size_t> &columns) override
        {
            std::vector<std::string> names;
            for (const size_t column : columns)
                names.push_back(name(column));
            return "df = df.drop(columns=" + literalList(names, language_) + ")\n";
        }

        static const char *aggregateFunction(Aggregate aggregate)
        {
            switch (aggregate)
            {
            case Aggregate::Count:
                return "count";
            case Aggregate::Distinct:
                return "nunique";
            case Aggregate::Sum:
                return "sum";
            case Aggregate::Mean:
                return "mean";
            case Aggregate::Min:
                return "min";
            case Aggregate::Max:
                return "max";
            case Aggregate::StdDev:
                return "std";
            }
            return "count";
        }

        std::string groupBy(const GroupBySpec &spec) override
        {
            std::string out = numericAggregatesNote(spec);
            if (spec.keys.empty())
            {
                out += "result = pd.DataFrame(\n    {\n        " + literal(unusedName("count")) + ": [len(df)],\n";
                for (const size_t column : spec.values)
                {
                    for (const auto &[aggregate, label] : aggregatesOf(spec, column))
                        out += "        " + literal(label) + ": [" + series(column) + "." +
                               aggregateFunction(aggregate) + "()],\n";
                }
                return out + "    }\n)\nprint(result)\n";
            }
            std::vector<std::string> keys;
            for (const size_t column : spec.keys)
                keys.push_back(name(column));
            out += "result = (\n    df.groupby(" + literalList(keys, language_) + ", dropna=False)\n    .agg(\n";
            out += "        **{\n            " + literal(unusedName("count")) + ": (" + col(spec.keys[0]) + ", \"size\"),\n";
            for (const size_t column : spec.values)
            {
                for (const auto &[aggregate, label] : aggregatesOf(spec, column))
                    out += "            " + literal(label) + ": (" + col(column) + ", \"" +
                           aggregateFunction(aggregate) + "\"),\n";
            }
            out += "        }\n    )\n    .reset_index()\n)\n";
            return out + "print(result)\n";
        }

        std::string figure(const std::string &body, const std::string &x, const std::string &y)
        {
            imports_.insert("import matplotlib.pyplot as plt");
            return "fig, ax = plt.subplots()\n" + body + "ax.set_xlabel(" + x + ")\nax.set_ylabel(" + y +
                   ")\nfig.tight_layout()\nplt.show()\n# fig.savefig(\"plot.png\")\n";
        }

        std::string histogram(const HistogramCodeSpec &spec) override
        {
            const std::string bins = std::to_string(spec.bins);
            if (barChart(spec))
            {
                if (spec.group_column)
                {
                    const std::vector<std::string> keys = {name(spec.column), name(*spec.group_column)};
                    return "top = " + series(spec.column) + ".value_counts().head(" + bins + ").index\n" +
                           "counts = (\n    df[" + series(spec.column) + ".isin(top)]\n    .groupby(" +
                           literalList(keys, language_) + ", dropna=False)\n    .size()\n    .unstack(fill_value=0)\n"
                           "    .reindex(top, fill_value=0)\n)\n" +
                           figure("counts.plot.bar(stacked=True, ax=ax)\nax.legend(title=" + col(*spec.group_column) + ")\n",
                                  col(spec.column), "\"rows\"");
                }
                // The most frequent values, as TableSkim lists them.
                return "counts = " + series(spec.column) + ".value_counts().head(" + bins + ")\n" +
                       figure("counts.plot.bar(ax=ax)\n", col(spec.column), "\"rows\"");
            }
            if (!spec.group_column)
                return figure("ax.hist(" + series(spec.column) + ".dropna(), bins=" + bins + ")\n", col(spec.column),
                              "\"rows\"");
            const std::string body =
                "groups = [(str(key), rows.dropna()) for key, rows in df.groupby(" + col(*spec.group_column) + ")[" +
                col(spec.column) + "]]\n" + "ax.hist(\n    [rows for _, rows in groups],\n    bins=" + bins +
                ",\n    stacked=True,\n    label=[key for key, _ in groups],\n)\nax.legend(title=" +
                col(*spec.group_column) + ")\n";
            return figure(body, col(spec.column), "\"rows\"");
        }

        std::string scatter(const ScatterCodeSpec &spec) override
        {
            if (categoricalX(spec))
            {
                return figure("df.boxplot(column=" + col(spec.y_column) + ", by=" + col(spec.x_column) + ", ax=ax)\n",
                              col(spec.x_column), col(spec.y_column));
            }
            if (!spec.color_column)
                return figure("ax.scatter(" + series(spec.x_column) + ", " + series(spec.y_column) +
                                  ", s=5, alpha=0.5)\n",
                              col(spec.x_column), col(spec.y_column));
            const std::string body = "for key, rows in df.groupby(" + col(*spec.color_column) + "):\n" +
                                     "    ax.scatter(rows[" + col(spec.x_column) + "], rows[" + col(spec.y_column) +
                                     "], s=5, alpha=0.5, label=str(key))\n" + "ax.legend(title=" +
                                     col(*spec.color_column) + ")\n";
            return figure(body, col(spec.x_column), col(spec.y_column));
        }
    };
} // namespace

const char *codeLanguageName(CodeLanguage language)
{
    switch (language)
    {
    case CodeLanguage::R:
        return "R (tidyverse)";
    case CodeLanguage::Polars:
        return "Python (polars + plotnine)";
    case CodeLanguage::Pandas:
        return "Python (pandas + matplotlib)";
    }
    return "";
}

const char *codeLanguageId(CodeLanguage language)
{
    switch (language)
    {
    case CodeLanguage::R:
        return "r";
    case CodeLanguage::Polars:
        return "polars";
    case CodeLanguage::Pandas:
        return "pandas";
    }
    return "";
}

std::optional<CodeLanguage> parseCodeLanguage(std::string_view id)
{
    for (const CodeLanguage language : CODE_LANGUAGES)
    {
        if (id == codeLanguageId(language))
            return language;
    }
    return std::nullopt;
}

const char *codeLanguageExtension(CodeLanguage language)
{
    return language == CodeLanguage::R ? ".R" : ".py";
}

std::string CodeSource::columnName(size_t column) const
{
    return column < columns.size() ? columns[column] : ::columnLetter(column);
}

ColumnKind CodeSource::kind(size_t column) const
{
    return column < kinds.size() ? kinds[column] : ColumnKind::Unknown;
}

CodeFormat codeFormatOf(const std::filesystem::path &path, bool text_format)
{
    if (text_format)
        return CodeFormat::Text;
    const std::string extension = lowerExtension(stripCompressionExtension(path));
    if (extension == ".xlsx" || extension == ".xlsm" || extension == ".xls")
        return CodeFormat::Excel;
    if (extension == ".parquet" || extension == ".pq")
        return CodeFormat::Parquet;
    if (extension == ".ndjson" || extension == ".jsonl")
        return CodeFormat::NdJson;
    if (extension == ".json")
        return CodeFormat::Json;
    if (extension == ".ods" || extension == ".ots")
        return CodeFormat::Ods;
    // Databases by their header: ".db" may be either. An OpenDocument
    // spreadsheet of another name starts with its "mimetype" ZIP entry.
    std::array<char, 96> head{};
    std::ifstream in(path, std::ios::binary);
    in.read(head.data(), head.size());
    if (std::string_view(head.data(), 4) == std::string_view("PK\x03\x04", 4) &&
        std::string_view(head.data(), head.size())
                .find("mimetypeapplication/vnd.oasis.opendocument.spreadsheet") != std::string_view::npos)
        return CodeFormat::Ods;
    if (std::string_view(head.data(), 16) == std::string_view("SQLite format 3\0", 16))
        return CodeFormat::Sqlite;
    if (std::string_view(head.data() + 8, 4) == "DUCK")
        return CodeFormat::DuckDb;
    if (extension == ".duckdb")
        return CodeFormat::DuckDb;
    if (extension == ".sqlite" || extension == ".sqlite3" || extension == ".db")
        return CodeFormat::Sqlite;
    return CodeFormat::Text;
}

CodeSource codeSourceOf(const std::filesystem::path &path, const TableBook &book, const TableModel &model,
                        std::optional<char> delimiter)
{
    CodeSource source;
    source.path = utf8String(path);
    source.format = codeFormatOf(path, book.isTextFormat());
    if (source.format == CodeFormat::Excel || source.format == CodeFormat::Ods ||
        source.format == CodeFormat::Sqlite || source.format == CodeFormat::DuckDb)
        source.table = model.name();
    if (source.format == CodeFormat::Text)
    {
        source.delimiter = delimiter;
        if (!delimiter)
        {
            const std::string extension = lowerExtension(stripCompressionExtension(path));
            if (extension == ".csv")
                source.delimiter = ',';
            else if (extension == ".tsv" || extension == ".tab" || extension == ".vcf")
                source.delimiter = '\t';
        }
        source.skip_meta_lines = book.skipsMetaLines();
        source.encoding = book.textEncoding().value_or(TextEncoding::Utf8);
    }
    if (source.format == CodeFormat::Text || source.format == CodeFormat::Excel || source.format == CodeFormat::Ods)
        source.header = model.hasHeader();
    const size_t columns = model.columnCount();
    const std::vector<ColumnType> types = model.columnTypes();
    for (size_t column = 0; column < columns; ++column)
    {
        source.columns.push_back(model.columnName(column));
        source.kinds.push_back(column < types.size() ? types[column].kind : ColumnKind::Unknown);
    }
    return source;
}

std::string analysisCode(const AnalysisCodeSpec &spec, CodeLanguage language)
{
    switch (language)
    {
    case CodeLanguage::R:
        return RWriter(spec, language).write();
    case CodeLanguage::Polars:
        return PolarsWriter(spec, language).write();
    case CodeLanguage::Pandas:
        return PandasWriter(spec, language).write();
    }
    return {};
}

std::string analysisCodeFileName(const AnalysisCodeSpec &spec, CodeLanguage language)
{
    std::filesystem::path file = stripCompressionExtension(utf8Path(spec.source.path)).filename();
    std::string name = utf8String(file.stem());
    if (name.empty())
        name = "table";
    if (std::holds_alternative<GroupBySpec>(spec.analysis))
        name += "-group-by";
    else if (std::holds_alternative<HistogramCodeSpec>(spec.analysis))
        name += "-histogram";
    else if (std::holds_alternative<ScatterCodeSpec>(spec.analysis))
        name += "-scatter";
    else
        name += "-view";
    if (language == CodeLanguage::Polars)
        name += "-polars";
    else if (language == CodeLanguage::Pandas)
        name += "-pandas";
    return name + codeLanguageExtension(language);
}

std::filesystem::path analysisCodeTarget(const AnalysisCodeSpec &spec, const std::filesystem::path &chosen,
                                         CodeLanguage language)
{
    const std::string name = utf8String(chosen.filename());
    const std::string extension = codeLanguageExtension(language);
    for (const CodeLanguage suggested_language : CODE_LANGUAGES)
    {
        // "data-view.R" as it is, or as choosers change it for another
        // type: "data-view.R.py", "data-view.py".
        const std::string suggested = analysisCodeFileName(spec, suggested_language);
        const std::string stem = utf8String(utf8Path(suggested).stem());
        if (name == suggested || name == suggested + extension || name == stem + extension)
            return chosen.parent_path() / utf8Path(analysisCodeFileName(spec, language));
    }
    const std::string chosen_extension = lowerExtension(chosen);
    if (chosen_extension == asciiLower(extension))
        return chosen;
    std::filesystem::path target = chosen;
    if (chosen_extension == ".r" || chosen_extension == ".py")
        target.replace_extension(utf8Path(extension)); // the other language's
    else
        target += extension;
    return target;
}

std::optional<CodeLanguage> codeLanguageFromFileName(const std::filesystem::path &path)
{
    const std::string extension = lowerExtension(path);
    if (extension == ".r")
        return CodeLanguage::R;
    if (extension != ".py")
        return std::nullopt;
    const std::string stem = asciiLower(utf8String(path.stem()));
    if (stem.find("polars") != std::string::npos)
        return CodeLanguage::Polars;
    if (stem.find("pandas") != std::string::npos)
        return CodeLanguage::Pandas;
    return std::nullopt;
}
