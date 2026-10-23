#ifndef _SQL_HISTORY_HXX_
#define _SQL_HISTORY_HXX_

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// "sql_history" in userDataDirectory(); empty if there is none.
std::filesystem::path defaultSqlHistoryPath();

// SQL run in the editors, oldest first. Kept in a file shared by every
// window and both UIs: one entry per line, with "\\", "\n" and "\r" escaped.
class SqlHistory
{
public:
    static constexpr size_t DEFAULT_LIMIT = 200;

    // An empty path keeps the history in memory only.
    explicit SqlHistory(std::filesystem::path file = defaultSqlHistoryPath(), size_t limit = DEFAULT_LIMIT);

    const std::vector<std::string> &entries() const { return entries_; }
    // Adds SQL (trimmed; blank SQL is ignored) as the newest entry, moving an
    // identical one there. The file is read again first, so that entries
    // added by other windows are kept, and then written; file errors are
    // logged, not thrown.
    void add(const std::string &sql);
    // Reads the file again.
    void reload();

private:
    std::filesystem::path file_;
    size_t limit_;
    std::vector<std::string> entries_;

    void save() const;
};

// Walks history entries from the text being edited, like a shell's Up and
// Down. The first previous() remembers that text, and next() past the
// newest entry brings it back.
class SqlHistoryCursor
{
public:
    // The entry before the current one (from the edited text: the newest);
    // nullopt at the oldest, or if there are none.
    std::optional<std::string> previous(const std::vector<std::string> &entries, const std::string &current);
    // The entry after the current one, or the edited text after the newest;
    // nullopt if not walking.
    std::optional<std::string> next(const std::vector<std::string> &entries);
    // Back to editing (after running, or once the text is changed by hand).
    void reset();
    bool walking() const { return index_.has_value(); }

private:
    std::optional<size_t> index_;
    std::string draft_;
};

// For the file: one line.
std::string escapeSqlHistoryEntry(const std::string &sql);
std::string unescapeSqlHistoryEntry(const std::string &line);

#endif // _SQL_HISTORY_HXX_
