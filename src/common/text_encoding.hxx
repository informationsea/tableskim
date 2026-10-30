#ifndef _TEXT_ENCODING_HXX_
#define _TEXT_ENCODING_HXX_

#include <array>
#include <cstddef>
#include <istream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Character encodings of text files (CSV, TSV, ...). Everything past the
// backend is UTF-8, so files in other encodings are converted while read.
enum class TextEncoding
{
    Utf8,
    Utf16LE,
    Utf16BE,
    Cp932, // Windows Shift_JIS, as written by Japanese Excel
};

// In the order the UIs list them.
inline constexpr std::array<TextEncoding, 4> TEXT_ENCODINGS = {
    TextEncoding::Utf8,
    TextEncoding::Utf16LE,
    TextEncoding::Utf16BE,
    TextEncoding::Cp932,
};

// "UTF-8", "UTF-16LE", "UTF-16BE", "Shift_JIS (CP932)".
const char *textEncodingName(TextEncoding encoding);

// Accepts the names above and common aliases (utf8, sjis, shift_jis,
// windows-31j, ...), ignoring case, '-' and '_'.
std::optional<TextEncoding> parseTextEncoding(std::string_view name);

// Guesses the encoding from the first bytes of a file: a byte order mark,
// NUL bytes in every other position (UTF-16 without a BOM), valid UTF-8, and
// otherwise CP932. A character cut off at the end of `head` is ignored.
TextEncoding detectTextEncoding(std::span<const std::byte> head);

// Converts text to UTF-8 piece by piece; a character split between two
// pieces is completed by the next one. A byte order mark at the start is
// dropped. Invalid CP932 and UTF-16 input becomes U+FFFD; UTF-8 is passed
// through unchecked.
class TextDecoder
{
private:
    TextEncoding encoding_;
    bool at_start_ = true;
    std::string pending_; // bytes of an incomplete character (or BOM)

    size_t decodeSome(const unsigned char *data, size_t size, std::string &out, bool final);

public:
    explicit TextDecoder(TextEncoding encoding) : encoding_(encoding) {}
    // Appends the converted text to `out`.
    void decode(std::span<const char> in, std::string &out);
    // Flushes an incomplete character at the end of the input as U+FFFD.
    void finish(std::string &out);
};

// One character of text in `encoding`, as TextDecoder splits the text.
struct TextCharacter
{
    size_t length = 0; // in bytes; 0 if more bytes are needed
    char ascii = 0;    // the character if it is ASCII (and not NUL), else 0
};
// The character at data[i] (i < size); one cut off at the end of `data` has
// length 0 unless `final` says that the text ends there. A byte order mark
// is not recognised here.
TextCharacter textCharacterAt(TextEncoding encoding, const unsigned char *data, size_t size, size_t i, bool final);

// The byte order mark of the encoding (empty for CP932).
std::string_view byteOrderMark(TextEncoding encoding);

// Converts UTF-8 text to `encoding`, without a byte order mark; nullopt if
// it is not valid UTF-8 or has a character the encoding lacks. Characters
// CP932 has twice become the code Windows writes.
std::optional<std::string> encodeText(std::string_view utf8, TextEncoding encoding);

// Length of the valid UTF-8 sequence at text[i] (1 to 4), or 0 if it is
// invalid: overlong, a surrogate, beyond U+10FFFF, or cut off.
size_t utf8SequenceLength(std::string_view text, size_t i);

// For consumers that refuse invalid UTF-8 (DuckDB): `text` with each byte
// that starts no valid sequence replaced by U+FFFD, or nothing if `text` is
// valid already.
std::optional<std::string> repairUtf8(std::string_view text);

// Wraps a stream of text in `encoding` so that it yields UTF-8. Read errors
// of `source` still throw (badbit is in exceptions()).
std::unique_ptr<std::istream> openDecodingStream(std::unique_ptr<std::istream> source, TextEncoding encoding);

#endif // _TEXT_ENCODING_HXX_
