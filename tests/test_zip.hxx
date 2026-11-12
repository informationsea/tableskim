#ifndef _TEST_ZIP_HXX_
#define _TEST_ZIP_HXX_

// Writes small ZIP archives (stored entries) for the tests, optionally with
// ZIP64 records, so that tests can build ODS files of their own.

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

struct TestZipEntry
{
    std::string name;
    std::string data;
};

inline uint32_t testCrc32(std::string_view data)
{
    static const auto table = []
    {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (const char ch : data)
        crc = table[(crc ^ static_cast<unsigned char>(ch)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

inline void writeTestZip(const std::filesystem::path &path, const std::vector<TestZipEntry> &entries, bool zip64 = false)
{
    std::string out;
    auto u16 = [&](uint32_t v)
    {
        out += static_cast<char>(v & 0xFF);
        out += static_cast<char>((v >> 8) & 0xFF);
    };
    auto u32 = [&](uint32_t v)
    {
        u16(v & 0xFFFF);
        u16(v >> 16);
    };
    auto u64 = [&](uint64_t v)
    {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    };
    const uint32_t saturated = 0xFFFFFFFFu;

    std::vector<uint64_t> offsets;
    for (const TestZipEntry &entry : entries)
    {
        offsets.push_back(out.size());
        const auto size = static_cast<uint32_t>(entry.data.size());
        u32(0x04034b50);
        u16(zip64 ? 45 : 20);
        u16(0); // flags
        u16(0); // stored
        u16(0);
        u16(0x21);
        u32(testCrc32(entry.data));
        u32(zip64 ? saturated : size);
        u32(zip64 ? saturated : size);
        u16(static_cast<uint32_t>(entry.name.size()));
        u16(zip64 ? 20 : 0);
        out += entry.name;
        if (zip64)
        {
            u16(0x0001);
            u16(16);
            u64(entry.data.size());
            u64(entry.data.size());
        }
        out += entry.data;
    }

    const uint64_t directory_offset = out.size();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const TestZipEntry &entry = entries[i];
        const auto size = static_cast<uint32_t>(entry.data.size());
        u32(0x02014b50);
        u16(zip64 ? 45 : 20);
        u16(zip64 ? 45 : 20);
        u16(0);
        u16(0);
        u16(0);
        u16(0x21);
        u32(testCrc32(entry.data));
        u32(zip64 ? saturated : size);
        u32(zip64 ? saturated : size);
        u16(static_cast<uint32_t>(entry.name.size()));
        u16(zip64 ? 28 : 0);
        u16(0); // comment
        u16(0); // disk
        u16(0);
        u32(0);
        u32(zip64 ? saturated : static_cast<uint32_t>(offsets[i]));
        out += entry.name;
        if (zip64)
        {
            u16(0x0001);
            u16(24);
            u64(entry.data.size());
            u64(entry.data.size());
            u64(offsets[i]);
        }
    }
    const uint64_t directory_size = out.size() - directory_offset;

    if (zip64)
    {
        const uint64_t record_offset = out.size();
        u32(0x06064b50);
        u64(44);
        u16(45);
        u16(45);
        u32(0);
        u32(0);
        u64(entries.size());
        u64(entries.size());
        u64(directory_size);
        u64(directory_offset);
        u32(0x07064b50);
        u32(0);
        u64(record_offset);
        u32(1);
    }
    u32(0x06054b50);
    u16(0);
    u16(0);
    u16(zip64 ? 0xFFFF : static_cast<uint32_t>(entries.size()));
    u16(zip64 ? 0xFFFF : static_cast<uint32_t>(entries.size()));
    u32(zip64 ? saturated : static_cast<uint32_t>(directory_size));
    u32(zip64 ? saturated : static_cast<uint32_t>(directory_offset));
    u16(0);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
}

#endif // _TEST_ZIP_HXX_
