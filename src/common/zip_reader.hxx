#ifndef _ZIP_READER_HXX_
#define _ZIP_READER_HXX_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Reads the entries of a ZIP archive (stored or deflated, ZIP64 included)
// as streams, without unpacking them into memory: ODS spreadsheets keep a
// whole workbook in one entry, which may be far larger than the archive.
class ZipArchive
{
public:
    struct Entry
    {
        std::string name;
        uint64_t compressed_size = 0;
        uint64_t size = 0;
        uint64_t local_header_offset = 0;
        uint32_t crc = 0;
        uint16_t method = 0;
        uint16_t flags = 0;
    };

    // Reads the central directory. Throws std::runtime_error.
    explicit ZipArchive(const std::filesystem::path &path);

    const std::filesystem::path &path() const { return path_; }
    const std::vector<Entry> &entries() const { return entries_; }
    // The entry of that name, or null.
    const Entry *find(std::string_view name) const;

private:
    std::filesystem::path path_;
    std::vector<Entry> entries_;
};

// One entry's contents, read from a file handle of its own, so that several
// readers of an archive can be open at once. Corrupt data (a bad CRC, a size
// other than the directory's) makes read() throw std::runtime_error.
class ZipEntryReader
{
public:
    ZipEntryReader(const ZipArchive &archive, const ZipArchive::Entry &entry);
    ~ZipEntryReader();
    ZipEntryReader(const ZipEntryReader &) = delete;
    ZipEntryReader &operator=(const ZipEntryReader &) = delete;

    // Up to `size` bytes; 0 at the end of the entry.
    size_t read(char *out, size_t size);

    // The whole entry, up to `limit` bytes (more throws).
    std::string readAll(size_t limit);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // _ZIP_READER_HXX_
