#include "wsl_output.h"

#include <cstdint>
#include <string_view>

namespace wsldisk::model {
namespace {

constexpr char32_t replacement_character = 0xFFFD;
constexpr char32_t high_surrogate_first = 0xD800;
constexpr char32_t high_surrogate_last = 0xDBFF;
constexpr char32_t low_surrogate_first = 0xDC00;
constexpr char32_t low_surrogate_last = 0xDFFF;
constexpr char32_t supplementary_base = 0x10000;

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

[[nodiscard]] char16_t unit_at(std::span<const std::byte> bytes, std::size_t index) {
    const auto low = static_cast<std::uint16_t>(bytes[index]);
    const auto high = static_cast<std::uint16_t>(bytes[index + 1]);
    return static_cast<char16_t>(low | static_cast<std::uint16_t>(high << 8));
}

/// Strips surrounding whitespace, and returns nothing for a line that is only
/// whitespace. NULs count as whitespace here: a read that ends mid-unit can
/// leave one behind, and a name made only of NULs is not a name.
[[nodiscard]] std::string_view trim(std::string_view line) {
    constexpr std::string_view trimmable{"\r\n\t\0 ", 5};
    const std::size_t first = line.find_first_not_of(trimmable);
    if (first == std::string_view::npos) {
        return {};
    }
    return line.substr(first, line.find_last_not_of(trimmable) - first + 1);
}

}  // namespace

std::string decode_utf16le(std::span<const std::byte> bytes) {
    std::size_t index = 0;
    // A BOM is optional and consumed when present, so it never reaches a name.
    if (bytes.size() >= 2 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0xFE}) {
        index = 2;
    }

    std::string text;
    while (index + 1 < bytes.size()) {
        const char16_t unit = unit_at(bytes, index);
        index += 2;

        if (unit >= high_surrogate_first && unit <= high_surrogate_last) {
            if (index + 1 < bytes.size()) {
                const char16_t low = unit_at(bytes, index);
                if (low >= low_surrogate_first && low <= low_surrogate_last) {
                    index += 2;
                    append_utf8(text, supplementary_base +
                                          ((static_cast<char32_t>(unit) - high_surrogate_first) << 10) +
                                          (static_cast<char32_t>(low) - low_surrogate_first));
                    continue;
                }
            }
            // A high surrogate with nothing valid after it: truncated read or
            // corrupt stream, either way not fatal.
            append_utf8(text, replacement_character);
            continue;
        }

        if (unit >= low_surrogate_first && unit <= low_surrogate_last) {
            append_utf8(text, replacement_character);
            continue;
        }

        append_utf8(text, unit);
    }
    return text;
}

std::vector<std::string> parse_distribution_list(std::span<const std::byte> bytes) {
    const std::string text = decode_utf16le(bytes);

    std::vector<std::string> names;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t line_end = text.find('\n', start);
        const std::size_t stop = line_end == std::string::npos ? text.size() : line_end;
        if (const std::string_view name = trim(std::string_view{text}.substr(start, stop - start));
            !name.empty()) {
            names.emplace_back(name);
        }
        start = line_end == std::string::npos ? text.size() : line_end + 1;
    }
    return names;
}

}  // namespace wsldisk::model
