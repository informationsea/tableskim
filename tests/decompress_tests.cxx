#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "decompress.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    void writeFile(const std::filesystem::path &path, const std::string &data)
    {
        std::ofstream file(path, std::ios::binary);
        file << data;
    }

    std::string decompress(const std::filesystem::path &path)
    {
        auto stream = openDecompressingStream(path, detectCompression(path));
        std::string data;
        char buffer[4096];
        while (stream->read(buffer, sizeof(buffer)) || stream->gcount() > 0)
            data.append(buffer, static_cast<size_t>(stream->gcount()));
        return data;
    }

    std::filesystem::path tempFile(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / ("tableskim-test-" + name);
    }

    uint32_t xzCrc32(const char *data, size_t size)
    {
        uint32_t crc = UINT32_MAX;
        for (size_t i = 0; i < size; ++i)
        {
            crc ^= static_cast<unsigned char>(data[i]);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        return ~crc;
    }
} // namespace

TEST_CASE("compression is detected from magic numbers and extensions")
{
    CHECK(detectCompression(kDemoData / "normal.csv") == Compression::None);
    CHECK(detectCompression(kDemoData / "normal.csv.gz") == Compression::Gzip);
    CHECK(detectCompression(kDemoData / "normal.csv.bz2") == Compression::Bzip2);
    CHECK(detectCompression(kDemoData / "normal.csv.xz") == Compression::Xz);
    CHECK(detectCompression(kDemoData / "normal.csv.zst") == Compression::Zstd);
    CHECK(detectCompression(std::span<const std::byte>{}) == Compression::None);

    CHECK(compressionFromExtension("a.CSV.GZ") == Compression::Gzip);
    CHECK(compressionFromExtension("a.tsv.zst") == Compression::Zstd);
    CHECK(compressionFromExtension("a.csv") == Compression::None);
    CHECK(stripCompressionExtension("dir/a.tsv.xz") == std::filesystem::path("dir/a.tsv"));
    CHECK(stripCompressionExtension("dir/a.csv") == std::filesystem::path("dir/a.csv"));
}

TEST_CASE("every format decompresses to the original bytes")
{
    const std::string expected = readFile(kDemoData / "normal.csv");
    REQUIRE_FALSE(expected.empty());
    for (const char *file : {"normal.csv.gz", "normal.csv.bz2", "normal.csv.xz", "normal.csv.zst"})
    {
        INFO(file);
        CHECK(decompress(kDemoData / file) == expected);
    }
}

TEST_CASE("concatenated streams are read to the end")
{
    const std::string expected = readFile(kDemoData / "normal.csv");
    for (const char *file : {"normal.csv.gz", "normal.csv.bz2", "normal.csv.xz", "normal.csv.zst"})
    {
        INFO(file);
        const auto path = tempFile(std::string("concat-") + file);
        const std::string compressed = readFile(kDemoData / file);
        writeFile(path, compressed + compressed);
        CHECK(decompress(path) == expected + expected);
        std::filesystem::remove(path);
    }
}

TEST_CASE("zero padding after a stream is ignored", "[regression]")
{
    // As tape tools leave it, and gzip -d accepts.
    const std::string expected = readFile(kDemoData / "normal.csv");
    const std::string padding(512, '\0');
    for (const char *file : {"normal.csv.gz", "normal.csv.bz2", "normal.csv.xz", "normal.csv.zst"})
    {
        INFO(file);
        const auto path = tempFile(std::string("padded-") + file);
        const std::string compressed = readFile(kDemoData / file);
        writeFile(path, compressed + padding);
        CHECK(decompress(path) == expected);
        // Between two streams too.
        writeFile(path, compressed + padding + compressed);
        CHECK(decompress(path) == expected + expected);
        std::filesystem::remove(path);
    }
}

TEST_CASE("truncated or corrupt data throws instead of ending silently")
{
    for (const char *file : {"normal.csv.gz", "normal.csv.bz2", "normal.csv.xz", "normal.csv.zst"})
    {
        INFO(file);
        const auto path = tempFile(std::string("broken-") + file);
        const std::string compressed = readFile(kDemoData / file);

        writeFile(path, compressed.substr(0, compressed.size() / 2));
        CHECK_THROWS_AS(decompress(path), std::runtime_error);

        std::string corrupt = compressed;
        for (size_t i = 16; i < corrupt.size() - 16; i += 7)
            corrupt[i] = static_cast<char>(~corrupt[i]);
        writeFile(path, corrupt);
        CHECK_THROWS_AS(decompress(path), std::runtime_error);
        std::filesystem::remove(path);
    }
}

TEST_CASE("xz streams exceeding the memory limit are rejected")
{
    const auto path = tempFile("memory-limit.xz");
    std::string compressed = readFile(kDemoData / "normal.csv.xz");

    // The demo file has a single LZMA2 filter. Change its dictionary property
    // from 8 MiB (0x16) to 512 MiB (0x22), then repair the Block Header CRC.
    REQUIRE(compressed.size() > 24);
    REQUIRE(static_cast<unsigned char>(compressed[12]) == 0x02);
    REQUIRE(static_cast<unsigned char>(compressed[14]) == 0x21);
    REQUIRE(static_cast<unsigned char>(compressed[15]) == 0x01);
    compressed[16] = 0x22;
    const uint32_t crc = xzCrc32(compressed.data() + 12, 8);
    for (size_t i = 0; i < 4; ++i)
        compressed[20 + i] = static_cast<char>(crc >> (i * 8));

    writeFile(path, compressed);
    std::string error;
    try
    {
        decompress(path);
    }
    catch (const std::runtime_error &e)
    {
        error = e.what();
    }
    CHECK(error == "XZ data exceeds the 256 MiB memory limit");
    std::filesystem::remove(path);
}

TEST_CASE("opening a missing file throws")
{
    CHECK_THROWS_AS(openDecompressingStream(tempFile("missing.gz"), Compression::Gzip), std::runtime_error);
}
