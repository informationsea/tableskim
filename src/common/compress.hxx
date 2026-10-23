#ifndef _COMPRESS_HXX_
#define _COMPRESS_HXX_

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

#include "decompress.hxx"

// Writing text formats back compressed as they were read (see decompress.hxx).

// Whether a gzip file is BGZF (bgzip, as tabix indexes need): its first
// member has the "BC" extra field.
bool isBgzf(const std::filesystem::path &path);

// Writes a new file, compressed with `compression` (gzip in BGZF blocks if
// `bgzf`) at the tools' default levels. Throws std::runtime_error on errors.
class CompressedWriter
{
public:
    virtual ~CompressedWriter() = default;
    virtual void write(std::string_view data) = 0;
    // Ends the compressed data and closes the file; the file is incomplete
    // without it.
    virtual void finish() = 0;
};

std::unique_ptr<CompressedWriter> openCompressedWriter(const std::filesystem::path &path, Compression compression,
                                                       bool bgzf = false);

#endif // _COMPRESS_HXX_
