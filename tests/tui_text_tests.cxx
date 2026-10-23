#include <catch2/catch_test_macros.hpp>

#include "tui_text.hxx"

TEST_CASE("fitText pads and cuts by terminal columns")
{
    CHECK(fitText("abc", 5, TextAlign::Left) == "abc  ");
    CHECK(fitText("abc", 5, TextAlign::Right) == "  abc");
    CHECK(fitText("abc", 6, TextAlign::Center) == " abc  ");
    CHECK(fitText("abcdef", 4, TextAlign::Left) == "abc…");
    CHECK(fitText("abcdef", 4, TextAlign::Left, false) == "abcd");
    CHECK(fitText("abc", 0, TextAlign::Left).empty());

    // Wide characters take two columns; one that does not fit leaves a gap.
    CHECK(fitText("日本", 4, TextAlign::Left) == "日本");
    CHECK(fitText("日本語", 4, TextAlign::Left) == "日… ");
    CHECK(fitText("日本語", 3, TextAlign::Left, false) == "日 ");
}

TEST_CASE("wrapText breaks at spaces and inside long words")
{
    CHECK(wrapText("", 10) == std::vector<std::string>{""});
    CHECK(wrapText("hello world", 20) == std::vector<std::string>{"hello world"});
    CHECK(wrapText("hello world foo", 11) == std::vector<std::string>{"hello ", "world foo"});
    CHECK(wrapText("abcdefghij", 4) == std::vector<std::string>{"abcd", "efgh", "ij"});
    CHECK(wrapText("ab cdefghij", 4) == std::vector<std::string>{"ab ", "cdef", "ghij"});
    CHECK(wrapText("日本語テキスト", 6) == std::vector<std::string>{"日本語", "テキス", "ト"});
}

TEST_CASE("eraseLastCharacter removes one UTF-8 character")
{
    std::string text = "aé日";
    eraseLastCharacter(text);
    CHECK(text == "aé");
    eraseLastCharacter(text);
    CHECK(text == "a");
    eraseLastCharacter(text);
    CHECK(text.empty());
    eraseLastCharacter(text);
    CHECK(text.empty());
}
