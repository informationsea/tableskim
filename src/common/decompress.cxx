#include "decompress.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <streambuf>
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
    // Turns a compressed byte stream into plain bytes. A file may hold several
    // concatenated streams (e.g. `cat a.gz b.gz`), so reset() starts the next one.
    class Decoder
    {
    public:
        virtual ~Decoder() = default;
        // Advances `in` and `out` by the bytes consumed and produced; throws on corrupt data.
        virtual void decode(const char *&in, const char *in_end, char *&out, char *out_end, bool input_eof) = 0;
        // True when the last decode() finished a complete stream.
        virtual bool ended() const = 0;
        virtual void reset() = 0;
    };

    class GzipDecoder : public Decoder
    {
    private:
        z_stream stream_{};
        bool ended_ = false;

    public:
        GzipDecoder()
        {
            // 15 + 16: maximum window, gzip header only.
            if (inflateInit2(&stream_, 15 + 16) != Z_OK)
                throw std::runtime_error("Cannot initialize gzip decoder");
        }
        ~GzipDecoder() override { inflateEnd(&stream_); }

        void decode(const char *&in, const char *in_end, char *&out, char *out_end, bool) override
        {
            stream_.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in));
            stream_.avail_in = static_cast<uInt>(in_end - in);
            stream_.next_out = reinterpret_cast<Bytef *>(out);
            stream_.avail_out = static_cast<uInt>(out_end - out);
            const int ret = inflate(&stream_, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
                throw std::runtime_error(std::string("Corrupt gzip data: ") + (stream_.msg ? stream_.msg : zError(ret)));
            ended_ = ret == Z_STREAM_END;
            in = reinterpret_cast<const char *>(stream_.next_in);
            out = reinterpret_cast<char *>(stream_.next_out);
        }
        bool ended() const override { return ended_; }
        void reset() override
        {
            inflateReset(&stream_);
            ended_ = false;
        }
    };

    class Bzip2Decoder : public Decoder
    {
    private:
        bz_stream stream_{};
        bool ended_ = false;

        void init()
        {
            stream_ = {};
            if (BZ2_bzDecompressInit(&stream_, 0, 0) != BZ_OK)
                throw std::runtime_error("Cannot initialize bzip2 decoder");
        }

    public:
        Bzip2Decoder() { init(); }
        ~Bzip2Decoder() override { BZ2_bzDecompressEnd(&stream_); }

        void decode(const char *&in, const char *in_end, char *&out, char *out_end, bool) override
        {
            stream_.next_in = const_cast<char *>(in);
            stream_.avail_in = static_cast<unsigned int>(in_end - in);
            stream_.next_out = out;
            stream_.avail_out = static_cast<unsigned int>(out_end - out);
            const int ret = BZ2_bzDecompress(&stream_);
            if (ret != BZ_OK && ret != BZ_STREAM_END)
                throw std::runtime_error("Corrupt bzip2 data (error " + std::to_string(ret) + ")");
            ended_ = ret == BZ_STREAM_END;
            in = stream_.next_in;
            out = stream_.next_out;
        }
        bool ended() const override { return ended_; }
        void reset() override
        {
            BZ2_bzDecompressEnd(&stream_);
            init();
            ended_ = false;
        }
    };

    class XzDecoder : public Decoder
    {
    private:
        // This comfortably covers liblzma's standard presets (up to a 64 MiB
        // dictionary) while preventing a crafted header from exhausting memory.
        static constexpr uint64_t kMemoryLimit = 256ULL * 1024 * 1024;

        lzma_stream stream_ = LZMA_STREAM_INIT;
        bool ended_ = false;

        void init()
        {
            // LZMA_CONCATENATED handles multi-stream files (as `xz -dc` does);
            // it reports the end only after LZMA_FINISH.
            if (lzma_stream_decoder(&stream_, kMemoryLimit, LZMA_CONCATENATED) != LZMA_OK)
                throw std::runtime_error("Cannot initialize xz decoder");
        }

    public:
        XzDecoder() { init(); }
        ~XzDecoder() override { lzma_end(&stream_); }

        void decode(const char *&in, const char *in_end, char *&out, char *out_end, bool input_eof) override
        {
            stream_.next_in = reinterpret_cast<const uint8_t *>(in);
            stream_.avail_in = static_cast<size_t>(in_end - in);
            stream_.next_out = reinterpret_cast<uint8_t *>(out);
            stream_.avail_out = static_cast<size_t>(out_end - out);
            const lzma_ret ret = lzma_code(&stream_, input_eof ? LZMA_FINISH : LZMA_RUN);
            if (ret != LZMA_OK && ret != LZMA_STREAM_END)
            {
                if (ret == LZMA_MEMLIMIT_ERROR)
                    throw std::runtime_error("XZ data exceeds the 256 MiB memory limit");
                if (ret == LZMA_BUF_ERROR)
                    throw std::runtime_error("Truncated xz data");
                throw std::runtime_error("Corrupt xz data (error " + std::to_string(ret) + ")");
            }
            ended_ = ret == LZMA_STREAM_END;
            in = reinterpret_cast<const char *>(stream_.next_in);
            out = reinterpret_cast<char *>(stream_.next_out);
        }
        bool ended() const override { return ended_; }
        void reset() override
        {
            init();
            ended_ = false;
        }
    };

    class ZstdDecoder : public Decoder
    {
    private:
        ZSTD_DStream *stream_;
        bool ended_ = false;

    public:
        ZstdDecoder() : stream_(ZSTD_createDStream())
        {
            if (!stream_)
                throw std::runtime_error("Cannot initialize zstd decoder");
        }
        ~ZstdDecoder() override { ZSTD_freeDStream(stream_); }

        void decode(const char *&in, const char *in_end, char *&out, char *out_end, bool) override
        {
            ZSTD_inBuffer input{in, static_cast<size_t>(in_end - in), 0};
            ZSTD_outBuffer output{out, static_cast<size_t>(out_end - out), 0};
            const size_t ret = ZSTD_decompressStream(stream_, &output, &input);
            if (ZSTD_isError(ret))
                throw std::runtime_error(std::string("Corrupt zstd data: ") + ZSTD_getErrorName(ret));
            // 0: a frame is complete and fully flushed. Following frames are decoded
            // by the same context, so reset() has nothing to do.
            ended_ = ret == 0;
            in += input.pos;
            out += output.pos;
        }
        bool ended() const override { return ended_; }
        void reset() override { ended_ = false; }
    };

    std::unique_ptr<Decoder> makeDecoder(Compression compression)
    {
        switch (compression)
        {
        case Compression::Gzip:
            return std::make_unique<GzipDecoder>();
        case Compression::Bzip2:
            return std::make_unique<Bzip2Decoder>();
        case Compression::Xz:
            return std::make_unique<XzDecoder>();
        case Compression::Zstd:
            return std::make_unique<ZstdDecoder>();
        case Compression::None:
            break;
        }
        throw std::invalid_argument("No decoder for uncompressed data");
    }

    class DecompressingBuffer : public std::streambuf
    {
    private:
        static constexpr size_t kBufferSize = 256 * 1024;

        // The file stream's own buffer; declared before the stream, which
        // uses it until it is destroyed.
        std::vector<char> file_buffer_ = std::vector<char>(kBufferSize);
        std::ifstream file_;
        std::unique_ptr<Decoder> decoder_;
        std::vector<char> in_ = std::vector<char>(kBufferSize);
        std::vector<char> out_ = std::vector<char>(kBufferSize);
        const char *in_pos_ = nullptr;
        const char *in_end_ = nullptr;
        bool input_eof_ = false;

        // Keeps the unconsumed input and appends more from the file.
        void refill()
        {
            const size_t kept = in_pos_ ? static_cast<size_t>(in_end_ - in_pos_) : 0;
            if (kept > 0)
                std::memmove(in_.data(), in_pos_, kept);
            file_.read(in_.data() + kept, static_cast<std::streamsize>(in_.size() - kept));
            const size_t n = static_cast<size_t>(file_.gcount());
            if (file_.bad())
                throw std::runtime_error("Cannot read compressed file");
            if (n == 0)
                input_eof_ = true;
            in_pos_ = in_.data();
            in_end_ = in_.data() + kept + n;
        }

    protected:
        int_type underflow() override
        {
            if (gptr() < egptr())
                return traits_type::to_int_type(*gptr());
            char *out = out_.data();
            char *const out_end = out_.data() + out_.size();
            while (out == out_.data())
            {
                if (in_pos_ == in_end_ && !input_eof_)
                    refill();
                if (in_pos_ == in_end_ && input_eof_ && decoder_->ended())
                    return traits_type::eof();
                if (decoder_->ended())
                {
                    // Zero bytes after a stream are padding (tape blocks,
                    // xz stream padding), which gzip -d skips too; no format
                    // starts with one.
                    while (in_pos_ < in_end_ && *in_pos_ == '\0')
                        ++in_pos_;
                    if (in_pos_ == in_end_)
                        continue; // more input, or the end
                    decoder_->reset(); // another stream follows
                }
                const char *const before = in_pos_;
                decoder_->decode(in_pos_, in_end_, out, out_end, input_eof_);
                if (out == out_.data() && in_pos_ == before)
                {
                    if (input_eof_)
                    {
                        if (decoder_->ended())
                            return traits_type::eof();
                        throw std::runtime_error("Truncated compressed data");
                    }
                    refill();
                }
            }
            setg(out_.data(), out_.data(), out);
            return traits_type::to_int_type(*gptr());
        }

    public:
        DecompressingBuffer(const std::filesystem::path &path, Compression compression)
            : decoder_(makeDecoder(compression))
        {
            // Read the file in large pieces: with the default buffer (4 KiB
            // in libc++), refill() reads it in many small ones, each of which
            // costs much more in a browser (WebAssembly reading a File).
            file_.rdbuf()->pubsetbuf(file_buffer_.data(), static_cast<std::streamsize>(file_buffer_.size()));
            file_.open(path, std::ios::binary);
            if (!file_)
                throw std::runtime_error("Cannot open file: " + path.string());
            setg(out_.data(), out_.data(), out_.data());
        }
    };

    class DecompressingStream : public std::istream
    {
    private:
        DecompressingBuffer buffer_;

    public:
        DecompressingStream(const std::filesystem::path &path, Compression compression)
            : std::istream(nullptr), buffer_(path, compression)
        {
            rdbuf(&buffer_);
            // Let decoding errors propagate instead of looking like the end of file.
            exceptions(std::ios::badbit);
        }
    };

    bool startsWith(std::span<const std::byte> head, std::initializer_list<unsigned char> magic)
    {
        return head.size() >= magic.size() &&
               std::equal(magic.begin(), magic.end(), head.begin(), [](unsigned char m, std::byte b)
                          { return std::byte{m} == b; });
    }
} // namespace

Compression detectCompression(std::span<const std::byte> head)
{
    if (startsWith(head, {0x1f, 0x8b}))
        return Compression::Gzip;
    if (startsWith(head, {'B', 'Z', 'h'}) && head.size() >= 4 &&
        head[3] >= std::byte{'1'} && head[3] <= std::byte{'9'})
        return Compression::Bzip2;
    if (startsWith(head, {0xfd, '7', 'z', 'X', 'Z', 0x00}))
        return Compression::Xz;
    if (startsWith(head, {0x28, 0xb5, 0x2f, 0xfd}))
        return Compression::Zstd;
    return Compression::None;
}

Compression detectCompression(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    std::array<std::byte, 8> head{};
    file.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
    return detectCompression(std::span<const std::byte>(head.data(), static_cast<size_t>(file.gcount())));
}

Compression compressionFromExtension(const std::filesystem::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    if (ext == ".gz" || ext == ".gzip" || ext == ".bgz")
        return Compression::Gzip;
    if (ext == ".bz2")
        return Compression::Bzip2;
    if (ext == ".xz")
        return Compression::Xz;
    if (ext == ".zst" || ext == ".zstd")
        return Compression::Zstd;
    return Compression::None;
}

std::filesystem::path stripCompressionExtension(const std::filesystem::path &path)
{
    if (compressionFromExtension(path) == Compression::None)
        return path;
    return std::filesystem::path(path).replace_extension();
}

std::unique_ptr<std::istream> openDecompressingStream(const std::filesystem::path &path, Compression compression)
{
    return std::make_unique<DecompressingStream>(path, compression);
}
