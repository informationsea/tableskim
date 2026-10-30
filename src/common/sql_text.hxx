#ifndef _SQL_TEXT_HXX_
#define _SQL_TEXT_HXX_

#include <string>
#include <string_view>
#include <vector>

// Helpers for DataSource::executeSql() in the SQL backends.

// "name" with its double quotes doubled, as SQLite and DuckDB read it.
std::string quoteSqlIdentifier(std::string_view name);
// 'text' with its single quotes doubled.
std::string quoteSqlString(std::string_view text);

// Base name of the sheet that shows a query's rows.
inline constexpr const char *SQL_RESULT_SHEET = "SQL Result";

// The statement without surrounding whitespace and trailing semicolons, so
// that it can follow "CREATE VIEW ... AS".
std::string trimSqlStatement(std::string_view sql);

// Whether there is nothing but whitespace, comments and semicolons.
bool isBlankSql(std::string_view sql);

// The first keyword, lowercased, after whitespace and comments ("" if none).
std::string firstSqlKeyword(std::string_view sql);

// Lexical classes for highlighting; there is no parsing.
enum class SqlTokenKind
{
    Plain, // identifiers, operators, whitespace, "quoted identifiers"
    Keyword,
    String, // '...', with '' inside
    Number,
    Comment, // -- to the end of the line, /* ... */
};

struct SqlToken
{
    size_t begin = 0; // byte offsets
    size_t end = 0;
    SqlTokenKind kind = SqlTokenKind::Plain;
    bool operator==(const SqlToken &) const = default;
};

// Covers the whole text, in order, adjacent tokens of one kind merged. An
// unterminated string or comment (still being typed) runs to the end.
std::vector<SqlToken> tokenizeSql(std::string_view sql);

// A table or view that SQL can name, for completion.
struct SqlTable
{
    std::string name; // "schema.name" outside the default schema
    std::vector<std::string> columns;
    bool operator==(const SqlTable &) const = default;
};

struct SqlCompletion
{
    size_t begin = 0; // the bytes [begin, end) that a candidate replaces
    size_t end = 0;
    std::vector<std::string> candidates; // as inserted, the likeliest first
};

// Candidates for the word before `cursor` (a byte offset), matched
// case-insensitively: column names (those of the tables the statement reads
// first), table names, and keywords, which follow the word's case. After
// "name." only that table's columns (or a schema's tables) are offered, by
// table name or by an alias given in FROM / JOIN; elsewhere a word must
// have been started. Names are quoted where SQL needs it. Nothing inside
// strings and comments.
SqlCompletion completeSql(std::string_view sql, size_t cursor, const std::vector<SqlTable> &schema);

// The longest start shared by all candidates (whole UTF-8 characters).
std::string commonCompletionPrefix(const std::vector<std::string> &candidates);

// `name` as SQL can name it: as is if it is a plain identifier that is not
// a keyword, else in double quotes.
std::string sqlIdentifier(std::string_view name);

// `base`, or "base (2)", "base (3)", ... if that is taken.
std::string uniqueSheetName(const std::string &base, const std::vector<std::string> &taken);

#endif // _SQL_TEXT_HXX_
