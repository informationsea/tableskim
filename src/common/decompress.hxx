#ifndef _DECOMPRESS_HXX_
#define _DECOMPRESS_HXX_

#include <cstddef>
#include <filesystem>
#include <istream>
#include <memory>
#include <span>

// Transparent decompression for text formats (gzip, bzip2, xz, zstd).
enum class Compression
{
    None,
    Gzip,
    Bzip2,
    Xz,
    Zstd,
};

// Detects the format from the magic number at the start of a file.
Compression detectCompression(std::span<const std::byte> head);
Compression detectCompression(const std::filesystem::path &path);

// Guesses the format from the last extension (.gz, .bgz, .bz2, .xz, .zst).
Compression compressionFromExtension(const std::filesystem::path &path);

// "data.csv.gz" -> "data.csv"; paths without a compression extension are returned as is.
std::filesystem::path stripCompressionExtension(const std::filesystem::path &path);

// Opens a stream that yields the decompressed contents. Corrupt or truncated
// data makes reads throw std::runtime_error (badbit is in exceptions()).
// Throws std::runtime_error if the file cannot be opened.
std::unique_ptr<std::istream> openDecompressingStream(const std::filesystem::path &path, Compression compression);

#endif // _DECOMPRESS_HXX_
