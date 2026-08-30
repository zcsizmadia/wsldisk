// Contract test: enumerate the real Lxss key, read-only.
//
// Nothing here writes to the registry and nothing assumes a distribution is
// installed -- a bare runner has none, and that is a valid answer. What is
// asserted is that whatever the machine actually has comes back well-formed,
// which is the part a fake cannot prove.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "model/distro.h"
#include "platform/registry.h"

using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::platform::Win32Registry;

TEST_CASE("enumerating the real Lxss key produces well-formed distributions", "[contract][distro]") {
    const Win32Registry registry;
    const auto list = enumerate(registry);

    // A machine with no WSL has no Lxss key at all, which is a failure to read
    // rather than an empty list. Both are acceptable here; a crash is not.
    if (!list.has_value()) {
        CHECK_FALSE(list.error().message.empty());
        return;
    }

    for (const Distro& distro : list->distros) {
        INFO("distribution " << distro.name);
        CHECK_FALSE(distro.name.empty());
        CHECK_FALSE(distro.guid.empty());
        CHECK_FALSE(distro.base_path.empty());
        CHECK_FALSE(distro.vhdx_path.empty());

        // Version is 1 or 2 and nothing else; the preflight that refuses WSL1
        // relies on that.
        CHECK((distro.version == 1 || distro.version == 2));

        // The resolved path is absolute and free of the extended-length prefix,
        // which is what makes it safe to hand to std::filesystem.
        CHECK(distro.vhdx_path.is_absolute());
        CHECK_FALSE(distro.vhdx_path.wstring().starts_with(LR"(\\?\)"));
        CHECK(distro.vhdx_path.has_filename());

        // The stored form is kept verbatim, prefix and all, because `relink`
        // and `move` write back whichever form they found.
        CHECK(distro.base_path.find(distro.vhdx_path.parent_path().filename().wstring()) !=
              std::wstring::npos);
    }

    // At most one default, so callers can take the first without checking.
    const auto defaults = std::ranges::count_if(list->distros, [](const Distro& d) { return d.is_default; });
    CHECK(defaults <= 1);

    // Names are unique per GUID; two distributions may share a name, but not a
    // GUID, and `orphans` keys on the GUID.
    for (std::size_t left = 0; left < list->distros.size(); ++left) {
        for (std::size_t right = left + 1; right < list->distros.size(); ++right) {
            CHECK(list->distros[left].guid != list->distros[right].guid);
        }
    }
}

TEST_CASE("a machine with distributions has a readable disk path for each", "[contract][distro]") {
    const Win32Registry registry;
    const auto list = enumerate(registry);
    if (!list.has_value() || list->distros.empty()) {
        SUCCEED("no distributions registered on this machine");
        return;
    }

    // Not that the file exists -- a dangling entry is exactly what `orphans`
    // reports -- but that the path is one the filesystem layer can be asked
    // about without throwing.
    for (const Distro& distro : list->distros) {
        INFO("distribution " << distro.name);
        std::error_code ignored;
        std::ignore = std::filesystem::exists(distro.vhdx_path, ignored);
        CHECK(distro.vhdx_path.extension() == ".vhdx");
    }
}
