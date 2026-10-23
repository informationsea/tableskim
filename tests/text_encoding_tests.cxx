#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <sstream>
#include <string>

#include "cp932_table.hxx"
#include "text_encoding.hxx"

namespace
{
    // Decodes `bytes` in pieces of `piece` bytes.
    std::string decode(const std::string &bytes, TextEncoding encoding, size_t piece = 4096)
    {
        TextDecoder decoder(encoding);
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += piece)
            decoder.decode(std::span<const char>(bytes.data() + i, std::min(piece, bytes.size() - i)), out);
        decoder.finish(out);
        return out;
    }

    TextEncoding detect(const std::string &bytes)
    {
        return detectTextEncoding(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    }

    // "ID,名前\r\n1,表\r\n" in UTF-16 (`le`: little-endian).
    std::string utf16Sample(bool le)
    {
        const char16_t text[] = u"ID,名前\r\n1,表\r\n";
        std::string bytes;
        for (const char16_t c : std::u16string_view(text))
        {
            const char hi = static_cast<char>(c >> 8), lo = static_cast<char>(c & 0xFF);
            bytes += le ? std::string{lo, hi} : std::string{hi, lo};
        }
        return bytes;
    }

    const std::string REPLACEMENT = "\xEF\xBF\xBD"; // U+FFFD
} // namespace

TEST_CASE("textEncodingName and parseTextEncoding", "[encoding]")
{
    for (const TextEncoding encoding : TEXT_ENCODINGS)
        CHECK(parseTextEncoding(textEncodingName(encoding)) == encoding);
    CHECK(parseTextEncoding("utf8") == TextEncoding::Utf8);
    CHECK(parseTextEncoding("UTF-16le") == TextEncoding::Utf16LE);
    CHECK(parseTextEncoding("utf_16_be") == TextEncoding::Utf16BE);
    for (const char *alias : {"cp932", "Shift_JIS", "sjis", "Windows-31J", "MS932"})
    {
        INFO(alias);
        CHECK(parseTextEncoding(alias) == TextEncoding::Cp932);
    }
    CHECK_FALSE(parseTextEncoding("utf-16"));
    CHECK_FALSE(parseTextEncoding("latin1"));
    CHECK_FALSE(parseTextEncoding(""));
}

TEST_CASE("UTF-8 is passed through without its BOM", "[encoding]")
{
    const size_t piece = GENERATE(1, 2, 3, 4096);
    INFO("piece " << piece);
    CHECK(decode("\xEF\xBB\xBF" "a,日本\n", TextEncoding::Utf8, piece) == "a,日本\n");
    CHECK(decode("a,日本\n", TextEncoding::Utf8, piece) == "a,日本\n");
    // Only a BOM at the very start is dropped; invalid bytes stay as they are.
    CHECK(decode("a\xEF\xBB\xBF\xFF", TextEncoding::Utf8, piece) == "a\xEF\xBB\xBF\xFF");
    CHECK(decode("\xEF\xBB", TextEncoding::Utf8, piece) == "\xEF\xBB");
    CHECK(decode("", TextEncoding::Utf8, piece).empty());
}

TEST_CASE("UTF-16 is converted, with or without a BOM", "[encoding]")
{
    const size_t piece = GENERATE(1, 2, 3, 4096);
    INFO("piece " << piece);
    const std::string expected = "ID,名前\r\n1,表\r\n";
    CHECK(decode(utf16Sample(true), TextEncoding::Utf16LE, piece) == expected);
    CHECK(decode("\xFF\xFE" + utf16Sample(true), TextEncoding::Utf16LE, piece) == expected);
    CHECK(decode(utf16Sample(false), TextEncoding::Utf16BE, piece) == expected);
    CHECK(decode("\xFE\xFF" + utf16Sample(false), TextEncoding::Utf16BE, piece) == expected);

    // A surrogate pair (U+1F600) and broken ones.
    CHECK(decode(std::string("\x3D\xD8\x00\xDE", 4), TextEncoding::Utf16LE, piece) == "\xF0\x9F\x98\x80");
    CHECK(decode(std::string("\x3D\xD8" "a\x00", 4), TextEncoding::Utf16LE, piece) == REPLACEMENT + "a");
    CHECK(decode(std::string("\x00\xDE" "a\x00", 4), TextEncoding::Utf16LE, piece) == REPLACEMENT + "a");
    CHECK(decode(std::string("a\x00\x3D\xD8", 4), TextEncoding::Utf16LE, piece) == "a" + REPLACEMENT);
    // An odd byte at the end.
    CHECK(decode(std::string("a\x00" "b", 3), TextEncoding::Utf16LE, piece) == "a" + REPLACEMENT);
}

TEST_CASE("CP932 is converted with its extensions", "[encoding]")
{
    const size_t piece = GENERATE(1, 2, 3, 4096);
    INFO("piece " << piece);
    // 表 and ソ end in 0x5C ('\'), ポ in 0x7C ('|'): neither is a separator.
    CHECK(decode("\x95\x5C,\x83\x5C|\x83\x7C\r\n", TextEncoding::Cp932, piece) == "表,ソ|ポ\r\n");
    // Half-width katakana, NEC special (①, ㈱) and IBM extension (髙) characters.
    CHECK(decode("\xB6\xC0\x87\x40\x87\x8A\xFB\xFC", TextEncoding::Cp932, piece) == "ｶﾀ①㈱髙");
    // Microsoft's mappings: 〜 is U+FF5E, − is U+FF0D.
    CHECK(decode("\x81\x60\x81\x7C", TextEncoding::Cp932, piece) == "～－");
    // ASCII stays ASCII, '\' and '~' included.
    CHECK(decode("a\\b~c", TextEncoding::Cp932, piece) == "a\\b~c");
}

TEST_CASE("invalid CP932 becomes U+FFFD without eating separators", "[encoding]")
{
    const size_t piece = GENERATE(1, 2, 4096);
    INFO("piece " << piece);
    // A lead byte followed by a line break or comma.
    CHECK(decode("\x95\n\x95,", TextEncoding::Cp932, piece) == REPLACEMENT + "\n" + REPLACEMENT + ",");
    // A lead byte at the end of the file.
    CHECK(decode("a\x95", TextEncoding::Cp932, piece) == "a" + REPLACEMENT);
    // An unmapped pair is one character; bytes that never start one are one each.
    CHECK(decode("\x85\x40" "a", TextEncoding::Cp932, piece) == REPLACEMENT + "a");
    CHECK(decode("\x80\xA0\xFD", TextEncoding::Cp932, piece) == REPLACEMENT + REPLACEMENT + REPLACEMENT);
}

TEST_CASE("the CP932 table has every character of the codec", "[encoding]")
{
    const auto mapped = std::count_if(std::begin(CP932_TABLE), std::end(CP932_TABLE), [](uint16_t cp)
                                      { return cp != 0; });
    CHECK(mapped == 9604);
    CHECK(CP932_TABLE[cp932LeadIndex(0x82) * CP932_TRAIL_COUNT + cp932TrailIndex(0xA0)] == u'あ');
    CHECK(CP932_TABLE[cp932LeadIndex(0xEA) * CP932_TRAIL_COUNT + cp932TrailIndex(0xA4)] == u'熙');
    // The user-defined area maps to the private use area.
    CHECK(CP932_TABLE[cp932LeadIndex(0xF0) * CP932_TRAIL_COUNT + cp932TrailIndex(0x40)] == 0xE000);
}

TEST_CASE("detectTextEncoding", "[encoding]")
{
    CHECK(detect("") == TextEncoding::Utf8);
    CHECK(detect("a,b\n1,2\n") == TextEncoding::Utf8);
    CHECK(detect("名前,部署\n山田,人事部\n") == TextEncoding::Utf8);
    // A character cut off by the end of the sample is still UTF-8.
    CHECK(detect(std::string("名前,部署\n山田,人事部\n").substr(0, 19)) == TextEncoding::Utf8);
    CHECK(detect("\xEF\xBB\xBF" "a") == TextEncoding::Utf8);

    CHECK(detect("\xFF\xFE" + utf16Sample(true)) == TextEncoding::Utf16LE);
    CHECK(detect("\xFE\xFF" + utf16Sample(false)) == TextEncoding::Utf16BE);
    CHECK(detect(utf16Sample(true)) == TextEncoding::Utf16LE);
    CHECK(detect(utf16Sample(false)) == TextEncoding::Utf16BE);
    // One NUL byte in a short file does not make it UTF-16; a tiny UTF-16 file
    // is still recognised.
    CHECK(detect(std::string("a,b\n1,x\0y\n", 11)) == TextEncoding::Utf8);
    CHECK(detect(std::string("a\0", 2)) == TextEncoding::Utf16LE);
    CHECK(detect(std::string("1\0,\0" "2\0", 6)) == TextEncoding::Utf16LE);

    // "名前,部署" in CP932.
    CHECK(detect("\x96\xBC\x91\x4F,\x95\x94\x8F\x90\r\n") == TextEncoding::Cp932);
    // Overlong forms and surrogates are not UTF-8.
    CHECK(detect("\xC0\xAF") == TextEncoding::Cp932);
    CHECK(detect("\xED\xA0\x80") == TextEncoding::Cp932);
}

TEST_CASE("repairUtf8", "[encoding]")
{
    CHECK_FALSE(repairUtf8("plain, 日本語 and 😀"));
    CHECK_FALSE(repairUtf8(""));
    CHECK(repairUtf8("caf\xe9") == "caf\xEF\xBF\xBD");
    // A cut-off sequence, a surrogate and an overlong form.
    CHECK(repairUtf8("a\xE3\x81") == "a\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(repairUtf8("\xED\xA0\x80x") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDx");
    CHECK(repairUtf8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST_CASE("openDecodingStream yields UTF-8", "[encoding]")
{
    // Longer than a read chunk, with a character across each chunk boundary.
    std::string cp932;
    std::string expected;
    for (int i = 0; i < 50000; ++i)
    {
        cp932 += "\x95\x5C" "a";
        expected += "表a";
    }
    auto stream = openDecodingStream(std::make_unique<std::istringstream>(cp932), TextEncoding::Cp932);
    std::ostringstream out;
    out << stream->rdbuf();
    CHECK(out.str() == expected);
}

TEST_CASE("encodeText is the reverse of the decoder", "[encoding]")
{
    const std::string text = "a,b\n日本語 😀 ｶﾀｶﾅ";
    for (const TextEncoding encoding : {TextEncoding::Utf8, TextEncoding::Utf16LE, TextEncoding::Utf16BE})
    {
        const auto encoded = encodeText(text, encoding);
        REQUIRE(encoded);
        CHECK(decode(*encoded, encoding) == text);
    }
    CHECK(*encodeText("A😀", TextEncoding::Utf16LE) == std::string("A\0\x3D\xD8\x00\xDE", 6));
    CHECK(*encodeText("A", TextEncoding::Utf16BE) == std::string("\0A", 2));
    // Not valid UTF-8.
    CHECK_FALSE(encodeText("\xff", TextEncoding::Utf16LE));

    const std::string cp932 = "日本語 ｶﾀｶﾅ ①Ⅰ ～－￠ \\~";
    const auto encoded = encodeText(cp932, TextEncoding::Cp932);
    REQUIRE(encoded);
    CHECK(decode(*encoded, TextEncoding::Cp932) == cp932);
    CHECK_FALSE(encodeText("😀", TextEncoding::Cp932));
    CHECK_FALSE(encodeText("€", TextEncoding::Cp932));
}

TEST_CASE("encodeText writes the CP932 code Windows writes", "[encoding]")
{
    // Characters CP932 has twice: the lowest code, but IBM extensions
    // (0xFA-0xFC) rather than their NEC-selected copies (0xED-0xEE).
    CHECK(*encodeText("≒", TextEncoding::Cp932) == "\x81\xE0");
    CHECK(*encodeText("￢", TextEncoding::Cp932) == "\x81\xCA");
    CHECK(*encodeText("∵", TextEncoding::Cp932) == "\x81\xE6");
    CHECK(*encodeText("Ⅰ", TextEncoding::Cp932) == "\x87\x54");
    CHECK(*encodeText("㈱", TextEncoding::Cp932) == "\x87\x8A");
    CHECK(*encodeText("ⅰ", TextEncoding::Cp932) == "\xFA\x40");
    CHECK(*encodeText("纊", TextEncoding::Cp932) == "\xFA\x5C");
    CHECK(*encodeText("髙", TextEncoding::Cp932) == "\xFB\xFC");
    CHECK(*encodeText("ｶ", TextEncoding::Cp932) == "\xB6");
}

TEST_CASE("textCharacterAt splits text as the decoder does", "[encoding]")
{
    auto split = [](const std::string &bytes, TextEncoding encoding)
    {
        std::vector<std::pair<size_t, char>> characters;
        const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
        for (size_t i = 0; i < bytes.size();)
        {
            const TextCharacter c = textCharacterAt(encoding, data, bytes.size(), i, true);
            characters.emplace_back(c.length, c.ascii);
            i += c.length;
        }
        return characters;
    };
    using Split = std::vector<std::pair<size_t, char>>;
    // A trail byte that looks like '|' is not one.
    CHECK(split("\x83\x7C|", TextEncoding::Cp932) == Split{{2, 0}, {1, '|'}});
    // A stray lead byte does not eat the line break.
    CHECK(split("\x83\n", TextEncoding::Cp932) == Split{{1, 0}, {1, '\n'}});
    CHECK(split("\xB6,", TextEncoding::Cp932) == Split{{1, 0}, {1, ','}});
    CHECK(split(std::string(",\0\x30\x42", 4), TextEncoding::Utf16LE) == Split{{2, ','}, {2, 0}});
    CHECK(split(std::string("\0,\x30", 3), TextEncoding::Utf16BE) == Split{{2, ','}, {1, 0}});
    CHECK(split("\xE3\x81\x82,", TextEncoding::Utf8) == Split{{1, 0}, {1, 0}, {1, 0}, {1, ','}});

    const auto *cut = reinterpret_cast<const unsigned char *>("\x83");
    CHECK(textCharacterAt(TextEncoding::Cp932, cut, 1, 0, false).length == 0);
    CHECK(byteOrderMark(TextEncoding::Utf16LE) == "\xFF\xFE");
    CHECK(byteOrderMark(TextEncoding::Cp932).empty());
}
