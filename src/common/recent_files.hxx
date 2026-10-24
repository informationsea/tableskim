#ifndef _RECENT_FILES_HXX_
#define _RECENT_FILES_HXX_

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// "recent_files" in userDataDirectory(); empty if there is none.
std::filesystem::path defaultRecentFilesPath();

// Files opened lately (GUI: File > Open Recent), newest first, as UTF-8
// paths. Kept in a file shared by every window and process, one path per
// line, like SqlHistory: each change reads the file again first, so that
// files opened in other windows are kept.
class RecentFiles
{
public:
    static constexpr size_t DEFAULT_LIMIT = 10;

    // An empty path keeps the list in memory only.
    explicit RecentFiles(std::filesystem::path file = defaultRecentFilesPath(), size_t limit = DEFAULT_LIMIT);

    const std::vector<std::string> &entries() const { return entries_; }
    // Makes the file (made absolute) the newest entry, moving it there if
    // it is listed already. File errors are logged, not thrown.
    void add(const std::string &path);
    // Takes a file off the list (e.g. one that cannot be opened any more).
    void remove(const std::string &path);
    void clear();
    // Reads the file again.
    void reload();

private:
    std::filesystem::path file_;
    size_t limit_;
    std::vector<std::string> entries_;

    void save() const;
};

#endif // _RECENT_FILES_HXX_
