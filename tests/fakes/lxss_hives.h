#pragma once

#include <string>

#include "fake_registry.h"
#include "model/distro.h"

/// Canned `Lxss` hives, reproducing the layouts measured in spike #4
/// (`docs/RESEARCH.md`, registry section) on a real machine running WSL 2.7.8
/// with Ubuntu, Docker Desktop and Rancher Desktop installed.
///
/// These exist so enumeration is tested against what WSL actually writes rather
/// than against what the plan assumed. The differences are not hypothetical: the
/// `BasePath` prefix form varies *per distribution on one machine*, `VhdFileName`
/// is absent on the legacy packaged layout, and `Flags` was 15 everywhere and so
/// cannot identify sparse mode.
namespace wsldisk::testing::hives {

/// Taken from the model rather than repeated here. The two were once
/// different literals, every unit test passed, and `list` could not find the
/// key on a real machine.
inline const std::wstring lxss{model::lxss_key()};

/// Ubuntu as `wsl --install` writes it today: a bare `BasePath` under
/// `%LOCALAPPDATA%\wsl\{GUID}`, `Modern=1`, and the guest OS recorded at import.
inline constexpr const wchar_t* modern_guid = L"{4d1297e9-bac4-4da1-9867-a2ab591e9581}";

/// Docker Desktop, whose `BasePath` is stored in the extended-length form.
/// `move` and `relink` have to write back whichever form they found.
inline constexpr const wchar_t* extended_guid = L"{6dae238e-9a7f-4941-8df8-2569b9d2d284}";

/// The MSIX layout: `Packages\<pkg>\LocalState`, no `VhdFileName`, no `Modern`.
inline constexpr const wchar_t* legacy_guid = L"{c8ea4246-c983-4794-9ecd-abdfb1199f39}";

/// A WSL1 distribution: enumerated by `list`, refused by everything else (D8).
inline constexpr const wchar_t* wsl1_guid = L"{f8e23f84-15de-4be6-b1e4-a1d3b3fcd8c4}";

/// A key whose `BasePath` points at a file that is not there -- what
/// `orphans --relink` exists to repair.
inline constexpr const wchar_t* dangling_guid = L"{0a0a0a0a-0000-0000-0000-00000000000a}";

/// A key with no `DistributionName` at all. Enumeration must skip it with a
/// warning rather than failing the whole command.
inline constexpr const wchar_t* nameless_guid = L"{0b0b0b0b-0000-0000-0000-00000000000b}";

namespace detail {

inline std::wstring path(const wchar_t* guid) {
    return std::wstring{lxss} + L"\\" + guid;
}

inline void add_modern(FakeRegistry& registry) {
    const std::wstring key = path(modern_guid);
    registry.set(key, L"DistributionName", std::wstring{L"Ubuntu"});
    registry.set(key, L"BasePath", std::wstring{LR"(C:\Users\example\AppData\Local\wsl\)"} + modern_guid);
    registry.set(key, L"VhdFileName", std::wstring{L"ext4.vhdx"});
    registry.set(key, L"Version", std::uint32_t{2});
    registry.set(key, L"DefaultUid", std::uint32_t{1000});
    registry.set(key, L"Flags", std::uint32_t{15});
    registry.set(key, L"State", std::uint32_t{1});
    registry.set(key, L"Modern", std::uint32_t{1});
    registry.set(key, L"Flavor", std::wstring{L"ubuntu"});
    registry.set(key, L"OsVersion", std::wstring{L"24.04"});
}

inline void add_extended(FakeRegistry& registry) {
    const std::wstring key = path(extended_guid);
    registry.set(key, L"DistributionName", std::wstring{L"docker-desktop"});
    registry.set(key, L"BasePath", std::wstring{LR"(\\?\C:\Users\example\AppData\Local\Docker\wsl\main)"});
    registry.set(key, L"VhdFileName", std::wstring{L"ext4.vhdx"});
    registry.set(key, L"Version", std::uint32_t{2});
    registry.set(key, L"DefaultUid", std::uint32_t{0});
    registry.set(key, L"Flags", std::uint32_t{15});
    registry.set(key, L"State", std::uint32_t{1});
    registry.set(key, L"Modern", std::uint32_t{1});
}

inline void add_legacy(FakeRegistry& registry) {
    const std::wstring key = path(legacy_guid);
    registry.set(key, L"DistributionName", std::wstring{L"Ubuntu-20.04"});
    registry.set(
        key, L"BasePath",
        std::wstring{
            LR"(C:\Users\example\AppData\Local\Packages\CanonicalGroupLimited.Ubuntu20.04LTS_79rhkp1fndgsc\LocalState)"});
    // No VhdFileName and no Modern: both are what mark this layout.
    registry.set(key, L"Version", std::uint32_t{2});
    registry.set(key, L"DefaultUid", std::uint32_t{1000});
    registry.set(key, L"Flags", std::uint32_t{15});
    registry.set(key, L"State", std::uint32_t{1});
}

inline void add_wsl1(FakeRegistry& registry) {
    const std::wstring key = path(wsl1_guid);
    registry.set(key, L"DistributionName", std::wstring{L"Legacy-WSL1"});
    registry.set(key, L"BasePath", std::wstring{LR"(C:\Users\example\AppData\Local\lxss)"});
    registry.set(key, L"Version", std::uint32_t{1});
    registry.set(key, L"DefaultUid", std::uint32_t{0});
    registry.set(key, L"Flags", std::uint32_t{7});
    registry.set(key, L"State", std::uint32_t{1});
}

inline void add_dangling(FakeRegistry& registry) {
    const std::wstring key = path(dangling_guid);
    registry.set(key, L"DistributionName", std::wstring{L"Moved-Away"});
    registry.set(key, L"BasePath", std::wstring{LR"(D:\gone\wsl\Moved-Away)"});
    registry.set(key, L"VhdFileName", std::wstring{L"ext4.vhdx"});
    registry.set(key, L"Version", std::uint32_t{2});
    registry.set(key, L"DefaultUid", std::uint32_t{1000});
    registry.set(key, L"Flags", std::uint32_t{15});
    registry.set(key, L"State", std::uint32_t{1});
}

inline void add_nameless(FakeRegistry& registry) {
    const std::wstring key = path(nameless_guid);
    registry.set(key, L"BasePath", std::wstring{LR"(C:\Users\example\AppData\Local\wsl\orphaned)"});
    registry.set(key, L"Version", std::uint32_t{2});
}

}  // namespace detail

/// The machine spike #4 was measured on: a modern distro, an extended-length
/// one, and a default marker pointing at the first.
inline FakeRegistry measured() {
    FakeRegistry registry;
    registry.add_key(lxss);
    detail::add_modern(registry);
    detail::add_extended(registry);
    registry.set(lxss, L"DefaultDistribution", std::wstring{modern_guid});
    registry.set(lxss, L"DefaultVersion", std::uint32_t{2});
    return registry;
}

/// Every layout at once, including the two broken keys. What enumeration has to
/// survive.
inline FakeRegistry everything() {
    FakeRegistry registry = measured();
    detail::add_legacy(registry);
    detail::add_wsl1(registry);
    detail::add_dangling(registry);
    detail::add_nameless(registry);
    return registry;
}

/// A registered distribution with no `DefaultDistribution` value -- possible
/// after the default is unregistered.
inline FakeRegistry without_default() {
    FakeRegistry registry;
    registry.add_key(lxss);
    detail::add_modern(registry);
    return registry;
}

/// `DefaultDistribution` naming a key that no longer exists.
inline FakeRegistry dangling_default() {
    FakeRegistry registry = without_default();
    registry.set(lxss, L"DefaultDistribution", std::wstring{L"{deadbeef-0000-0000-0000-000000000000}"});
    return registry;
}

/// WSL present but nothing installed.
inline FakeRegistry empty() {
    FakeRegistry registry;
    registry.add_key(lxss);
    return registry;
}

}  // namespace wsldisk::testing::hives
