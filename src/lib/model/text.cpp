#include "text.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace wsldisk::model {
namespace {

constexpr char32_t replacement_character = 0xFFFD;
constexpr char32_t high_surrogate_first = 0xD800;
constexpr char32_t high_surrogate_last = 0xDBFF;
constexpr char32_t low_surrogate_first = 0xDC00;
constexpr char32_t low_surrogate_last = 0xDFFF;
constexpr char32_t supplementary_base = 0x10000;
constexpr char32_t max_code_point = 0x10FFFF;

void append_utf8(std::string& text, char32_t code_point) {
    if (code_point < 0x80) {
        text.push_back(static_cast<char>(code_point));
        return;
    }
    if (code_point < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        return;
    }
    if (code_point < supplementary_base) {
        text.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        text.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        return;
    }
    text.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    text.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
}

void append_utf16(std::wstring& text, char32_t code_point) {
    if (code_point < supplementary_base) {
        text.push_back(static_cast<wchar_t>(code_point));
        return;
    }
    const char32_t offset = code_point - supplementary_base;
    text.push_back(static_cast<wchar_t>(high_surrogate_first + (offset >> 10)));
    text.push_back(static_cast<wchar_t>(low_surrogate_first + (offset & 0x3FF)));
}

/// How many bytes the lead byte of a UTF-8 sequence claims, or zero when it is
/// not a lead byte at all.
[[nodiscard]] std::size_t sequence_length(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 0;
}

/// One decoded UTF-8 sequence: the code point, and how many bytes to advance.
///
/// On malformed input the code point is U+FFFD and `consumed` is the maximal
/// well-formed subpart, which is what makes the caller resynchronise on the byte
/// that broke the sequence rather than swallowing it.
struct Decoded {
    char32_t code_point = replacement_character;
    std::size_t consumed = 1;
};

/// How many bytes at `index` are valid continuations of a truncated sequence.
[[nodiscard]] std::size_t continuation_run(std::string_view text, std::size_t index) {
    std::size_t consumed = 1;
    while (index + consumed < text.size() &&
           (static_cast<unsigned char>(text[index + consumed]) & 0xC0) == 0x80) {
        ++consumed;
    }
    return consumed;
}

[[nodiscard]] Decoded decode_one(std::string_view text, std::size_t index) {
    const auto lead = static_cast<unsigned char>(text[index]);
    const std::size_t length = sequence_length(lead);

    // Not a lead byte at all: one replacement, move on by one.
    if (length == 0) {
        return Decoded{};
    }
    // A lead claiming more bytes than are left.
    if (index + length > text.size()) {
        return Decoded{.consumed = continuation_run(text, index)};
    }
    if (length == 1) {
        return Decoded{.code_point = lead, .consumed = 1};
    }

    constexpr std::array<unsigned char, 5> lead_mask{0, 0x7F, 0x1F, 0x0F, 0x07};
    char32_t code_point = lead & lead_mask.at(length);
    std::size_t consumed = 1;
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if ((continuation & 0xC0) != 0x80) {
            break;
        }
        code_point = (code_point << 6) | (continuation & 0x3F);
        ++consumed;
    }
    if (consumed != length) {
        return Decoded{.consumed = consumed};
    }

    // An over-long encoding, a surrogate half, or a value past the last code
    // point: all things a decoder must not pass through.
    const bool overlong = (length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800) ||
                          (length == 4 && code_point < supplementary_base);
    const bool surrogate = code_point >= high_surrogate_first && code_point <= low_surrogate_last;
    if (overlong || surrogate || code_point > max_code_point) {
        return Decoded{.consumed = length};
    }
    return Decoded{.code_point = code_point, .consumed = length};
}

}  // namespace

std::string to_utf8(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto unit = static_cast<char32_t>(static_cast<std::uint16_t>(text[index]));

        if (unit >= high_surrogate_first && unit <= high_surrogate_last) {
            if (index + 1 < text.size()) {
                const auto low = static_cast<char32_t>(static_cast<std::uint16_t>(text[index + 1]));
                if (low >= low_surrogate_first && low <= low_surrogate_last) {
                    ++index;
                    append_utf8(result, supplementary_base + ((unit - high_surrogate_first) << 10) +
                                            (low - low_surrogate_first));
                    continue;
                }
            }
            append_utf8(result, replacement_character);
            continue;
        }

        if (unit >= low_surrogate_first && unit <= low_surrogate_last) {
            append_utf8(result, replacement_character);
            continue;
        }

        append_utf8(result, unit);
    }
    return result;
}

std::wstring to_wide(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());

    for (std::size_t index = 0; index < text.size();) {
        const Decoded decoded = decode_one(text, index);
        append_utf16(result, decoded.code_point);
        index += decoded.consumed;
    }
    return result;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    return to_utf8(path.wstring());
}

}  // namespace wsldisk::model
