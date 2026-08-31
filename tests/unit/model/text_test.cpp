#include "model/text.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using wsldisk::model::to_utf8;
using wsldisk::model::to_wide;

TEST_CASE("to_utf8 passes ASCII through", "[model][text]") {
    CHECK(to_utf8(L"Ubuntu") == "Ubuntu");
}

TEST_CASE("to_utf8 encodes each width", "[model][text]") {
    CHECK(to_utf8(L"Aé中") == "A\xC3\xA9\xE4\xB8\xAD");
}

TEST_CASE("to_utf8 encodes a surrogate pair as one code point", "[model][text]") {
    const std::wstring emoji{L'\xD83D', L'\xDE00'};  // U+1F600
    CHECK(to_utf8(emoji) == "\xF0\x9F\x98\x80");
}

TEST_CASE("to_utf8 replaces an unpaired high surrogate", "[model][text]") {
    const std::wstring lone{L'\xD83D'};
    CHECK(to_utf8(lone) == "\xEF\xBF\xBD");
}

TEST_CASE("to_utf8 replaces a high surrogate followed by something else", "[model][text]") {
    const std::wstring bad{L'\xD83D', L'A'};
    CHECK(to_utf8(bad) == "\xEF\xBF\xBD\x41");
}

TEST_CASE("to_utf8 replaces an unpaired low surrogate", "[model][text]") {
    const std::wstring lone{L'\xDE00'};
    CHECK(to_utf8(lone) == "\xEF\xBF\xBD");
}

TEST_CASE("to_utf8 keeps the first code point past the surrogates", "[model][text]") {
    const std::wstring private_use{L'\xE000'};
    CHECK(to_utf8(private_use) == "\xEE\x80\x80");
}

TEST_CASE("to_utf8 handles an empty string", "[model][text]") {
    CHECK(to_utf8(L"").empty());
}

TEST_CASE("to_wide passes ASCII through", "[model][text]") {
    CHECK(to_wide("Ubuntu") == L"Ubuntu");
}

TEST_CASE("to_wide decodes each width", "[model][text]") {
    CHECK(to_wide("A\xC3\xA9\xE4\xB8\xAD") == L"Aé中");
}

TEST_CASE("to_wide builds a surrogate pair for a supplementary code point", "[model][text]") {
    const std::wstring expected{L'\xD83D', L'\xDE00'};
    CHECK(to_wide("\xF0\x9F\x98\x80") == expected);
}

TEST_CASE("to_wide replaces a stray continuation byte", "[model][text]") {
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\x80") == expected);
}

TEST_CASE("to_wide replaces a truncated sequence", "[model][text]") {
    // A three-byte lead with only one byte after it.
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\xE4\xB8") == expected);
}

TEST_CASE("to_wide replaces a sequence with a bad continuation", "[model][text]") {
    // Full length, so this is not the truncated path: the second byte is a
    // valid continuation and the third is not. It resynchronises on the
    // offending byte rather than swallowing it, so both 'A's survive.
    const std::wstring expected{L'\xFFFD', L'A', L'A'};
    CHECK(to_wide("\xE4\xB8\x41\x41") == expected);
}

TEST_CASE("to_wide rejects an over-long encoding", "[model][text]") {
    // C0 80 is a two-byte encoding of NUL: the classic way to smuggle a
    // terminator past a naive check.
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\xC0\x80") == expected);
}

TEST_CASE("to_wide rejects an over-long three-byte encoding", "[model][text]") {
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\xE0\x80\x80") == expected);
}

TEST_CASE("to_wide rejects an over-long four-byte encoding", "[model][text]") {
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\xF0\x80\x80\x80") == expected);
}

TEST_CASE("to_wide rejects an encoded surrogate half", "[model][text]") {
    // ED A0 80 is U+D800, which is not a character and must not become one.
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\xED\xA0\x80") == expected);
}

TEST_CASE("to_wide rejects a code point past the last one", "[model][text]") {
    // F4 90 80 80 is U+110000.
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\xF4\x90\x80\x80") == expected);
}

TEST_CASE("to_wide rejects a byte that is not a lead at all", "[model][text]") {
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\xFF") == expected);
}

TEST_CASE("to_wide handles an empty string", "[model][text]") {
    CHECK(to_wide("").empty());
}

TEST_CASE("a name survives a round trip in both directions", "[model][text]") {
    const std::wstring original = L"Ubuntu-テスト-24.04";
    CHECK(to_wide(to_utf8(original)) == original);
}

TEST_CASE("to_wide replaces a bare lead byte at the end", "[model][text]") {
    // The truncated-sequence path with no continuation bytes to consume.
    const std::wstring expected{L'\xFFFD'};
    CHECK(to_wide("\xE4") == expected);
}

TEST_CASE("to_wide keeps text after a truncated sequence", "[model][text]") {
    // Truncation ends the sequence, not the string: 'A' is a lead byte rather
    // than a continuation, so it is not consumed with the replacement.
    const std::wstring expected{L'\xFFFD', L'A'};
    CHECK(to_wide("\xE4\x41") == expected);
}

TEST_CASE("path_to_utf8 does not go through the active code page", "[model][text]") {
    // `std::filesystem::path::string()` converts through the ACP on MSVC. On a
    // 932 machine that produces Shift-JIS bytes, which nlohmann's dump() rejects
    // as invalid UTF-8; on 1252 an unmappable character silently becomes `?`,
    // giving a path that parses and names a file that does not exist.
    const std::filesystem::path japanese = LR"(C:\Users\example\テスト\ext4.vhdx)";

    const std::string utf8 = wsldisk::model::path_to_utf8(japanese);

    // The three kana as UTF-8 bytes, whatever this machine's code page is.
    CHECK(utf8.find("\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88") != std::string::npos);
    CHECK(utf8 == wsldisk::model::to_utf8(japanese.wstring()));
}

TEST_CASE("path_to_utf8 round-trips back to the same path", "[model][text]") {
    const std::filesystem::path original = LR"(D:\wsl\Übuntu\ext4.vhdx)";

    CHECK(std::filesystem::path{wsldisk::model::to_wide(wsldisk::model::path_to_utf8(original))} == original);
}
