#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "test_zip.hxx"
#include "zip_reader.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    std::string readEntry(const ZipArchive &archive, std::string_view name, size_t piece)
    {
        const auto *entry = archive.find(name);
        REQUIRE(entry);
        ZipEntryReader reader(archive, *entry);
        std::string data;
        std::string buffer(piece, '\0');
        while (const size_t got = reader.read(buffer.data(), buffer.size()))
            data.append(buffer, 0, got);
        return data;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    void writeFile(const std::filesystem::path &path, const std::string &data)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
} // namespace

TEST_CASE("ZIP reader: stored entries, with and without ZIP64")
{
    for (const bool zip64 : {false, true})
    {
        INFO("zip64: " << zip64);
        const auto path = std::filesystem::temp_directory_path() / "tableskim-test-stored.zip";
        const std::string big(200000, 'x');
        writeTestZip(path, {{"a.txt", "hello"}, {"dir/empty", ""}, {"big", big}}, zip64);
        const ZipArchive archive(path);
        REQUIRE(archive.entries().size() == 3);
        CHECK(archive.find("missing") == nullptr);
        CHECK(readEntry(archive, "a.txt", 2) == "hello");
        CHECK(readEntry(archive, "dir/empty", 16).empty());
        CHECK(readEntry(archive, "big", 4096) == big);
        {
            // Closed before the file is removed: Windows cannot remove an open file.
            ZipEntryReader reader(archive, *archive.find("big"));
            CHECK_THROWS_AS(reader.readAll(1000), std::runtime_error);
        }
        std::filesystem::remove(path);
    }
}

TEST_CASE("ZIP reader: deflated entries read alike in any piece size")
{
    const ZipArchive archive(kDemoData / "large.ods");
    const auto *content = archive.find("content.xml");
    REQUIRE(content);
    CHECK(content->method == 8);
    const std::string whole = ZipEntryReader(archive, *content).readAll(content->size);
    CHECK(whole.size() == content->size);
    CHECK(whole.find("<office:spreadsheet>") != std::string::npos);
    CHECK(readEntry(archive, "content.xml", 13) == whole);
    CHECK(readEntry(archive, "content.xml", 7777) == whole);
    CHECK(readEntry(archive, "mimetype", 1) == "application/vnd.oasis.opendocument.spreadsheet");
}

TEST_CASE("ZIP reader: corrupt archives fail cleanly")
{
    const auto path = std::filesystem::temp_directory_path() / "tableskim-test-corrupt.zip";

    SECTION("not a ZIP archive")
    {
        writeFile(path, "PK\x03\x04 this is not really a zip");
        CHECK_THROWS_AS(ZipArchive(path), std::runtime_error);
    }
    SECTION("truncated")
    {
        const std::string data = readFile(kDemoData / "normal.ods");
        writeFile(path, data.substr(0, data.size() / 2));
        CHECK_THROWS_AS(ZipArchive(path), std::runtime_error);
    }
    SECTION("damaged compressed data")
    {
        std::string data = readFile(kDemoData / "large.ods");
        const ZipArchive original(kDemoData / "large.ods");
        const auto *content = original.find("content.xml");
        REQUIRE(content);
        // Somewhere in the middle of the entry's compressed data.
        data[static_cast<size_t>(content->local_header_offset + 100 + content->compressed_size / 2)] ^= 0x55;
        writeFile(path, data);
        const ZipArchive archive(path);
        CHECK_THROWS_AS(ZipEntryReader(archive, *archive.find("content.xml")).readAll(content->size + 1),
                        std::runtime_error);
    }
    SECTION("bad CRC of a stored entry")
    {
        writeTestZip(path, {{"a.txt", "hello"}});
        std::string data = readFile(path);
        data[30 + 5] = 'j'; // "jello"
        writeFile(path, data);
        const ZipArchive archive(path);
        CHECK_THROWS_AS(ZipEntryReader(archive, *archive.find("a.txt")).readAll(100), std::runtime_error);
    }
    std::filesystem::remove(path);
}
