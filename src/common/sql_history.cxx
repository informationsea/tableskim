#include "sql_history.hxx"

#include <algorithm>
#include <fstream>

#include "spdlog/spdlog.h"

#include "file_util.hxx"
#include "sql_text.hxx"

std::filesystem::path defaultSqlHistoryPath()
{
    const std::filesystem::path directory = userDataDirectory();
    return directory.empty() ? std::filesystem::path() : directory / "sql_history";
}

std::string escapeSqlHistoryEntry(const std::string &sql)
{
    std::string line;
    line.reserve(sql.size());
    for (const char c : sql)
    {
        if (c == '\\')
            line += "\\\\";
        else if (c == '\n')
            line += "\\n";
        else if (c == '\r')
            line += "\\r";
        else
            line += c;
    }
    return line;
}

std::string unescapeSqlHistoryEntry(const std::string &line)
{
    std::string sql;
    sql.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] != '\\' || i + 1 == line.size())
        {
            sql += line[i];
            continue;
        }
        const char c = line[++i];
        sql += c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return sql;
}

SqlHistory::SqlHistory(std::filesystem::path file, size_t limit) : file_(std::move(file)), limit_(limit)
{
    reload();
}

void SqlHistory::reload()
{
    if (file_.empty())
        return;
    std::ifstream in(file_, std::ios::binary);
    if (!in)
        return; // none yet
    std::vector<std::string> entries;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back(); // edited on Windows
        if (!line.empty())
            entries.push_back(unescapeSqlHistoryEntry(line));
    }
    entries_ = std::move(entries);
    if (entries_.size() > limit_)
        entries_.erase(entries_.begin(), entries_.end() - static_cast<std::ptrdiff_t>(limit_));
}

void SqlHistory::add(const std::string &sql)
{
    const std::string entry = trimSqlStatement(sql);
    if (entry.empty())
        return;
    reload();
    std::erase(entries_, entry);
    entries_.push_back(entry);
    if (entries_.size() > limit_)
        entries_.erase(entries_.begin(), entries_.end() - static_cast<std::ptrdiff_t>(limit_));
    save();
}

void SqlHistory::save() const
{
    if (file_.empty())
        return;
    std::string contents;
    for (const auto &entry : entries_)
        contents += escapeSqlHistoryEntry(entry) + '\n';
    if (const std::string error = replaceFileContents(file_, contents); !error.empty())
        spdlog::warn("Cannot write the SQL history to {}: {}", utf8String(file_), error);
}

std::optional<std::string> SqlHistoryCursor::previous(const std::vector<std::string> &entries,
                                                      const std::string &current)
{
    if (entries.empty())
        return std::nullopt;
    if (!index_)
    {
        draft_ = current;
        index_ = entries.size();
    }
    // The entries may have shrunk (another window's limit); stay in range.
    *index_ = std::min(*index_, entries.size());
    if (*index_ == 0)
        return std::nullopt;
    --*index_;
    return entries[*index_];
}

std::optional<std::string> SqlHistoryCursor::next(const std::vector<std::string> &entries)
{
    if (!index_)
        return std::nullopt;
    if (*index_ + 1 >= entries.size())
    {
        std::string draft = std::move(draft_);
        reset();
        return draft;
    }
    ++*index_;
    return entries[*index_];
}

void SqlHistoryCursor::reset()
{
    index_.reset();
    draft_.clear();
}
