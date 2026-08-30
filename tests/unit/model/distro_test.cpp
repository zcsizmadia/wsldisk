#include "model/distro.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "errors.h"
#include "fake_registry.h"
#include "lxss_hives.h"

using wsldisk::ErrorCode;
using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::model::strip_extended_prefix;
using wsldisk::model::vhdx_path_for;
using wsldisk::testing::FakeRegistry;
namespace hives = wsldisk::testing::hives;

TEST_CASE("enumerate reads the layouts spike 4 measured", "[model][distro]") {
    const FakeRegistry registry = hives::measured();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    REQUIRE(list->distros.size() == 2);
    CHECK(list->warnings.empty());

    const Distro* ubuntu = list->find("Ubuntu");
    REQUIRE(ubuntu != nullptr);
    CHECK(ubuntu->version == 2);
    CHECK(ubuntu->is_default);
    CHECK(ubuntu->modern);
    CHECK(ubuntu->flavor == "ubuntu");
    CHECK(ubuntu->os_version == "24.04");
    CHECK(ubuntu->vhdx_path.filename() == "ext4.vhdx");
}

TEST_CASE("the default distribution is the one DefaultDistribution names", "[model][distro]") {
    const FakeRegistry registry = hives::measured();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    const Distro* fallback = list->default_distro();
    REQUIRE(fallback != nullptr);
    CHECK(fallback->name == "Ubuntu");
    // Exactly one, so a caller can rely on it rather than checking.
    const auto defaults = std::ranges::count_if(list->distros, [](const Distro& d) { return d.is_default; });
    CHECK(defaults == 1);
}

TEST_CASE("an extended-length BasePath is stored as found and resolved for use", "[model][distro]") {
    // The prefix form varies per distribution on one machine, so `move` and
    // `relink` have to write back whichever form they read.
    const FakeRegistry registry = hives::measured();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    const Distro* docker = list->find("docker-desktop");
    REQUIRE(docker != nullptr);
    CHECK(docker->base_path.starts_with(LR"(\\?\)"));
    CHECK_FALSE(docker->vhdx_path.wstring().starts_with(LR"(\\?\)"));
    CHECK(docker->vhdx_path.filename() == "ext4.vhdx");
}

TEST_CASE("a distribution with no VhdFileName falls back to ext4.vhdx", "[model][distro]") {
    // The legacy MSIX layout has no such value at all.
    const FakeRegistry registry = hives::everything();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    const Distro* legacy = list->find("Ubuntu-20.04");
    REQUIRE(legacy != nullptr);
    CHECK(legacy->vhdx_path.filename() == "ext4.vhdx");
    CHECK_FALSE(legacy->modern);
}

TEST_CASE("a WSL1 distribution enumerates rather than being hidden", "[model][distro]") {
    // `list` shows it; every other command refuses it in a shared preflight.
    const FakeRegistry registry = hives::everything();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    const Distro* legacy = list->find("Legacy-WSL1");
    REQUIRE(legacy != nullptr);
    CHECK(legacy->version == 1);
    CHECK_FALSE(legacy->is_wsl2());
}

TEST_CASE("a key with no DistributionName is skipped with a warning", "[model][distro]") {
    // One unusable key must not stop the command reporting the rest.
    const FakeRegistry registry = hives::everything();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    REQUIRE(list->warnings.size() == 1);
    CHECK(list->warnings[0].find("DistributionName") != std::string::npos);
    for (const Distro& distro : list->distros) {
        CHECK_FALSE(distro.name.empty());
    }
}

TEST_CASE("a distribution whose disk is gone still enumerates", "[model][distro]") {
    // Nothing here touches the filesystem; `orphans` is what notices.
    const FakeRegistry registry = hives::everything();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    const Distro* moved = list->find("Moved-Away");
    REQUIRE(moved != nullptr);
    CHECK(moved->vhdx_path.wstring().starts_with(L"D:"));
}

TEST_CASE("a key with no BasePath is skipped with a warning", "[model][distro]") {
    FakeRegistry registry = hives::measured();
    registry.add_key(std::wstring{hives::lxss} + L"\\{deadbeef-0000-0000-0000-000000000000}");
    registry.set(std::wstring{hives::lxss} + L"\\{deadbeef-0000-0000-0000-000000000000}", L"DistributionName",
                 std::wstring{L"No-Path"});

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    REQUIRE(list->warnings.size() == 1);
    CHECK(list->warnings[0].find("No-Path") != std::string::npos);
    CHECK(list->find("No-Path") == nullptr);
}

TEST_CASE("no DefaultDistribution value means no default", "[model][distro]") {
    const FakeRegistry registry = hives::without_default();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    CHECK_FALSE(list->distros.empty());
    CHECK(list->default_distro() == nullptr);
}

TEST_CASE("a dangling DefaultDistribution marks nothing", "[model][distro]") {
    FakeRegistry registry = hives::measured();
    registry.set(hives::lxss, L"DefaultDistribution",
                 std::wstring{L"{ffffffff-ffff-ffff-ffff-ffffffffffff}"});

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    CHECK(list->default_distro() == nullptr);
}

TEST_CASE("find matches a name the way wsl.exe does", "[model][distro]") {
    const FakeRegistry registry = hives::measured();

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    CHECK(list->find("ubuntu") != nullptr);
    CHECK(list->find("UBUNTU") != nullptr);
    CHECK(list->find("Ubunt") == nullptr);
    CHECK(list->find("") == nullptr);
}

TEST_CASE("enumerate reports a registry it cannot read", "[model][distro]") {
    FakeRegistry registry = hives::measured();
    registry.fail_with(wsldisk::Error{ErrorCode::Preflight, "the hive is gone", "check WSL is installed"});

    const auto list = enumerate(registry);

    REQUIRE_FALSE(list.has_value());
    CHECK(list.error().code == ErrorCode::Preflight);
}

TEST_CASE("an empty Lxss key enumerates to nothing", "[model][distro]") {
    FakeRegistry registry;
    registry.add_key(hives::lxss);

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    CHECK(list->distros.empty());
    CHECK(list->warnings.empty());
    CHECK(list->default_distro() == nullptr);
}

TEST_CASE("a non-ASCII distribution name survives the conversion", "[model][distro]") {
    FakeRegistry registry;
    registry.add_key(hives::lxss);
    const std::wstring key = std::wstring{hives::lxss} + L"\\{11111111-0000-0000-0000-000000000001}";
    registry.add_key(key);
    registry.set(key, L"DistributionName", std::wstring{L"\u30C6\u30B9\u30C8"});
    registry.set(key, L"BasePath", std::wstring{LR"(C:\wsl\test)"});

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    REQUIRE(list->distros.size() == 1);
    CHECK(list->distros[0].name == "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88");
}

TEST_CASE("strip_extended_prefix leaves an ordinary path alone", "[model][distro]") {
    CHECK(strip_extended_prefix(LR"(C:\wsl\ext4.vhdx)") == LR"(C:\wsl\ext4.vhdx)");
}

TEST_CASE("strip_extended_prefix removes the local-device prefix", "[model][distro]") {
    CHECK(strip_extended_prefix(LR"(\\?\C:\wsl)") == LR"(C:\wsl)");
}

TEST_CASE("strip_extended_prefix turns the UNC form back into a UNC path", "[model][distro]") {
    // Stripping only `\\?\` would leave `UNC\server\share`, which resolves to
    // nothing at all.
    CHECK(strip_extended_prefix(LR"(\\?\UNC\server\share\wsl)") == LR"(\\server\share\wsl)");
}

TEST_CASE("strip_extended_prefix handles an empty path", "[model][distro]") {
    CHECK(strip_extended_prefix(L"").empty());
}

TEST_CASE("vhdx_path_for defaults the file name", "[model][distro]") {
    CHECK(vhdx_path_for(LR"(C:\wsl)", L"") == std::filesystem::path{LR"(C:\wsl\ext4.vhdx)"});
}

TEST_CASE("vhdx_path_for uses the name it was given", "[model][distro]") {
    CHECK(vhdx_path_for(LR"(C:\wsl)", L"custom.vhdx") == std::filesystem::path{LR"(C:\wsl\custom.vhdx)"});
}

TEST_CASE("enumerate reports a string value it cannot read", "[model][distro]") {
    FakeRegistry registry = hives::measured();
    registry.fail_value(L"DistributionName",
                        wsldisk::Error{ErrorCode::Generic, "the value is corrupt", "run `wsl --list`"});

    const auto list = enumerate(registry);

    REQUIRE_FALSE(list.has_value());
    CHECK(list.error().message.find("corrupt") != std::string::npos);
}

TEST_CASE("enumerate reports a number value it cannot read", "[model][distro]") {
    // Reached only because the string reads before it succeed, which is the
    // point of failing one named value rather than all of them.
    FakeRegistry registry = hives::measured();
    registry.fail_value(L"Version",
                        wsldisk::Error{ErrorCode::Generic, "the value is corrupt", "run `wsl --list`"});

    const auto list = enumerate(registry);

    REQUIRE_FALSE(list.has_value());
    CHECK(list.error().message.find("corrupt") != std::string::npos);
}

TEST_CASE("a failed read stops the reads after it", "[model][distro]") {
    // Every later read is a no-op once one has failed, so the error reported is
    // the first one rather than the last.
    FakeRegistry registry = hives::measured();
    registry.fail_value(L"BasePath", wsldisk::Error{ErrorCode::Generic, "first failure", "."});
    registry.fail_value(L"State", wsldisk::Error{ErrorCode::Generic, "later failure", "."});

    const auto list = enumerate(registry);

    REQUIRE_FALSE(list.has_value());
    CHECK(list.error().message == "first failure");
}

TEST_CASE("enumerate reports a DefaultDistribution it cannot read", "[model][distro]") {
    // Distinct from the value being absent, which is normal and not an error.
    FakeRegistry registry = hives::measured();
    registry.fail_value(L"DefaultDistribution",
                        wsldisk::Error{ErrorCode::Generic, "the value is corrupt", "run `wsl --list`"});

    const auto list = enumerate(registry);

    REQUIRE_FALSE(list.has_value());
    CHECK(list.error().message.find("corrupt") != std::string::npos);
}

TEST_CASE("an empty DistributionName is treated as absent", "[model][distro]") {
    // Found by the fuzzer: a value can be present and blank -- an interrupted
    // install writes the key before the name -- and a distribution with an
    // empty name is one `wsl.exe` cannot be asked about.
    FakeRegistry registry;
    registry.add_key(hives::lxss);
    const std::wstring key = std::wstring{hives::lxss} + L"\\{22222222-0000-0000-0000-000000000002}";
    registry.set(key, L"DistributionName", std::wstring{});
    registry.set(key, L"BasePath", std::wstring{LR"(C:\wsl\blank)"});

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    CHECK(list->distros.empty());
    REQUIRE(list->warnings.size() == 1);
    CHECK(list->warnings[0].find("DistributionName") != std::string::npos);
}

TEST_CASE("an empty BasePath is treated as absent", "[model][distro]") {
    // Otherwise the disk resolves to a bare `ext4.vhdx`, relative to whatever
    // the process's current directory happens to be.
    FakeRegistry registry;
    registry.add_key(hives::lxss);
    const std::wstring key = std::wstring{hives::lxss} + L"\\{33333333-0000-0000-0000-000000000003}";
    registry.set(key, L"DistributionName", std::wstring{L"Blank-Path"});
    registry.set(key, L"BasePath", std::wstring{});

    const auto list = enumerate(registry);

    REQUIRE(list.has_value());
    CHECK(list->distros.empty());
    REQUIRE(list->warnings.size() == 1);
    CHECK(list->warnings[0].find("Blank-Path") != std::string::npos);
}
