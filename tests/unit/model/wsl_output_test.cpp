#include "model/wsl_output.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

using wsldisk::model::decode_utf16le;
using wsldisk::model::parse_distribution_list;

namespace {

/// Encodes UTF-16LE the way wsl.exe writes it, so the tests read as the text
/// they are about rather than as byte tables.
std::vector<std::byte> utf16le(std::u16string_view units, bool with_bom = false) {
    std::vector<std::byte> bytes;
    if (with_bom) {
        bytes.push_back(std::byte{0xFF});
        bytes.push_back(std::byte{0xFE});
    }
    for (const char16_t unit : units) {
        bytes.push_back(static_cast<std::byte>(unit & 0xFF));
        bytes.push_back(static_cast<std::byte>((unit >> 8) & 0xFF));
    }
    return bytes;
}

std::span<const std::byte> bytes_of(const std::vector<std::byte>& bytes) {
    return bytes;
}

}  // namespace

TEST_CASE("decode_utf16le reads plain ASCII", "[model][wsl-output]") {
    const auto bytes = utf16le(u"Ubuntu");
    CHECK(decode_utf16le(bytes_of(bytes)) == "Ubuntu");
}

TEST_CASE("decode_utf16le consumes a byte-order mark", "[model][wsl-output]") {
    // wsl.exe emits one on some builds and not others, and a BOM left in the
    // text would become part of the first distribution's name.
    const auto bytes = utf16le(u"Ubuntu", true);
    CHECK(decode_utf16le(bytes_of(bytes)) == "Ubuntu");
}

TEST_CASE("decode_utf16le handles an empty stream", "[model][wsl-output]") {
    CHECK(decode_utf16le({}).empty());
}

TEST_CASE("decode_utf16le keeps a lone 0xFF byte that is not a BOM", "[model][wsl-output]") {
    // Two bytes, but not FF FE: it is a character, not a mark.
    const std::vector<std::byte> bytes{std::byte{0xFF}, std::byte{0x00}};
    CHECK(decode_utf16le(bytes_of(bytes)) == "\xC3\xBF");  // U+00FF
}

TEST_CASE("decode_utf16le encodes each UTF-8 width", "[model][wsl-output]") {
    // One byte, two, three and four -- the four arms of the encoder.
    const auto bytes = utf16le(u"Aé中\U0001F600");
    CHECK(decode_utf16le(bytes_of(bytes)) == "A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80");
}

TEST_CASE("decode_utf16le drops a trailing half unit", "[model][wsl-output]") {
    // A pipe read can end mid-character; the odd byte is not a character.
    auto bytes = utf16le(u"Ok");
    bytes.push_back(std::byte{0x41});
    CHECK(decode_utf16le(bytes_of(bytes)) == "Ok");
}

TEST_CASE("decode_utf16le replaces an unpaired high surrogate", "[model][wsl-output]") {
    const auto bytes = utf16le(u"\xD83D");
    CHECK(decode_utf16le(bytes_of(bytes)) == "\xEF\xBF\xBD");
}

TEST_CASE("decode_utf16le replaces a high surrogate followed by a normal unit", "[model][wsl-output]") {
    // Spelled out unit by unit: "\xD83DA" would parse as one oversized escape.
    const std::u16string units{u'\xD83D', u'A'};
    const auto bytes = utf16le(units);
    CHECK(decode_utf16le(bytes_of(bytes)) == "\xEF\xBF\xBD\x41");
}

TEST_CASE("decode_utf16le replaces an unpaired low surrogate", "[model][wsl-output]") {
    const auto bytes = utf16le(u"\xDE00");
    CHECK(decode_utf16le(bytes_of(bytes)) == "\xEF\xBF\xBD");
}

TEST_CASE("decode_utf16le keeps an embedded NUL", "[model][wsl-output]") {
    // Stripping NULs is how a UTF-16 list gets read as a single name.
    const auto bytes = utf16le(std::u16string_view{u"a\0b", 3});
    CHECK(decode_utf16le(bytes_of(bytes)) == std::string{"a\0b", 3});
}

TEST_CASE("parse_distribution_list reads CRLF-separated names", "[model][wsl-output]") {
    const auto bytes = utf16le(u"Ubuntu\r\nAlpine\r\n");
    const auto names = parse_distribution_list(bytes_of(bytes));
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Ubuntu");
    CHECK(names[1] == "Alpine");
}

TEST_CASE("parse_distribution_list handles a final line with no newline", "[model][wsl-output]") {
    const auto bytes = utf16le(u"Ubuntu\r\nAlpine");
    const auto names = parse_distribution_list(bytes_of(bytes));
    REQUIRE(names.size() == 2);
    CHECK(names[1] == "Alpine");
}

TEST_CASE("parse_distribution_list drops blank lines", "[model][wsl-output]") {
    const auto bytes = utf16le(u"\r\nUbuntu\r\n\r\n");
    const auto names = parse_distribution_list(bytes_of(bytes));
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "Ubuntu");
}

TEST_CASE("parse_distribution_list drops a line that is only NULs", "[model][wsl-output]") {
    // A truncated read can leave one behind, and it is not a distribution.
    const auto bytes = utf16le(std::u16string_view{u"Ubuntu\r\n\0\0", 10});
    const auto names = parse_distribution_list(bytes_of(bytes));
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "Ubuntu");
}

TEST_CASE("parse_distribution_list trims surrounding whitespace", "[model][wsl-output]") {
    const auto bytes = utf16le(u"  Ubuntu  \r\n");
    const auto names = parse_distribution_list(bytes_of(bytes));
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "Ubuntu");
}

TEST_CASE("parse_distribution_list reads nothing from an empty stream", "[model][wsl-output]") {
    CHECK(parse_distribution_list({}).empty());
}

TEST_CASE("parse_distribution_list keeps names with spaces inside", "[model][wsl-output]") {
    const auto bytes = utf16le(u"Ubuntu 22.04 LTS\r\n");
    const auto names = parse_distribution_list(bytes_of(bytes));
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "Ubuntu 22.04 LTS");
}

TEST_CASE("parse_distribution_list keeps a non-ASCII name", "[model][wsl-output]") {
    const auto bytes = utf16le(u"テスト\r\n", true);
    const auto names = parse_distribution_list(bytes_of(bytes));
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88");
}

TEST_CASE("decode_utf16le replaces a high surrogate followed by a private-use unit", "[model][wsl-output]") {
    // 0xE000 is above the surrogate block, so it fails the upper half of the
    // pairing test rather than the lower half a plain ASCII follower fails.
    const std::u16string units{u'\xD83D', u'\xE000'};
    const auto bytes = utf16le(units);
    CHECK(decode_utf16le(bytes_of(bytes)) == "\xEF\xBF\xBD\xEE\x80\x80");
}

TEST_CASE("decode_utf16le keeps a unit just past the surrogate block", "[model][wsl-output]") {
    // U+E000 is the first code point after the surrogates: it must decode
    // normally rather than being mistaken for an unpaired low surrogate.
    const auto bytes = utf16le(u"\xE000");
    CHECK(decode_utf16le(bytes_of(bytes)) == "\xEE\x80\x80");
}
