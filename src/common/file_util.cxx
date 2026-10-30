#include "file_util.hxx"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// Not declared by every libc's headers (e.g. macOS).
extern char **environ;

namespace
{
    // Runs a command with the current environment, discarding its output, and
    // returns its exit status (-1 if it could not be started).
    int runQuietly(const char *const args[])
    {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        pid_t pid;
        int err = posix_spawn(&pid, args[0], &actions, NULL, const_cast<char **>(args), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (err != 0)
            return -1;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
            return -1;
        return WEXITSTATUS(status);
    }
}
#endif

std::filesystem::path utf8Path(const std::string &path)
{
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::string utf8String(const std::filesystem::path &path)
{
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

std::string lowerExtension(const std::filesystem::path &path)
{
    std::string ext = utf8String(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::filesystem::path userDataDirectory()
{
#ifdef _WIN32
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (!appdata || !*appdata)
        return {};
    return std::filesystem::path(appdata) / "TableSkim";
#else
    const char *home = std::getenv("HOME");
#ifdef __APPLE__
    if (!home || !*home)
        return {};
    return std::filesystem::path(home) / "Library" / "Application Support" / "TableSkim";
#else
    if (const char *state = std::getenv("XDG_STATE_HOME"); state && *state == '/')
        return std::filesystem::path(state) / "tableskim";
    if (!home || !*home)
        return {};
    return std::filesystem::path(home) / ".local" / "state" / "tableskim";
#endif
#endif
}

std::string replaceFileContents(const std::filesystem::path &file, const std::string &contents)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path partial = file;
    static std::mt19937_64 random{std::random_device{}()};
    static std::mutex random_mutex;
    {
        std::lock_guard lock(random_mutex);
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), ".%016llx", static_cast<unsigned long long>(random()));
        partial += suffix;
    }
    partial += ".tableskim-partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << contents;
        out.close();
        if (!out)
        {
            std::filesystem::remove(partial, ec);
            return "cannot write " + utf8String(partial);
        }
    }
    // Windows refuses to replace a file another window is reading at that
    // moment, so try again briefly.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        std::filesystem::rename(partial, file, ec);
        if (!ec)
            return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::string error = ec.message();
    std::filesystem::remove(partial, ec);
    return error;
}

bool readFileStamp(const std::filesystem::path &path, std::filesystem::file_time_type &mtime, std::uintmax_t &size)
{
    std::error_code ec;
    mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
        return false;
    size = std::filesystem::file_size(path, ec);
    return !ec;
}

std::string fileUri(const std::string &absolute_path)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string uri = "file://";
    for (const char c : absolute_path)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) && byte < 0x80)
            uri += c;
        else if (c == '/' || c == '-' || c == '.' || c == '_' || c == '~')
            uri += c;
        else
        {
            uri += '%';
            uri += hex[byte >> 4];
            uri += hex[byte & 0x0F];
        }
    }
    return uri;
}

void openWithFileBrowser(const std::filesystem::path &path_arg)
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::absolute(path_arg, ec);
    if (ec)
        path = path_arg;
    // path.string() throws on Windows for characters outside the ANSI code page.
    const std::u8string u8 = path.u8string();
    spdlog::debug("Open with file browser: {}", std::string(u8.begin(), u8.end()));
#ifdef _WIN32
    // Explorer may take a while to respond, so keep it off the UI thread.
    // ILCreateFromPathW does not accept forward slashes.
    path.make_preferred();
    std::thread([path_string = path.wstring(),
                 dir_string = path.parent_path().wstring()]()
                {
        // SHOpenFolderAndSelectItems needs COM on the calling thread.
        const HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool shown = false;
        if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(path_string.c_str()))
        {
            shown = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, NULL, 0));
            ILFree(item);
        }
        if (!shown)
        {
            spdlog::debug("SHOpenFolderAndSelectItems failed; opening the folder instead");
            const auto result = reinterpret_cast<INT_PTR>(
                ShellExecuteW(NULL, L"explore", dir_string.c_str(), NULL, NULL, SW_SHOWNORMAL));
            if (result <= 32)
                spdlog::warn("Failed to open file browser (error {})", static_cast<long long>(result));
        }
        if (SUCCEEDED(init))
            CoUninitialize(); })
        .detach();
#elif __APPLE__
    std::thread([path_string = path.string()]()
                {
        const char *args[] = {
            "/usr/bin/open",
            "-R",
            path_string.c_str(),
            NULL};
        runQuietly(args); })
        .detach();
#else
    // Waiting for the child would block the UI, so run it on a detached thread.
    std::thread([dir_string = path.parent_path().string(), path_string = path.string()]()
                {
        // ShowItems selects the file, but needs a file manager implementing
        // org.freedesktop.FileManager1; --print-reply makes failures visible.
        // dbus-send splits array items at ',', which the URI encodes.
        std::string item = "array:string:" + fileUri(path_string);
        const char *dbus_args[] = {
            "/usr/bin/dbus-send",
            "--session",
            "--print-reply",
            "--dest=org.freedesktop.FileManager1",
            "--type=method_call",
            "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1.ShowItems",
            item.c_str(),
            "string:",
            NULL};
        if (runQuietly(dbus_args) == 0)
            return;
        spdlog::debug("FileManager1.ShowItems failed; falling back to xdg-open");
        const char *xdg_open_args[] = {
            "/usr/bin/xdg-open",
            dir_string.c_str(),
            NULL};
        if (runQuietly(xdg_open_args) != 0)
            spdlog::warn("Failed to open file browser for {}", dir_string); })
        .detach();
#endif
}