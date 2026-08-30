#include "wsl_output.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "text.h"

namespace wsldisk::model {
namespace {

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

    // A trailing odd byte is dropped rather than half-decoded: a pipe read can
    // end mid-character, and half a unit is not one.
    std::wstring units;
    units.reserve((bytes.size() - index) / 2);
    while (index + 1 < bytes.size()) {
        units.push_back(static_cast<wchar_t>(unit_at(bytes, index)));
        index += 2;
    }
    // Surrogate handling, including the unpaired cases this stream produces when
    // a read is truncated, lives in one place.
    return to_utf8(units);
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
