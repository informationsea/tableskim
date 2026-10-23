#include "compress.hxx"

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
// bzlib.h includes <windows.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

namespace
{
    constexpr size_t OUT_CHUNK = 64 * 1024;

    // The file itself; compressing writers put their output here.
    class FileOutput
    {
    private:
        std::ofstream file_;

    public:
        explicit FileOutput(const std::filesystem::path &path) : file_(path, std::ios::binary | std::ios::trunc)
        {
            if (!file_)
                throw std::runtime_error("Cannot create file: " + path.string());
        }
        void write(const char *data, size_t size)
        {
            file_.write(data, static_cast<std::streamsize>(size));
            if (!file_)
                throw std::runtime_error("Cannot write file");
        }
        void close()
        {
            file_.close();
            if (file_.fail())
                throw std::runtime_error("Cannot write file");
        }
    };

    class PlainWriter : public CompressedWriter
    {
    private:
        FileOutput file_;

    public:
        explicit PlainWriter(const std::filesystem::path &path) : file_(path) {}
        void write(std::string_view data) override { file_.write(data.data(), data.size()); }
        void finish() override { file_.close(); }
    };

    class GzipWriter : public CompressedWriter
    {
    private:
        FileOutput file_;
        z_stream stream_{};
        std::vector<char> out_ = std::vector<char>(OUT_CHUNK);

        void deflateAll(const char *data, size_t size, int flush)
        {
            stream_.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
            stream_.avail_in = static_cast<uInt>(size);
            int ret;
            do
            {
                stream_.next_out = reinterpret_cast<Bytef *>(out_.data());
                stream_.avail_out = static_cast<uInt>(out_.size());
                ret = deflate(&stream_, flush);
                if (ret == Z_STREAM_ERROR)
                    throw std::runtime_error("Cannot compress gzip data");
                file_.write(out_.data(), out_.size() - stream_.avail_out);
            } while (stream_.avail_out == 0 || (flush == Z_FINISH && ret != Z_STREAM_END));
        }

    public:
        explicit GzipWriter(const std::filesystem::path &path) : file_(path)
        {
            // 15 + 16: maximum window, gzip header.
            if (deflateInit2(&stream_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                throw std::runtime_error("Cannot initialize gzip encoder");
        }
        ~GzipWriter() override { deflateEnd(&stream_); }
        void write(std::string_view data) override
        {
            // avail_in is 32 bits.
            for (size_t done = 0; done < data.size(); done += OUT_CHUNK)
                deflateAll(data.data() + done, std::min(OUT_CHUNK, data.size() - done), Z_NO_FLUSH);
        }
        void finish() override
        {
            deflateAll(nullptr, 0, Z_FINISH);
            file_.close();
        }
    };

    // BGZF: gzip members of at most 64 KiB, each with its size in a "BC"
    // extra field, and an empty member at the end.
    class BgzfWriter : public CompressedWriter
    {
    private:
        // As bgzip: the compressed block, with header and footer, always fits in 64 KiB.
        static constexpr size_t BLOCK_INPUT = 0xff00;
        FileOutput file_;
        std::string pending_;

        void putLe(std::string &out, uint32_t value, int bytes)
        {
            for (int i = 0; i < bytes; ++i)
                out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
        }

        void writeBlock(std::string_view data)
        {
            z_stream stream{};
            // Raw deflate: the header and footer are written here.
            if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                throw std::runtime_error("Cannot initialize gzip encoder");
            std::vector<char> deflated(deflateBound(&stream, static_cast<uLong>(data.size())));
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
            stream.avail_in = static_cast<uInt>(data.size());
            stream.next_out = reinterpret_cast<Bytef *>(deflated.data());
            stream.avail_out = static_cast<uInt>(deflated.size());
            const int ret = deflate(&stream, Z_FINISH);
            const size_t size = deflated.size() - stream.avail_out;
            deflateEnd(&stream);
            if (ret != Z_STREAM_END)
                throw std::runtime_error("Cannot compress gzip data");

            std::string block("\x1f\x8b\x08\x04\0\0\0\0\0\xff\x06\0BC\x02\0", 16);
            putLe(block, static_cast<uint32_t>(18 + size + 8 - 1), 2);
            block.append(deflated.data(), size);
            const uLong crc = crc32(crc32(0, nullptr, 0), reinterpret_cast<const Bytef *>(data.data()),
                                    static_cast<uInt>(data.size()));
            putLe(block, static_cast<uint32_t>(crc), 4);
            putLe(block, static_cast<uint32_t>(data.size()), 4);
            file_.write(block.data(), block.size());
        }

    public:
        explicit BgzfWriter(const std::filesystem::path &path) : file_(path) {}
        void write(std::string_view data) override
        {
            pending_.append(data);
            size_t done = 0;
            while (pending_.size() - done >= BLOCK_INPUT)
            {
                writeBlock(std::string_view(pending_).substr(done, BLOCK_INPUT));
                done += BLOCK_INPUT;
            }
            pending_.erase(0, done);
        }
        void finish() override
        {
            if (!pending_.empty())
                writeBlock(pending_);
            static constexpr std::array<unsigned char, 28> END_OF_FILE = {
                0x1f, 0x8b, 0x08, 0x04, 0, 0, 0, 0, 0, 0xff, 0x06, 0, 0x42, 0x43,
                0x02, 0, 0x1b, 0, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            file_.write(reinterpret_cast<const char *>(END_OF_FILE.data()), END_OF_FILE.size());
            file_.close();
        }
    };

    class Bzip2Writer : public CompressedWriter
    {
    private:
        FileOutput file_;
        bz_stream stream_{};
        std::vector<char> out_ = std::vector<char>(OUT_CHUNK);

        void compressAll(const char *data, size_t size, int action)
        {
            stream_.next_in = const_cast<char *>(data);
            stream_.avail_in = static_cast<unsigned int>(size);
            int ret;
            do
            {
                stream_.next_out = out_.data();
                stream_.avail_out = static_cast<unsigned int>(out_.size());
                ret = BZ2_bzCompress(&stream_, action);
                if (ret < 0)
                    throw std::runtime_error("Cannot compress bzip2 data (error " + std::to_string(ret) + ")");
                file_.write(out_.data(), out_.size() - stream_.avail_out);
            } while (action == BZ_FINISH ? ret != BZ_STREAM_END : stream_.avail_in > 0);
        }

    public:
        explicit Bzip2Writer(const std::filesystem::path &path) : file_(path)
        {
            if (BZ2_bzCompressInit(&stream_, 9, 0, 0) != BZ_OK)
                throw std::runtime_error("Cannot initialize bzip2 encoder");
        }
        ~Bzip2Writer() override { BZ2_bzCompressEnd(&stream_); }
        void write(std::string_view data) override
        {
            for (size_t done = 0; done < data.size(); done += OUT_CHUNK)
                compressAll(data.data() + done, std::min(OUT_CHUNK, data.size() - done), BZ_RUN);
        }
        void finish() override
        {
            compressAll(nullptr, 0, BZ_FINISH);
            file_.close();
        }
    };

    class XzWriter : public CompressedWriter
    {
    private:
        FileOutput file_;
        lzma_stream stream_ = LZMA_STREAM_INIT;
        std::vector<char> out_ = std::vector<char>(OUT_CHUNK);

        void codeAll(const char *data, size_t size, lzma_action action)
        {
            stream_.next_in = reinterpret_cast<const uint8_t *>(data);
            stream_.avail_in = size;
            lzma_ret ret;
            do
            {
                stream_.next_out = reinterpret_cast<uint8_t *>(out_.data());
                stream_.avail_out = out_.size();
                ret = lzma_code(&stream_, action);
                if (ret != LZMA_OK && ret != LZMA_STREAM_END)
                    throw std::runtime_error("Cannot compress xz data (error " + std::to_string(ret) + ")");
                file_.write(out_.data(), out_.size() - stream_.avail_out);
            } while (action == LZMA_FINISH ? ret != LZMA_STREAM_END : stream_.avail_in > 0);
        }

    public:
        explicit XzWriter(const std::filesystem::path &path) : file_(path)
        {
            if (lzma_easy_encoder(&stream_, 6, LZMA_CHECK_CRC64) != LZMA_OK)
                throw std::runtime_error("Cannot initialize xz encoder");
        }
        ~XzWriter() override { lzma_end(&stream_); }
        void write(std::string_view data) override { codeAll(data.data(), data.size(), LZMA_RUN); }
        void finish() override
        {
            codeAll(nullptr, 0, LZMA_FINISH);
            file_.close();
        }
    };

    class ZstdWriter : public CompressedWriter
    {
    private:
        FileOutput file_;
        ZSTD_CCtx *context_ = nullptr;
        std::vector<char> out_ = std::vector<char>(ZSTD_CStreamOutSize());

        void compressAll(const char *data, size_t size, ZSTD_EndDirective mode)
        {
            ZSTD_inBuffer in{data, size, 0};
            size_t remaining;
            do
            {
                ZSTD_outBuffer out{out_.data(), out_.size(), 0};
                remaining = ZSTD_compressStream2(context_, &out, &in, mode);
                if (ZSTD_isError(remaining))
                    throw std::runtime_error(std::string("Cannot compress zstd data: ") + ZSTD_getErrorName(remaining));
                file_.write(out_.data(), out.pos);
            } while (mode == ZSTD_e_end ? remaining != 0 : in.pos < in.size);
        }

    public:
        explicit ZstdWriter(const std::filesystem::path &path) : file_(path), context_(ZSTD_createCCtx())
        {
            // As the zstd tool: a checksum of the contents.
            if (!context_ || ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_checksumFlag, 1)))
                throw std::runtime_error("Cannot initialize zstd encoder");
        }
        ~ZstdWriter() override { ZSTD_freeCCtx(context_); }
        void write(std::string_view data) override { compressAll(data.data(), data.size(), ZSTD_e_continue); }
        void finish() override
        {
            compressAll(nullptr, 0, ZSTD_e_end);
            file_.close();
        }
    };
} // namespace

bool isBgzf(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 16> head{};
    file.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
    if (file.gcount() < static_cast<std::streamsize>(head.size()))
        return false;
    // gzip, deflate, FEXTRA; the first subfield of the extra field is "BC".
    return head[0] == 0x1f && head[1] == 0x8b && head[2] == 0x08 && (head[3] & 0x04) != 0 && head[12] == 'B' &&
           head[13] == 'C';
}

std::unique_ptr<CompressedWriter> openCompressedWriter(const std::filesystem::path &path, Compression compression,
                                                       bool bgzf)
{
    switch (compression)
    {
    case Compression::None:
        return std::make_unique<PlainWriter>(path);
    case Compression::Gzip:
        if (bgzf)
            return std::make_unique<BgzfWriter>(path);
        return std::make_unique<GzipWriter>(path);
    case Compression::Bzip2:
        return std::make_unique<Bzip2Writer>(path);
    case Compression::Xz:
        return std::make_unique<XzWriter>(path);
    case Compression::Zstd:
        return std::make_unique<ZstdWriter>(path);
    }
    throw std::runtime_error("Unknown compression");
}
