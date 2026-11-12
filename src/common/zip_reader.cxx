#include "zip_reader.hxx"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <zlib.h>

#include "file_util.hxx"

namespace
{
    constexpr uint32_t kEndOfCentralDirectory = 0x06054b50;
    constexpr uint32_t kZip64EndLocator = 0x07064b50;
    constexpr uint32_t kZip64End = 0x06064b50;
    constexpr uint32_t kCentralHeader = 0x02014b50;
    constexpr uint32_t kLocalHeader = 0x04034b50;
    constexpr size_t kEndRecordSize = 22;
    constexpr size_t kMaxCommentSize = 0xFFFF;
    // An ODS file's directory holds a few dozen entries; anything larger is
    // not a document.
    constexpr uint64_t kMaxCentralDirectorySize = 64ULL << 20;
    // Reads of a file are dear in a browser: read in large pieces.
    constexpr size_t kInputBufferSize = 1 << 20;

    [[noreturn]] void corrupt(const std::string &what)
    {
        throw std::runtime_error("Corrupt ZIP archive: " + what);
    }

    uint16_t le16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
    uint32_t le32(const unsigned char *p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    uint64_t le64(const unsigned char *p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

    std::ifstream openFile(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open " + utf8String(path));
        return file;
    }

    std::vector<unsigned char> readAt(std::ifstream &file, uint64_t offset, size_t size)
    {
        std::vector<unsigned char> data(size);
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
        if (static_cast<size_t>(file.gcount()) != size)
            corrupt("unexpected end of file");
        return data;
    }
} // namespace

ZipArchive::ZipArchive(const std::filesystem::path &path) : path_(path)
{
    std::ifstream file = openFile(path);
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    if (end < 0)
        corrupt("cannot tell its size");
    const auto file_size = static_cast<uint64_t>(end);
    if (file_size < kEndRecordSize)
        corrupt("too short");

    // The end record is followed by a comment of up to 64 KiB.
    const size_t tail_size = static_cast<size_t>(std::min<uint64_t>(file_size, kEndRecordSize + kMaxCommentSize));
    const uint64_t tail_offset = file_size - tail_size;
    const auto tail = readAt(file, tail_offset, tail_size);
    size_t end_pos = std::numeric_limits<size_t>::max();
    for (size_t pos = tail_size - kEndRecordSize + 1; pos-- > 0;)
    {
        if (le32(&tail[pos]) == kEndOfCentralDirectory && pos + kEndRecordSize + le16(&tail[pos + 20]) <= tail_size)
        {
            end_pos = pos;
            break;
        }
    }
    if (end_pos == std::numeric_limits<size_t>::max())
        corrupt("no end of central directory");

    const unsigned char *record = &tail[end_pos];
    uint64_t entry_count = le16(record + 10);
    uint64_t directory_size = le32(record + 12);
    uint64_t directory_offset = le32(record + 16);
    if (le16(record + 4) != 0 || le16(record + 6) != 0)
        corrupt("multi-volume archives are not supported");

    const uint64_t end_offset = tail_offset + end_pos;
    if (end_offset >= 20)
    {
        const auto locator = readAt(file, end_offset - 20, 20);
        if (le32(locator.data()) == kZip64EndLocator)
        {
            const uint64_t zip64_offset = le64(&locator[8]);
            if (zip64_offset > file_size - 56)
                corrupt("bad ZIP64 end record offset");
            const auto zip64 = readAt(file, zip64_offset, 56);
            if (le32(zip64.data()) != kZip64End)
                corrupt("no ZIP64 end record");
            entry_count = le64(&zip64[32]);
            directory_size = le64(&zip64[40]);
            directory_offset = le64(&zip64[48]);
        }
    }

    if (directory_size > kMaxCentralDirectorySize || directory_offset > file_size ||
        directory_size > file_size - directory_offset)
        corrupt("bad central directory");
    const auto directory = readAt(file, directory_offset, static_cast<size_t>(directory_size));

    size_t pos = 0;
    entries_.reserve(static_cast<size_t>(std::min<uint64_t>(entry_count, directory_size / 46)));
    for (uint64_t i = 0; i < entry_count; ++i)
    {
        if (pos + 46 > directory.size() || le32(&directory[pos]) != kCentralHeader)
            corrupt("bad central directory entry");
        const unsigned char *header = &directory[pos];
        Entry entry;
        entry.flags = le16(header + 8);
        entry.method = le16(header + 10);
        entry.crc = le32(header + 16);
        entry.compressed_size = le32(header + 20);
        entry.size = le32(header + 24);
        const size_t name_size = le16(header + 28);
        const size_t extra_size = le16(header + 30);
        const size_t comment_size = le16(header + 32);
        entry.local_header_offset = le32(header + 42);
        if (pos + 46 + name_size + extra_size + comment_size > directory.size())
            corrupt("bad central directory entry");
        entry.name.assign(reinterpret_cast<const char *>(header + 46), name_size);

        // ZIP64: the fields saturated in the header follow in this order.
        const unsigned char *extra = header + 46 + name_size;
        for (size_t at = 0; at + 4 <= extra_size;)
        {
            const uint16_t id = le16(extra + at);
            const size_t size = le16(extra + at + 2);
            if (at + 4 + size > extra_size)
                break;
            if (id == 0x0001)
            {
                const unsigned char *field = extra + at + 4;
                const unsigned char *field_end = field + size;
                auto take = [&](uint64_t &value)
                {
                    if (value != 0xFFFFFFFFu)
                        return;
                    if (field + 8 > field_end)
                        corrupt("bad ZIP64 extra field");
                    value = le64(field);
                    field += 8;
                };
                take(entry.size);
                take(entry.compressed_size);
                take(entry.local_header_offset);
            }
            at += 4 + size;
        }
        pos += 46 + name_size + extra_size + comment_size;
        entries_.push_back(std::move(entry));
    }
}

const ZipArchive::Entry *ZipArchive::find(std::string_view name) const
{
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry)
                                 { return entry.name == name; });
    return it == entries_.end() ? nullptr : &*it;
}

class ZipEntryReader::Impl
{
public:
    Impl(const ZipArchive &archive, const ZipArchive::Entry &entry)
        : file_(openFile(archive.path())), name_(entry.name), method_(entry.method), remaining_in_(entry.compressed_size),
          expected_size_(entry.size), expected_crc_(entry.crc)
    {
        if (entry.flags & 0x0001)
            throw std::runtime_error("Encrypted ZIP entries are not supported: " + name_);
        if (method_ != 0 && method_ != 8)
            throw std::runtime_error("Unsupported ZIP compression method " + std::to_string(method_) + ": " + name_);
        const auto header = readAt(file_, entry.local_header_offset, 30);
        if (le32(header.data()) != kLocalHeader)
            corrupt("bad local header of " + name_);
        const uint64_t data_offset = entry.local_header_offset + 30 + le16(&header[26]) + le16(&header[28]);
        file_.seekg(static_cast<std::streamoff>(data_offset));
        if (method_ == 8)
        {
            if (inflateInit2(&z_, -MAX_WBITS) != Z_OK)
                throw std::runtime_error("Cannot start decompressing " + name_);
            inflating_ = true;
            input_.resize(kInputBufferSize);
        }
    }

    ~Impl()
    {
        if (inflating_)
            inflateEnd(&z_);
    }

    size_t read(char *out, size_t size)
    {
        if (finished_ || size == 0)
            return 0;
        size = std::min<size_t>(size, std::numeric_limits<uInt>::max());
        size_t got = 0;
        bool stream_end = false;
        if (method_ == 0)
        {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(size, remaining_in_));
            file_.read(out, static_cast<std::streamsize>(want));
            if (static_cast<size_t>(file_.gcount()) != want)
                corrupt("unexpected end of " + name_);
            remaining_in_ -= want;
            got = want;
            stream_end = remaining_in_ == 0;
        }
        else
        {
            z_.next_out = reinterpret_cast<Bytef *>(out);
            z_.avail_out = static_cast<uInt>(size);
            while (z_.avail_out == size)
            {
                if (z_.avail_in == 0 && remaining_in_ > 0)
                {
                    const size_t want = static_cast<size_t>(std::min<uint64_t>(input_.size(), remaining_in_));
                    file_.read(input_.data(), static_cast<std::streamsize>(want));
                    if (static_cast<size_t>(file_.gcount()) != want)
                        corrupt("unexpected end of " + name_);
                    remaining_in_ -= want;
                    z_.next_in = reinterpret_cast<Bytef *>(input_.data());
                    z_.avail_in = static_cast<uInt>(want);
                }
                const int result = inflate(&z_, Z_NO_FLUSH);
                if (result == Z_STREAM_END)
                {
                    stream_end = true;
                    break;
                }
                if (result == Z_BUF_ERROR && z_.avail_in == 0 && remaining_in_ == 0)
                    corrupt("truncated data in " + name_);
                if (result != Z_OK && result != Z_BUF_ERROR)
                    corrupt("bad compressed data in " + name_);
            }
            got = size - z_.avail_out;
        }

        produced_ += got;
        if (produced_ > expected_size_)
            corrupt(name_ + " is larger than its directory entry says");
        crc_ = crc32(crc_, reinterpret_cast<const Bytef *>(out), static_cast<uInt>(got));
        if (stream_end)
        {
            finished_ = true;
            if (produced_ != expected_size_)
                corrupt(name_ + " is smaller than its directory entry says");
            if (crc_ != expected_crc_)
                corrupt("bad CRC of " + name_);
        }
        return got;
    }

private:
    std::ifstream file_;
    std::string name_;
    uint16_t method_;
    uint64_t remaining_in_;
    uint64_t expected_size_;
    uint32_t expected_crc_;
    uint64_t produced_ = 0;
    uLong crc_ = crc32(0, nullptr, 0);
    z_stream z_{};
    bool inflating_ = false;
    bool finished_ = false;
    std::vector<char> input_;
};

ZipEntryReader::ZipEntryReader(const ZipArchive &archive, const ZipArchive::Entry &entry)
    : impl_(std::make_unique<Impl>(archive, entry))
{
}

ZipEntryReader::~ZipEntryReader() = default;

size_t ZipEntryReader::read(char *out, size_t size)
{
    return impl_->read(out, size);
}

std::string ZipEntryReader::readAll(size_t limit)
{
    std::string data;
    std::array<char, 64 * 1024> buffer;
    while (const size_t got = read(buffer.data(), buffer.size()))
    {
        if (data.size() + got > limit)
            throw std::runtime_error("ZIP entry is too large");
        data.append(buffer.data(), got);
    }
    return data;
}
