#include "recent_files.hxx"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "spdlog/spdlog.h"

#include "file_util.hxx"

namespace
{
    // One spelling per file, so that it is listed once.
    std::string normalizedPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(utf8Path(path), ec);
        return ec ? path : utf8String(absolute.lexically_normal());
    }
} // namespace

std::filesystem::path defaultRecentFilesPath()
{
    const std::filesystem::path directory = userDataDirectory();
    return directory.empty() ? std::filesystem::path() : directory / "recent_files";
}

RecentFiles::RecentFiles(std::filesystem::path file, size_t limit) : file_(std::move(file)), limit_(limit)
{
    reload();
}

void RecentFiles::reload()
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
        if (!line.empty() && std::find(entries.begin(), entries.end(), line) == entries.end())
            entries.push_back(line);
    }
    entries_ = std::move(entries);
    if (entries_.size() > limit_)
        entries_.resize(limit_);
}

void RecentFiles::add(const std::string &path)
{
    // A line break would split the entry in the file.
    if (path.empty() || path.find_first_of("\r\n") != std::string::npos)
        return;
    const std::string entry = normalizedPath(path);
    reload();
    std::erase(entries_, entry);
    entries_.insert(entries_.begin(), entry);
    if (entries_.size() > limit_)
        entries_.resize(limit_);
    save();
}

void RecentFiles::remove(const std::string &path)
{
    reload();
    const size_t count = std::erase(entries_, path) + std::erase(entries_, normalizedPath(path));
    if (count > 0)
        save();
}

void RecentFiles::clear()
{
    entries_.clear();
    save();
}

void RecentFiles::save() const
{
    if (file_.empty())
        return;
    std::string contents;
    for (const auto &entry : entries_)
        contents += entry + '\n';
    if (const std::string error = replaceFileContents(file_, contents); !error.empty())
        spdlog::warn("Cannot write the recent files to {}: {}", utf8String(file_), error);
}
