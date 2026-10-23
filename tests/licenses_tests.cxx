#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "licenses.hxx"

TEST_CASE("licenses.third_party_text", "[licenses]")
{
    const std::string_view text = thirdPartyLicenses();
    REQUIRE_FALSE(text.empty());
    // NUL-terminated so it can be handed to C APIs such as Fl_Text_Buffer.
    CHECK(text.data()[text.size()] == '\0');
    CHECK(text.find("TableSkim") != std::string_view::npos);
    CHECK(text.find("DuckDB") != std::string_view::npos);
    CHECK(text.find("Zstandard") != std::string_view::npos);
    CHECK(text.find("FTXUI") != std::string_view::npos);
#ifdef TABLESKIM_TESTS_HAVE_GUI
    CHECK(text.find("Font: BIZ UDPGothic") != std::string_view::npos);
    CHECK(text.find("SIL OPEN FONT LICENSE Version 1.1") != std::string_view::npos);
#endif
}
