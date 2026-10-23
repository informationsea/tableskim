#include "text_encoding.hxx"

#include <algorithm>
#include <cctype>
#include <streambuf>
#include <vector>

#include "cp932_table.hxx"

namespace
{
    constexpr char32_t REPLACEMENT_CHARACTER = 0xFFFD;

    void appendUtf8(std::string &out, char32_t cp)
    {
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // Length of the UTF-8 sequence at data[i], or 0 if it is invalid. One cut
    // off at the end of `data` is valid (its length is what is left) if
    // `allow_cut`.
    size_t utf8SequenceLength(const unsigned char *data, size_t size, size_t i, bool allow_cut)
    {
        const unsigned char b = data[i];
        if (b < 0x80)
            return 1;
        size_t length;
        // The allowed range of the second byte excludes overlong forms,
        // surrogates and code points above U+10FFFF.
        unsigned char second_min = 0x80, second_max = 0xBF;
        if (b >= 0xC2 && b <= 0xDF)
            length = 2;
        else if (b >= 0xE0 && b <= 0xEF)
        {
            length = 3;
            if (b == 0xE0)
                second_min = 0xA0;
            else if (b == 0xED)
                second_max = 0x9F;
        }
        else if (b >= 0xF0 && b <= 0xF4)
        {
            length = 4;
            if (b == 0xF0)
                second_min = 0x90;
            else if (b == 0xF4)
                second_max = 0x8F;
        }
        else
            return 0;
        for (size_t k = 1; k < length; ++k)
        {
            if (i + k >= size)
                return allow_cut ? size - i : 0;
            const unsigned char c = data[i + k];
            if (k == 1 ? (c < second_min || c > second_max) : (c & 0xC0) != 0x80)
                return 0;
        }
        return length;
    }

    // Whether `data` is UTF-8, allowing a sequence cut off at the end.
    bool isValidUtf8(const unsigned char *data, size_t size)
    {
        for (size_t i = 0; i < size;)
        {
            const size_t length = utf8SequenceLength(data, size, i, true);
            if (length == 0)
                return false;
            i += length;
        }
        return true;
    }

    class DecodingBuf : public std::streambuf
    {
    private:
        static constexpr size_t CHUNK_SIZE = 64 * 1024;
        std::unique_ptr<std::istream> source_;
        TextDecoder decoder_;
        std::vector<char> in_;
        std::string out_;
        bool eof_ = false;

    protected:
        int_type underflow() override
        {
            if (gptr() < egptr())
                return traits_type::to_int_type(*gptr());
            out_.clear();
            while (out_.empty() && !eof_)
            {
                const std::streamsize n = source_->rdbuf()->sgetn(in_.data(), static_cast<std::streamsize>(in_.size()));
                if (n <= 0)
                {
                    decoder_.finish(out_);
                    eof_ = true;
                    break;
                }
                decoder_.decode(std::span<const char>(in_.data(), static_cast<size_t>(n)), out_);
            }
            if (out_.empty())
                return traits_type::eof();
            setg(out_.data(), out_.data(), out_.data() + out_.size());
            return traits_type::to_int_type(*gptr());
        }

    public:
        DecodingBuf(std::unique_ptr<std::istream> source, TextEncoding encoding)
            : source_(std::move(source)), decoder_(encoding), in_(CHUNK_SIZE)
        {
            // UTF-16 doubles in size at most when converted (ASCII halves).
            out_.reserve(CHUNK_SIZE * 2);
        }
    };

    class DecodingStream : public std::istream
    {
    private:
        DecodingBuf buf_;

    public:
        DecodingStream(std::unique_ptr<std::istream> source, TextEncoding encoding)
            : std::istream(nullptr), buf_(std::move(source), encoding)
        {
            rdbuf(&buf_);
            // Like the decompressing stream: read errors must not look like the end of the file.
            exceptions(std::ios::badbit);
        }
    };
} // namespace

const char *textEncodingName(TextEncoding encoding)
{
    switch (encoding)
    {
    case TextEncoding::Utf8:
        return "UTF-8";
    case TextEncoding::Utf16LE:
        return "UTF-16LE";
    case TextEncoding::Utf16BE:
        return "UTF-16BE";
    case TextEncoding::Cp932:
        return "Shift_JIS (CP932)";
    }
    return "?";
}

std::optional<TextEncoding> parseTextEncoding(std::string_view name)
{
    std::string key;
    for (const char c : name)
    {
        if (c != '-' && c != '_' && c != ' ')
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (key == "utf8")
        return TextEncoding::Utf8;
    if (key == "utf16le")
        return TextEncoding::Utf16LE;
    if (key == "utf16be")
        return TextEncoding::Utf16BE;
    for (const char *alias : {"cp932", "ms932", "windows31j", "shiftjis", "sjis", "mskanji", "shiftjis(cp932)"})
    {
        if (key == alias)
            return TextEncoding::Cp932;
    }
    return std::nullopt;
}

TextEncoding detectTextEncoding(std::span<const std::byte> head)
{
    const auto *data = reinterpret_cast<const unsigned char *>(head.data());
    const size_t size = head.size();
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        return TextEncoding::Utf8;
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        return TextEncoding::Utf16LE;
    if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF)
        return TextEncoding::Utf16BE;

    // Delimiters, line breaks and digits are ASCII, so UTF-16 tables have a
    // NUL in every other byte. Other text has hardly any NUL bytes; CJK
    // characters in UTF-16 may have one on either side.
    const size_t pairs = size / 2;
    size_t even_zeros = 0, odd_zeros = 0;
    for (size_t i = 0; i + 1 < size; i += 2)
    {
        even_zeros += data[i] == 0;
        odd_zeros += data[i + 1] == 0;
    }
    // A stray NUL in a short file is not enough.
    const size_t enough = std::min<size_t>(4, (pairs + 1) / 2);
    if (odd_zeros > even_zeros * 4 && odd_zeros * 8 >= pairs && odd_zeros >= enough)
        return TextEncoding::Utf16LE;
    if (even_zeros > odd_zeros * 4 && even_zeros * 8 >= pairs && even_zeros >= enough)
        return TextEncoding::Utf16BE;

    if (isValidUtf8(data, size))
        return TextEncoding::Utf8;
    return TextEncoding::Cp932;
}

// Returns the number of bytes consumed; the rest (an incomplete character)
// is passed again with the next piece. With `final`, everything is consumed.
size_t TextDecoder::decodeSome(const unsigned char *data, size_t size, std::string &out, bool final)
{
    size_t i = 0;
    if (at_start_)
    {
        static constexpr unsigned char UTF8_BOM[] = {0xEF, 0xBB, 0xBF};
        static constexpr unsigned char UTF16LE_BOM[] = {0xFF, 0xFE};
        static constexpr unsigned char UTF16BE_BOM[] = {0xFE, 0xFF};
        std::span<const unsigned char> bom;
        if (encoding_ == TextEncoding::Utf8)
            bom = UTF8_BOM;
        else if (encoding_ == TextEncoding::Utf16LE)
            bom = UTF16LE_BOM;
        else if (encoding_ == TextEncoding::Utf16BE)
            bom = UTF16BE_BOM;
        if (size < bom.size() && !final && std::equal(data, data + size, bom.begin()))
            return 0;
        if (size >= bom.size() && std::equal(bom.begin(), bom.end(), data))
            i = bom.size();
        at_start_ = false;
    }

    switch (encoding_)
    {
    case TextEncoding::Utf8:
        out.append(reinterpret_cast<const char *>(data + i), size - i);
        return size;

    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE:
    {
        const bool le = encoding_ == TextEncoding::Utf16LE;
        auto unit = [&](size_t at) -> char32_t
        {
            return le ? data[at] | (data[at + 1] << 8) : (data[at] << 8) | data[at + 1];
        };
        while (i + 2 <= size)
        {
            const char32_t u = unit(i);
            if (u < 0x80)
            {
                out.push_back(static_cast<char>(u));
                i += 2;
                continue;
            }
            if (u >= 0xD800 && u <= 0xDBFF)
            {
                if (i + 4 > size)
                {
                    if (!final)
                        break;
                    appendUtf8(out, REPLACEMENT_CHARACTER);
                    i += 2;
                    continue;
                }
                const char32_t low = unit(i + 2);
                if (low >= 0xDC00 && low <= 0xDFFF)
                {
                    appendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00));
                    i += 4;
                }
                else
                {
                    appendUtf8(out, REPLACEMENT_CHARACTER);
                    i += 2;
                }
                continue;
            }
            appendUtf8(out, u >= 0xDC00 && u <= 0xDFFF ? REPLACEMENT_CHARACTER : u);
            i += 2;
        }
        if (final && i < size)
        {
            appendUtf8(out, REPLACEMENT_CHARACTER);
            i = size;
        }
        return i;
    }

    case TextEncoding::Cp932:
        while (i < size)
        {
            const unsigned char b = data[i];
            if (b < 0x80)
            {
                size_t end = i + 1;
                while (end < size && data[end] < 0x80)
                    ++end;
                out.append(reinterpret_cast<const char *>(data + i), end - i);
                i = end;
            }
            else if (b >= 0xA1 && b <= 0xDF)
            {
                // Half-width katakana.
                appendUtf8(out, 0xFF61 + (b - 0xA1));
                ++i;
            }
            else if (isCp932Lead(b))
            {
                if (i + 1 >= size)
                {
                    if (!final)
                        break;
                    appendUtf8(out, REPLACEMENT_CHARACTER);
                    ++i;
                }
                else if (const unsigned char trail = data[i + 1]; isCp932Trail(trail))
                {
                    const uint16_t cp = CP932_TABLE[cp932LeadIndex(b) * CP932_TRAIL_COUNT + cp932TrailIndex(trail)];
                    appendUtf8(out, cp ? cp : REPLACEMENT_CHARACTER);
                    i += 2;
                }
                else
                {
                    // Not part of the character: a line break after a stray lead byte stays one.
                    appendUtf8(out, REPLACEMENT_CHARACTER);
                    ++i;
                }
            }
            else
            {
                appendUtf8(out, REPLACEMENT_CHARACTER);
                ++i;
            }
        }
        return i;
    }
    return size;
}

void TextDecoder::decode(std::span<const char> in, std::string &out)
{
    if (pending_.empty())
    {
        const auto *data = reinterpret_cast<const unsigned char *>(in.data());
        const size_t consumed = decodeSome(data, in.size(), out, false);
        pending_.assign(in.data() + consumed, in.size() - consumed);
        return;
    }
    pending_.append(in.data(), in.size());
    const size_t consumed = decodeSome(reinterpret_cast<const unsigned char *>(pending_.data()), pending_.size(), out, false);
    pending_.erase(0, consumed);
}

void TextDecoder::finish(std::string &out)
{
    decodeSome(reinterpret_cast<const unsigned char *>(pending_.data()), pending_.size(), out, true);
    pending_.clear();
}

std::unique_ptr<std::istream> openDecodingStream(std::unique_ptr<std::istream> source, TextEncoding encoding)
{
    return std::make_unique<DecodingStream>(std::move(source), encoding);
}

TextCharacter textCharacterAt(TextEncoding encoding, const unsigned char *data, size_t size, size_t i, bool final)
{
    const unsigned char b = data[i];
    switch (encoding)
    {
    case TextEncoding::Utf8:
        // Bytes of a multibyte sequence are never ASCII.
        return {1, static_cast<char>(b < 0x80 ? b : 0)};
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE:
    {
        if (i + 2 > size)
            return {final ? size - i : 0, 0};
        const unsigned unit = encoding == TextEncoding::Utf16LE ? b | (data[i + 1] << 8) : (b << 8) | data[i + 1];
        // Surrogates come in units of their own, neither of them ASCII.
        return {2, static_cast<char>(unit < 0x80 ? unit : 0)};
    }
    case TextEncoding::Cp932:
        if (b < 0x80)
            return {1, static_cast<char>(b)};
        if (isCp932Lead(b))
        {
            if (i + 1 >= size)
                return {final ? size_t{1} : size_t{0}, 0};
            // A stray lead byte is a character of its own (see decodeSome()).
            return {isCp932Trail(data[i + 1]) ? size_t{2} : size_t{1}, 0};
        }
        return {1, 0};
    }
    return {1, 0};
}

std::string_view byteOrderMark(TextEncoding encoding)
{
    switch (encoding)
    {
    case TextEncoding::Utf8:
        return "\xEF\xBB\xBF";
    case TextEncoding::Utf16LE:
        return "\xFF\xFE";
    case TextEncoding::Utf16BE:
        return "\xFE\xFF";
    case TextEncoding::Cp932:
        break;
    }
    return {};
}

namespace
{
    // Unicode (BMP) -> CP932 double-byte code. Where CP932 has a character
    // twice, Windows writes the lowest code, except that IBM extensions
    // (0xFA-0xFC) win over their NEC-selected copies (0xED-0xEE).
    const std::vector<uint16_t> &cp932EncodeTable()
    {
        static const std::vector<uint16_t> table = []
        {
            std::vector<uint16_t> codes(0x10000, 0);
            auto rank = [](uint16_t code)
            {
                const unsigned lead = code >> 8;
                return std::pair<int, uint16_t>(lead == 0xED || lead == 0xEE ? 1 : 0, code);
            };
            for (unsigned lead = 0x81; lead <= 0xFC; ++lead)
            {
                if (!isCp932Lead(static_cast<unsigned char>(lead)))
                    continue;
                for (unsigned trail = 0x40; trail <= 0xFC; ++trail)
                {
                    if (!isCp932Trail(static_cast<unsigned char>(trail)))
                        continue;
                    const uint16_t cp = CP932_TABLE[cp932LeadIndex(static_cast<unsigned char>(lead)) * CP932_TRAIL_COUNT +
                                                    cp932TrailIndex(static_cast<unsigned char>(trail))];
                    if (cp == 0)
                        continue;
                    const auto code = static_cast<uint16_t>(lead << 8 | trail);
                    if (codes[cp] == 0 || rank(code) < rank(codes[cp]))
                        codes[cp] = code;
                }
            }
            return codes;
        }();
        return table;
    }
} // namespace

std::optional<std::string> encodeText(std::string_view utf8, TextEncoding encoding)
{
    const auto *data = reinterpret_cast<const unsigned char *>(utf8.data());
    const size_t size = utf8.size();
    std::string out;
    out.reserve(encoding == TextEncoding::Cp932 ? size : size * 2);
    for (size_t i = 0; i < size;)
    {
        const size_t length = utf8SequenceLength(data, size, i, false);
        if (length == 0)
            return std::nullopt;
        char32_t cp = length == 1 ? data[i] : data[i] & (0x7F >> length);
        for (size_t k = 1; k < length; ++k)
            cp = (cp << 6) | (data[i + k] & 0x3F);
        i += length;

        switch (encoding)
        {
        case TextEncoding::Utf8:
            break;
        case TextEncoding::Utf16LE:
        case TextEncoding::Utf16BE:
        {
            auto unit = [&](char32_t u)
            {
                const char high = static_cast<char>(u >> 8), low = static_cast<char>(u & 0xFF);
                out.push_back(encoding == TextEncoding::Utf16LE ? low : high);
                out.push_back(encoding == TextEncoding::Utf16LE ? high : low);
            };
            if (cp >= 0x10000)
            {
                unit(0xD800 + ((cp - 0x10000) >> 10));
                unit(0xDC00 + ((cp - 0x10000) & 0x3FF));
            }
            else
                unit(cp);
            continue;
        }
        case TextEncoding::Cp932:
            if (cp < 0x80)
                out.push_back(static_cast<char>(cp));
            else if (cp >= 0xFF61 && cp <= 0xFF9F)
                out.push_back(static_cast<char>(0xA1 + (cp - 0xFF61)));
            else if (const uint16_t code = cp < 0x10000 ? cp932EncodeTable()[cp] : 0; code != 0)
            {
                out.push_back(static_cast<char>(code >> 8));
                out.push_back(static_cast<char>(code & 0xFF));
            }
            else
                return std::nullopt;
            continue;
        }
        out.append(reinterpret_cast<const char *>(data + i - length), length);
    }
    return out;
}

std::optional<std::string> repairUtf8(std::string_view text)
{
    const auto *data = reinterpret_cast<const unsigned char *>(text.data());
    size_t i = 0;
    size_t length = 0;
    while (i < text.size() && (length = utf8SequenceLength(data, text.size(), i, false)) != 0)
        i += length;
    if (i == text.size())
        return std::nullopt;
    std::string repaired(text.substr(0, i));
    while (i < text.size())
    {
        length = utf8SequenceLength(data, text.size(), i, false);
        if (length == 0)
        {
            appendUtf8(repaired, REPLACEMENT_CHARACTER);
            ++i;
        }
        else
        {
            repaired.append(text.substr(i, length));
            i += length;
        }
    }
    return repaired;
}
