#include "sql_text.hxx"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>

namespace
{
    bool isSpace(char c)
    {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    }

    bool isIdentifierStart(char c)
    {
        // Bytes of multibyte UTF-8 characters count as letters.
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
    }

    bool isIdentifierChar(char c)
    {
        return isIdentifierStart(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '$';
    }

    bool isDigit(char c)
    {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    }

    // Common to DuckDB and SQLite, including type names.
    const std::set<std::string, std::less<>> &keywords()
    {
        static const std::set<std::string, std::less<>> words = {
            "all", "alter", "and", "any", "as", "asc", "attach", "begin", "between", "bigint", "blob", "boolean",
            "by", "case", "cast", "check", "collate", "commit", "conflict", "constraint", "copy", "create", "cross",
            "date", "decimal", "default", "delete", "desc", "describe", "detach", "distinct", "do", "double", "drop",
            "else", "end", "escape", "except", "exists", "explain", "false", "filter", "first", "float", "following",
            "for", "foreign", "from", "full", "glob", "group", "having", "if", "ilike", "in", "index", "inner",
            "insert", "int", "integer", "intersect", "interval", "into", "is", "join", "key", "last", "lateral",
            "left", "like", "limit", "materialized", "natural", "not", "nothing", "null", "nulls", "numeric",
            "offset", "on", "or", "order", "outer", "over", "partition", "pivot", "pragma", "preceding", "primary",
            "qualify", "range", "real", "recursive", "references", "regexp", "release", "replace", "returning",
            "right", "rollback", "row", "rows", "savepoint", "select", "set", "show", "smallint", "some", "summarize",
            "table", "temp", "temporary", "text", "then", "time", "timestamp", "to", "true", "unbounded", "union",
            "unique", "unpivot", "update", "use", "using", "vacuum", "values", "varchar", "view", "when", "where",
            "window", "with", "without",
        };
        return words;
    }

    // Position after whitespace and comments.
    size_t skipBlank(std::string_view sql, size_t pos)
    {
        while (pos < sql.size())
        {
            if (isSpace(sql[pos]))
                ++pos;
            else if (sql.substr(pos, 2) == "--")
            {
                const size_t end = sql.find('\n', pos);
                pos = end == std::string_view::npos ? sql.size() : end + 1;
            }
            else if (sql.substr(pos, 2) == "/*")
            {
                const size_t end = sql.find("*/", pos + 2);
                pos = end == std::string_view::npos ? sql.size() : end + 2;
            }
            else
                break;
        }
        return pos;
    }
} // namespace

std::string trimSqlStatement(std::string_view sql)
{
    size_t first = 0;
    while (first < sql.size() && isSpace(sql[first]))
        ++first;
    size_t last = sql.size();
    while (last > first && (isSpace(sql[last - 1]) || sql[last - 1] == ';'))
        --last;
    return std::string(sql.substr(first, last - first));
}

bool isBlankSql(std::string_view sql)
{
    size_t pos = skipBlank(sql, 0);
    while (pos < sql.size() && sql[pos] == ';')
        pos = skipBlank(sql, pos + 1);
    return pos == sql.size();
}

std::string firstSqlKeyword(std::string_view sql)
{
    size_t pos = skipBlank(sql, 0);
    std::string keyword;
    while (pos < sql.size() && (std::isalpha(static_cast<unsigned char>(sql[pos])) || sql[pos] == '_'))
        keyword += static_cast<char>(std::tolower(static_cast<unsigned char>(sql[pos++])));
    return keyword;
}

std::vector<SqlToken> tokenizeSql(std::string_view sql)
{
    std::vector<SqlToken> tokens;
    auto add = [&tokens](size_t begin, size_t end, SqlTokenKind kind)
    {
        if (begin == end)
            return;
        if (!tokens.empty() && tokens.back().kind == kind && tokens.back().end == begin)
            tokens.back().end = end;
        else
            tokens.push_back({begin, end, kind});
    };
    size_t pos = 0;
    while (pos < sql.size())
    {
        const size_t start = pos;
        const char c = sql[pos];
        if (sql.substr(pos, 2) == "--")
        {
            const size_t newline = sql.find('\n', pos);
            pos = newline == std::string_view::npos ? sql.size() : newline;
            add(start, pos, SqlTokenKind::Comment);
        }
        else if (sql.substr(pos, 2) == "/*")
        {
            const size_t close = sql.find("*/", pos + 2);
            pos = close == std::string_view::npos ? sql.size() : close + 2;
            add(start, pos, SqlTokenKind::Comment);
        }
        else if (c == '\'' || c == '"')
        {
            // A doubled quote stays inside.
            ++pos;
            while (pos < sql.size())
            {
                if (sql[pos] == c)
                {
                    if (pos + 1 < sql.size() && sql[pos + 1] == c)
                        pos += 2;
                    else
                    {
                        ++pos;
                        break;
                    }
                }
                else
                    ++pos;
            }
            add(start, pos, c == '\'' ? SqlTokenKind::String : SqlTokenKind::Plain);
        }
        else if (isIdentifierStart(c))
        {
            while (pos < sql.size() && isIdentifierChar(sql[pos]))
                ++pos;
            std::string word(sql.substr(start, pos - start));
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char ch)
                           { return static_cast<char>(std::tolower(ch)); });
            add(start, pos, keywords().contains(word) ? SqlTokenKind::Keyword : SqlTokenKind::Plain);
        }
        else if (isDigit(c) || (c == '.' && pos + 1 < sql.size() && isDigit(sql[pos + 1])))
        {
            while (pos < sql.size() && (isDigit(sql[pos]) || sql[pos] == '.' || sql[pos] == '_'))
                ++pos;
            if (pos < sql.size() && (sql[pos] == 'e' || sql[pos] == 'E'))
            {
                size_t exponent = pos + 1;
                if (exponent < sql.size() && (sql[exponent] == '+' || sql[exponent] == '-'))
                    ++exponent;
                if (exponent < sql.size() && isDigit(sql[exponent]))
                {
                    pos = exponent;
                    while (pos < sql.size() && isDigit(sql[pos]))
                        ++pos;
                }
            }
            add(start, pos, SqlTokenKind::Number);
        }
        else
        {
            ++pos;
            add(start, pos, SqlTokenKind::Plain);
        }
    }
    return tokens;
}

std::string uniqueSheetName(const std::string &base, const std::vector<std::string> &taken)
{
    std::string name = base;
    for (int n = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++n)
        name = base + " (" + std::to_string(n) + ")";
    return name;
}

namespace
{
    std::string lowerAscii(std::string_view text)
    {
        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return lower;
    }

    bool startsWithIgnoringCase(std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() && lowerAscii(text.substr(0, prefix.size())) == lowerAscii(prefix);
    }

    bool equalIgnoringCase(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && lowerAscii(a) == lowerAscii(b);
    }

    std::string quoteName(std::string_view name)
    {
        std::string quoted = "\"";
        for (const char c : name)
        {
            quoted += c;
            if (c == '"')
                quoted += '"';
        }
        return quoted + "\"";
    }

    // Where the cursor is: in code, or inside a string, a comment or a
    // "quoted identifier" (which starts at `start`).
    struct CursorContext
    {
        enum class Kind
        {
            Code,
            String,
            Comment,
            Quoted,
        };
        Kind kind = Kind::Code;
        size_t start = 0;
    };

    CursorContext cursorContext(std::string_view sql, size_t cursor)
    {
        size_t pos = 0;
        while (pos < cursor)
        {
            const size_t start = pos;
            if (sql.substr(pos, 2) == "--")
            {
                const size_t newline = sql.find('\n', pos);
                if (newline == std::string_view::npos || newline >= cursor)
                    return {CursorContext::Kind::Comment, start};
                pos = newline + 1;
            }
            else if (sql.substr(pos, 2) == "/*")
            {
                const size_t close = sql.find("*/", pos + 2);
                if (close == std::string_view::npos || close + 2 > cursor)
                    return {CursorContext::Kind::Comment, start};
                pos = close + 2;
            }
            else if (sql[pos] == '\'' || sql[pos] == '"')
            {
                const char quote = sql[pos++];
                bool closed = false;
                while (pos < cursor)
                {
                    if (sql[pos] == quote)
                    {
                        if (pos + 1 < cursor && sql[pos + 1] == quote)
                            pos += 2;
                        else
                        {
                            ++pos;
                            closed = true;
                            break;
                        }
                    }
                    else
                        ++pos;
                }
                if (!closed)
                    return {quote == '"' ? CursorContext::Kind::Quoted : CursorContext::Kind::String, start};
            }
            else
                ++pos;
        }
        return {};
    }

    // A word of a statement, for finding the tables it reads.
    struct SqlItem
    {
        enum class Kind
        {
            Name,    // identifier or "quoted", dotted parts joined with "."
            Keyword, // lowercased in `text`
            Punct,
        };
        Kind kind = Kind::Punct;
        std::string text;
        size_t begin = 0;
    };

    std::vector<SqlItem> sqlItems(std::string_view sql, size_t begin, size_t end)
    {
        std::vector<SqlItem> items;
        size_t pos = begin;
        bool join_next = false; // the last item ended with "."
        while (true)
        {
            pos = skipBlank(sql, pos);
            if (pos >= end)
                break;
            const size_t start = pos;
            const char c = sql[pos];
            std::optional<std::string> name;
            if (c == '\'')
            {
                ++pos;
                while (pos < end && !(sql[pos] == '\'' && (pos + 1 >= end || sql[pos + 1] != '\'')))
                    pos += sql[pos] == '\'' ? 2 : 1;
                ++pos;
                items.push_back({SqlItem::Kind::Punct, "'", start});
                join_next = false;
                continue;
            }
            if (c == '"')
            {
                std::string text;
                ++pos;
                while (pos < end)
                {
                    if (sql[pos] == '"')
                    {
                        if (pos + 1 < end && sql[pos + 1] == '"')
                        {
                            text += '"';
                            pos += 2;
                            continue;
                        }
                        ++pos;
                        break;
                    }
                    text += sql[pos++];
                }
                name = std::move(text);
            }
            else if (isIdentifierStart(c))
            {
                while (pos < end && isIdentifierChar(sql[pos]))
                    ++pos;
                const std::string word(sql.substr(start, pos - start));
                if (!join_next && keywords().contains(lowerAscii(word)))
                {
                    items.push_back({SqlItem::Kind::Keyword, lowerAscii(word), start});
                    join_next = false;
                    continue;
                }
                name = word;
            }
            if (name)
            {
                if (join_next && !items.empty() && items.back().kind == SqlItem::Kind::Name)
                    items.back().text += "." + *name;
                else
                    items.push_back({SqlItem::Kind::Name, std::move(*name), start});
                join_next = false;
                continue;
            }
            ++pos;
            if (c == '.' && !items.empty() && items.back().kind == SqlItem::Kind::Name)
                join_next = true;
            else
            {
                items.push_back({SqlItem::Kind::Punct, std::string(1, c), start});
                join_next = false;
            }
        }
        return items;
    }

    struct TableReference
    {
        std::string table;
        std::string alias;
    };

    // FROM a [AS] x, b y ... JOIN c [AS] z
    std::vector<TableReference> tableReferences(const std::vector<SqlItem> &items)
    {
        std::vector<TableReference> references;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items[i].kind != SqlItem::Kind::Keyword || (items[i].text != "from" && items[i].text != "join"))
                continue;
            const bool list = items[i].text == "from";
            size_t j = i + 1;
            while (j < items.size() && items[j].kind == SqlItem::Kind::Name)
            {
                TableReference reference{items[j].text, {}};
                ++j;
                if (j < items.size() && items[j].kind == SqlItem::Kind::Keyword && items[j].text == "as")
                    ++j;
                if (j < items.size() && items[j].kind == SqlItem::Kind::Name)
                    reference.alias = items[j++].text;
                references.push_back(std::move(reference));
                if (!list || j >= items.size() || items[j].text != ",")
                    break;
                ++j;
            }
        }
        return references;
    }

    // "t", "main.t" and "T" all name the table "t" of the default schema.
    const SqlTable *findTable(const std::vector<SqlTable> &schema, std::string_view name)
    {
        for (const auto &table : schema)
        {
            if (equalIgnoringCase(table.name, name) ||
                (startsWithIgnoringCase(name, "main.") && equalIgnoringCase(table.name, name.substr(5))))
                return &table;
        }
        return nullptr;
    }

    // A table name with each dotted part quoted as needed.
    std::string tableIdentifier(std::string_view name)
    {
        std::string result;
        size_t start = 0;
        while (true)
        {
            const size_t dot = name.find('.', start);
            result += sqlIdentifier(name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
            if (dot == std::string_view::npos)
                return result;
            result += '.';
            start = dot + 1;
        }
    }

    // Reads the name that ends at `end` backwards ("quoted" or plain).
    std::optional<std::pair<size_t, std::string>> nameBefore(std::string_view sql, size_t end)
    {
        if (end == 0)
            return std::nullopt;
        if (sql[end - 1] == '"')
        {
            size_t pos = end - 1;
            while (pos > 0)
            {
                const size_t open = sql.rfind('"', pos - 1);
                if (open == std::string_view::npos)
                    return std::nullopt;
                // A doubled quote is part of the name.
                if (open > 0 && sql[open - 1] == '"')
                {
                    pos = open - 1;
                    continue;
                }
                std::string name;
                for (size_t i = open + 1; i + 1 < end; ++i)
                {
                    name += sql[i];
                    if (sql[i] == '"')
                        ++i;
                }
                return std::pair<size_t, std::string>{open, name};
            }
            return std::nullopt;
        }
        size_t begin = end;
        while (begin > 0 && isIdentifierChar(sql[begin - 1]))
            --begin;
        if (begin == end || !isIdentifierStart(sql[begin]))
            return std::nullopt;
        return std::pair<size_t, std::string>{begin, std::string(sql.substr(begin, end - begin))};
    }
} // namespace

std::string sqlIdentifier(std::string_view name)
{
    bool plain = !name.empty() && isIdentifierStart(name[0]) &&
                 std::all_of(name.begin(), name.end(), [](char c)
                             { return isIdentifierChar(c); }) &&
                 !keywords().contains(lowerAscii(name));
    return plain ? std::string(name) : quoteName(name);
}

SqlCompletion completeSql(std::string_view sql, size_t cursor, const std::vector<SqlTable> &schema)
{
    cursor = std::min(cursor, sql.size());
    SqlCompletion result{cursor, cursor, {}};
    const CursorContext context = cursorContext(sql, cursor);
    if (context.kind == CursorContext::Kind::String || context.kind == CursorContext::Kind::Comment)
        return result;

    // The word typed so far, without its quote.
    const bool quoted = context.kind == CursorContext::Kind::Quoted;
    size_t begin = cursor;
    std::string prefix;
    if (quoted)
    {
        begin = context.start;
        for (size_t i = begin + 1; i < cursor; ++i)
        {
            prefix += sql[i];
            if (sql[i] == '"')
                ++i;
        }
    }
    else
    {
        while (begin > 0 && isIdentifierChar(sql[begin - 1]))
            --begin;
        if (begin < cursor && !isIdentifierStart(sql[begin]))
            return result; // a number
        prefix = std::string(sql.substr(begin, cursor - begin));
    }
    result.begin = begin;

    // "a.b." before the word.
    std::string qualifier;
    for (size_t end = begin; end > 0 && sql[end - 1] == '.';)
    {
        const auto part = nameBefore(sql, end - 1);
        if (!part)
            break;
        qualifier = qualifier.empty() ? part->second : part->second + "." + qualifier;
        end = part->first;
    }

    // The statement around the cursor, between semicolons outside strings.
    size_t statement_begin = 0;
    size_t statement_end = sql.size();
    for (const auto &token : tokenizeSql(sql))
    {
        if (token.kind != SqlTokenKind::Plain)
            continue;
        for (size_t i = token.begin; i < token.end; ++i)
        {
            if (sql[i] != ';')
                continue;
            if (i < begin)
                statement_begin = i + 1;
            else if (statement_end == sql.size())
                statement_end = i;
        }
    }
    // The word itself is left out, or "FROM ta" would read "ta" as a table.
    std::vector<SqlItem> items = sqlItems(sql, statement_begin, begin);
    std::vector<SqlItem> after = sqlItems(sql, cursor, statement_end);
    if (!after.empty() && after.front().begin == cursor && cursor < sql.size() && isIdentifierChar(sql[cursor]))
        after.erase(after.begin()); // the rest of the word
    items.insert(items.end(), after.begin(), after.end());
    const std::vector<TableReference> references = tableReferences(items);

    std::set<std::string> seen;
    auto offer = [&](const std::string &candidate)
    {
        if (seen.insert(candidate).second)
            result.candidates.push_back(candidate);
    };
    auto render = [&](std::string_view name)
    { return quoted ? quoteName(name) : sqlIdentifier(name); };
    auto offerColumns = [&](const SqlTable &table)
    {
        for (const auto &column : table.columns)
        {
            if (startsWithIgnoringCase(column, prefix))
                offer(render(column));
        }
    };

    if (!qualifier.empty())
    {
        const SqlTable *table = nullptr;
        for (const auto &reference : references)
        {
            if (!reference.alias.empty() && equalIgnoringCase(reference.alias, qualifier))
                table = findTable(schema, reference.table);
        }
        if (!table)
            table = findTable(schema, qualifier);
        if (table)
            offerColumns(*table);
        // A schema's tables.
        for (const auto &other : schema)
        {
            if (startsWithIgnoringCase(other.name, qualifier + ".") &&
                startsWithIgnoringCase(std::string_view(other.name).substr(qualifier.size() + 1), prefix))
                offer(render(std::string_view(other.name).substr(qualifier.size() + 1)));
        }
        return result;
    }
    if (prefix.empty() && !quoted)
        return result;

    // Right after FROM, JOIN, ...: a table.
    bool table_wanted = false;
    {
        const std::vector<SqlItem> before = sqlItems(sql, statement_begin, begin);
        if (!before.empty() && before.back().kind == SqlItem::Kind::Keyword)
        {
            static const std::set<std::string> table_keywords = {"from", "join", "into", "update", "table",
                                                                 "describe", "summarize"};
            table_wanted = table_keywords.contains(before.back().text);
        }
    }
    std::vector<const SqlTable *> read;
    for (const auto &reference : references)
    {
        if (const SqlTable *table = findTable(schema, reference.table))
            read.push_back(table);
    }
    auto offerTables = [&]
    {
        for (const auto &table : schema)
        {
            if (startsWithIgnoringCase(table.name, prefix))
                offer(quoted ? quoteName(table.name) : tableIdentifier(table.name));
        }
    };
    auto offerKeywords = [&]
    {
        if (quoted)
            return;
        const bool upper = std::any_of(prefix.begin(), prefix.end(), [](unsigned char c)
                                       { return std::isupper(c); });
        for (const auto &keyword : keywords())
        {
            if (startsWithIgnoringCase(keyword, prefix))
            {
                std::string text = keyword;
                if (upper)
                    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::toupper(c)); });
                offer(text);
            }
        }
    };
    if (table_wanted)
    {
        offerTables();
        offerKeywords();
        return result;
    }
    for (const SqlTable *table : read)
        offerColumns(*table);
    offerKeywords();
    offerTables();
    for (const auto &table : schema)
    {
        if (std::find(read.begin(), read.end(), &table) == read.end())
            offerColumns(table);
    }
    return result;
}

std::string commonCompletionPrefix(const std::vector<std::string> &candidates)
{
    if (candidates.empty())
        return {};
    size_t length = candidates.front().size();
    for (const auto &candidate : candidates)
    {
        size_t i = 0;
        while (i < length && i < candidate.size() && candidate[i] == candidates.front()[i])
            ++i;
        length = i;
    }
    // Not in the middle of a UTF-8 character.
    const std::string &first = candidates.front();
    while (length > 0 && length < first.size() && (static_cast<unsigned char>(first[length]) & 0xC0) == 0x80)
        --length;
    return first.substr(0, length);
}
