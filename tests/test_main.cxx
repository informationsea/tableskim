// main() of tableskim-tests: runs the Catch2 tests with a temporary directory
// of their own. ctest runs every test case in a process of its own
// (catch_discover_tests), possibly in parallel, and many tests write files of
// fixed names into std::filesystem::temp_directory_path().

#include <catch2/catch_session.hpp>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

int main(int argc, char *argv[])
{
    std::random_device random;
    const auto directory = std::filesystem::temp_directory_path() /
                           ("tableskim-tests-" + std::to_string(random()) + "-" + std::to_string(random()));
    std::filesystem::create_directories(directory);
    // temp_directory_path() reads TMPDIR on POSIX, TMP and TEMP on Windows.
#ifdef _WIN32
    _wputenv_s(L"TMP", directory.c_str());
    _wputenv_s(L"TEMP", directory.c_str());
#else
    setenv("TMPDIR", directory.c_str(), 1);
#endif

    const int result = Catch::Session().run(argc, argv);

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    return result;
}
