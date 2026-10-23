#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "compress.hxx"
#include "decompress.hxx"

namespace
{
    std::string decompressed(const std::filesystem::path &path)
    {
        auto in = openDecompressingStream(path, detectCompression(path));
        std::ostringstream text;
        text << in->rdbuf();
        return text.str();
    }

    std::string sample()
    {
        std::string text;
        for (int i = 0; i < 50000; ++i)
            text += std::to_string(i) + ",value " + std::to_string(i * 31 % 977) + "\n";
        return text;
    }
} // namespace

TEST_CASE("openCompressedWriter writes what openDecompressingStream reads")
{
    const auto [compression, bgzf] = GENERATE(std::pair{Compression::None, false}, std::pair{Compression::Gzip, false},
                                              std::pair{Compression::Gzip, true}, std::pair{Compression::Bzip2, false},
                                              std::pair{Compression::Xz, false}, std::pair{Compression::Zstd, false});
    INFO(static_cast<int>(compression) << (bgzf ? " BGZF" : ""));
    const auto path = std::filesystem::temp_directory_path() / "tableskim-test-compressed";
    const std::string text = sample();
    {
        auto writer = openCompressedWriter(path, compression, bgzf);
        // In pieces of all sizes.
        size_t done = 0;
        for (size_t piece = 1; done < text.size(); piece = piece * 3 + 1)
        {
            const size_t n = std::min(piece, text.size() - done);
            writer->write(std::string_view(text).substr(done, n));
            done += n;
        }
        writer->finish();
    }
    CHECK(detectCompression(path) == compression);
    CHECK(isBgzf(path) == bgzf);
    if (compression == Compression::None)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream raw;
        raw << in.rdbuf();
        CHECK(raw.str() == text);
    }
    else
        CHECK(decompressed(path) == text);
    std::filesystem::remove(path);
}

TEST_CASE("BGZF: blocks of at most 64 KiB and the end-of-file block")
{
    const auto path = std::filesystem::temp_directory_path() / "tableskim-test.bgz";
    {
        auto writer = openCompressedWriter(path, Compression::Gzip, true);
        writer->write(sample());
        writer->finish();
    }
    std::string bytes;
    {
        // Closed before the remove below, which Windows refuses otherwise.
        std::ifstream in(path, std::ios::binary);
        std::ostringstream raw;
        raw << in.rdbuf();
        bytes = raw.str();
    }
    size_t blocks = 0;
    for (size_t at = 0; at < bytes.size(); ++blocks)
    {
        REQUIRE(bytes.size() - at >= 28);
        REQUIRE(bytes.compare(at, 4, "\x1f\x8b\x08\x04") == 0);
        REQUIRE(bytes.compare(at + 12, 2, "BC") == 0);
        const size_t size = (static_cast<unsigned char>(bytes[at + 16]) | static_cast<unsigned char>(bytes[at + 17]) << 8) + 1;
        REQUIRE(size <= 65536);
        at += size;
        if (at == bytes.size())
            CHECK(size == 28); // the empty block at the end
    }
    CHECK(blocks > 2);
    std::filesystem::remove(path);
}

TEST_CASE("isBgzf tells BGZF from plain gzip")
{
    CHECK_FALSE(isBgzf(std::filesystem::path(TABLESKIM_DEMO_DATA_DIR) / "normal.csv.gz"));
    CHECK_FALSE(isBgzf(std::filesystem::path(TABLESKIM_DEMO_DATA_DIR) / "normal.csv"));
}
