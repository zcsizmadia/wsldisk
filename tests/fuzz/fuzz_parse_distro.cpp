// Fuzzes distribution enumeration over arbitrary registry values.
//
// The registry is user-writable and outlives the software that wrote it, so
// BasePath and VhdFileName are effectively untrusted input: a half-written key
// from an interrupted install, a path someone edited by hand, a name from a
// distribution imported on another machine. Enumeration has to survive all of
// it, because `list` is the first thing anyone runs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fake_registry.h"
#include "model/distro.h"
#include "model/text.h"

namespace {

/// Splits the input into up to `count` pieces, so one corpus entry can drive
/// several registry values at once.
std::vector<std::string_view> split(std::string_view input, std::size_t count) {
    std::vector<std::string_view> parts;
    if (count == 0) {
        return parts;
    }
    const std::size_t each = (input.size() / count) + 1;
    for (std::size_t at = 0; at < count; ++at) {
        const std::size_t start = std::min(at * each, input.size());
        parts.push_back(input.substr(start, std::min(each, input.size() - start)));
    }
    return parts;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    const auto parts = split(input, 4);

    wsldisk::testing::FakeRegistry registry;
    registry.add_key(L"Lxss");

    const std::wstring guid = L"{00000000-0000-0000-0000-000000000001}";
    const std::wstring key = L"Lxss\\" + guid;
    registry.add_key(key);
    registry.set(key, L"DistributionName", wsldisk::model::to_wide(parts[0]));
    registry.set(key, L"BasePath", wsldisk::model::to_wide(parts[1]));
    registry.set(key, L"VhdFileName", wsldisk::model::to_wide(parts[2]));
    registry.set(L"Lxss", L"DefaultDistribution", wsldisk::model::to_wide(parts[3]));

    const auto list = wsldisk::model::enumerate(registry);
    if (!list.has_value()) {
        // The fake never fails, so enumeration must not either.
        __builtin_trap();
    }

    for (const auto& distro : list->distros) {
        // A distribution that enumerated must be usable: it has a name, and a
        // path that can be handed to the filesystem layer.
        if (distro.name.empty()) {
            __builtin_trap();
        }
        if (distro.vhdx_path.empty()) {
            __builtin_trap();
        }
        // The stored form is preserved verbatim, which `relink` depends on.
        if (distro.base_path.empty()) {
            __builtin_trap();
        }
    }
    return 0;
}
