// Fuzzes the `wsl --list --quiet` output decoder.
//
// The input is a byte stream off a pipe, so it is attacker-shaped by accident
// rather than by malice: truncated reads, missing byte-order marks, odd byte
// counts, lone surrogates and embedded NULs all occur in practice. The decoder
// has to survive every one of them, because the alternative is the tool
// crashing while listing distributions.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "model/wsl_output.h"

namespace {

/// Whether `text` is well-formed UTF-8. The decoder's whole job is producing
/// this, and a caller that hands malformed UTF-8 to a console or a JSON encoder
/// has a bug that only shows up on someone else's machine.
bool is_valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (lead < 0x80) {
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};

    const std::string text = wsldisk::model::decode_utf16le(bytes);
    if (!is_valid_utf8(text)) {
        __builtin_trap();
    }

    for (const std::vector<std::string> names = wsldisk::model::parse_distribution_list(bytes);
         const std::string& name : names) {
        // A blank name would mean the tool offering to compact "" -- the parser
        // drops those, and nothing downstream re-checks.
        if (name.empty()) {
            __builtin_trap();
        }
        if (!is_valid_utf8(name)) {
            __builtin_trap();
        }
    }
    return 0;
}
