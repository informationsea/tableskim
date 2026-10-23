#ifndef _FILE_UTIL_HXX_
#define _FILE_UTIL_HXX_

#include <cstdint>
#include <filesystem>
#include <string>

// UI toolkits hand out UTF-8 strings; std::filesystem::path(std::string)
// would use the ANSI code page on Windows.
std::filesystem::path utf8Path(const std::string &path);
std::string utf8String(const std::filesystem::path &path);

// Modification time and size, used to tell whether a file has changed.
bool readFileStamp(const std::filesystem::path &path, std::filesystem::file_time_type &mtime, std::uintmax_t &size);

// Where TableSkim keeps its own files (e.g. the SQL history), not created
// here: %APPDATA%\TableSkim on Windows, ~/Library/Application Support/TableSkim
// on macOS, $XDG_STATE_HOME/tableskim (~/.local/state/tableskim) elsewhere.
// Empty if the home directory is unknown.
std::filesystem::path userDataDirectory();

// A file:// URI for an absolute path, every byte but unreserved characters
// and '/' percent-encoded (so ',', '#', '%', '?' and spaces too).
std::string fileUri(const std::string &absolute_path);

// Shows the file in Finder / Explorer / the desktop's file manager. Returns
// immediately; the file manager is started on a detached thread.
void openWithFileBrowser(const std::filesystem::path &path);

#endif // _FILE_UTIL_HXX_
