#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "file_util.hxx"
#include "recent_files.hxx"

TEST_CASE("Recent files")
{
    const auto path = std::filesystem::temp_directory_path() / "tableskim-test-recent" / "recent_files";
    std::filesystem::remove_all(path.parent_path());
    const auto absolute = [](const std::string &name)
    { return utf8String((std::filesystem::temp_directory_path() / utf8Path(name)).lexically_normal()); };

    SECTION("newest first, moved to the front when opened again, and limited")
    {
        RecentFiles recent(path, 3);
        recent.add(absolute("a.csv"));
        recent.add(absolute("b.csv"));
        recent.add(absolute("a.csv"));
        CHECK(recent.entries() == std::vector<std::string>{absolute("a.csv"), absolute("b.csv")});
        recent.add(absolute("c.csv"));
        recent.add(absolute("日本語.xlsx"));
        CHECK(recent.entries() == std::vector<std::string>{absolute("日本語.xlsx"), absolute("c.csv"), absolute("a.csv")});

        // Another window's list sees them, and keeps them when it adds.
        RecentFiles other(path, 3);
        CHECK(other.entries() == recent.entries());
        other.add(absolute("d.csv"));
        recent.add(absolute("e.csv"));
        CHECK(recent.entries() == std::vector<std::string>{absolute("e.csv"), absolute("d.csv"), absolute("日本語.xlsx")});
    }
    SECTION("paths are made absolute and normal")
    {
        RecentFiles recent(path);
        const std::string relative = "some-dir/../relative.csv";
        recent.add(relative);
        REQUIRE(recent.entries().size() == 1);
        CHECK(recent.entries()[0] == utf8String((std::filesystem::current_path() / "relative.csv").lexically_normal()));
        recent.add(utf8String(std::filesystem::current_path() / "relative.csv"));
        CHECK(recent.entries().size() == 1);
        recent.add("a\nb.csv");
        CHECK(recent.entries().size() == 1);
    }
    SECTION("remove and clear")
    {
        RecentFiles recent(path);
        recent.add(absolute("a.csv"));
        recent.add(absolute("b.csv"));
        recent.remove(absolute("a.csv"));
        CHECK(recent.entries() == std::vector<std::string>{absolute("b.csv")});
        CHECK(RecentFiles(path).entries() == recent.entries());
        recent.clear();
        CHECK(recent.entries().empty());
        CHECK(RecentFiles(path).entries().empty());
    }
    SECTION("one path per line in the file")
    {
        RecentFiles recent(path);
        recent.add(absolute("a.csv"));
        std::ifstream in(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        CHECK(text == absolute("a.csv") + "\n");
    }
    SECTION("in memory only without a file")
    {
        RecentFiles recent({});
        recent.add(absolute("a.csv"));
        CHECK(recent.entries().size() == 1);
    }
    std::filesystem::remove_all(path.parent_path());
}
