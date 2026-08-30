// The fakes are load-bearing: every operation in M1 is tested against them, so a
// fake that quietly disagrees with the real implementation would hide bugs
// rather than find them. These tests pin the behaviour the production
// Win32Registry has, and the hive fixtures against the layouts spike #4 measured.

#include "fake_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "lxss_hives.h"

using wsldisk::ErrorCode;
using wsldisk::testing::FakeRegistry;
namespace hives = wsldisk::testing::hives;

namespace {

bool contains(const std::vector<std::wstring>& names, const wchar_t* wanted) {
    return std::ranges::find(names, std::wstring{wanted}) != names.end();
}

}  // namespace

TEST_CASE("the fake enumerates direct children only", "[fakes][registry]") {
    FakeRegistry registry;
    registry.set(L"Lxss\\{a}", L"DistributionName", std::wstring{L"A"});
    registry.set(L"Lxss\\{a}\\Nested", L"Value", std::wstring{L"x"});
    registry.set(L"Lxss\\{b}", L"DistributionName", std::wstring{L"B"});
    registry.set(L"Other\\{c}", L"DistributionName", std::wstring{L"C"});

    const auto names = registry.subkeys(L"Lxss");

    REQUIRE(names.has_value());
    CHECK(names->size() == 2);
    CHECK(contains(*names, L"{a}"));
    CHECK(contains(*names, L"{b}"));
}

TEST_CASE("the fake distinguishes a missing value from a missing key", "[fakes][registry]") {
    FakeRegistry registry;
    registry.set(L"Lxss\\{a}", L"DistributionName", std::wstring{L"A"});

    // Present key, absent value: nullopt, like the real one.
    const auto absent = registry.read_string(L"Lxss\\{a}", L"VhdFileName");
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->has_value());

    // Absent key: an error.
    CHECK_FALSE(registry.read_string(L"Lxss\\{nope}", L"DistributionName").has_value());
}

TEST_CASE("the fake reports a type mismatch like the real one", "[fakes][registry]") {
    FakeRegistry registry;
    registry.set(L"Lxss\\{a}", L"Version", std::uint32_t{2});

    CHECK_FALSE(registry.read_string(L"Lxss\\{a}", L"Version").has_value());
    CHECK(registry.read_dword(L"Lxss\\{a}", L"Version").has_value());
}

TEST_CASE("the fake can be told to fail every call", "[fakes][registry]") {
    FakeRegistry registry;
    registry.set(L"Lxss\\{a}", L"DistributionName", std::wstring{L"A"});
    registry.fail_with(wsldisk::Error{ErrorCode::NeedsElevation, "denied", "elevate"});

    CHECK_FALSE(registry.subkeys(L"Lxss").has_value());
    CHECK_FALSE(registry.read_string(L"Lxss\\{a}", L"DistributionName").has_value());
    CHECK_FALSE(registry.read_dword(L"Lxss\\{a}", L"Version").has_value());
    CHECK_FALSE(registry.write_string(L"Lxss\\{a}", L"BasePath", L"D:\\x").has_value());
}

TEST_CASE("the fake records writes so a rollback can be asserted", "[fakes][registry]") {
    FakeRegistry registry;
    registry.set(L"Lxss\\{a}", L"BasePath", std::wstring{L"C:\\old"});

    REQUIRE(registry.write_string(L"Lxss\\{a}", L"BasePath", L"D:\\new").has_value());
    REQUIRE(registry.write_string(L"Lxss\\{a}", L"BasePath", L"C:\\old").has_value());

    REQUIRE(registry.writes().size() == 2);
    CHECK(registry.writes()[0].data == L"D:\\new");
    CHECK(registry.writes()[1].data == L"C:\\old");

    const auto now = registry.read_string(L"Lxss\\{a}", L"BasePath");
    REQUIRE(now.has_value());
    CHECK(**now == L"C:\\old");
}

TEST_CASE("the measured hive reproduces what spike #4 found", "[fakes][hives]") {
    const FakeRegistry registry = hives::measured();

    const auto names = registry.subkeys(hives::lxss);
    REQUIRE(names.has_value());
    CHECK(names->size() == 2);

    SECTION("the modern layout has a bare BasePath and the guest OS recorded") {
        const std::wstring key = std::wstring{hives::lxss} + L"\\" + hives::modern_guid;
        const auto base = registry.read_string(key, L"BasePath");
        REQUIRE(base.has_value());
        REQUIRE(base->has_value());
        CHECK_FALSE((*base)->starts_with(LR"(\\?\)"));
        CHECK((*base)->find(LR"(\AppData\Local\wsl\)") != std::wstring::npos);

        const auto modern = registry.read_dword(key, L"Modern");
        REQUIRE(modern.has_value());
        CHECK(**modern == 1);

        const auto flavor = registry.read_string(key, L"Flavor");
        REQUIRE(flavor.has_value());
        CHECK(**flavor == L"ubuntu");
    }

    SECTION("the extended-length layout keeps its prefix") {
        const std::wstring key = std::wstring{hives::lxss} + L"\\" + hives::extended_guid;
        const auto base = registry.read_string(key, L"BasePath");
        REQUIRE(base.has_value());
        REQUIRE(base->has_value());
        // The whole point: two distributions on one machine, two prefix forms.
        CHECK((*base)->starts_with(LR"(\\?\)"));
    }

    SECTION("the default marker names the modern distribution") {
        const auto def = registry.read_string(hives::lxss, L"DefaultDistribution");
        REQUIRE(def.has_value());
        CHECK(**def == hives::modern_guid);
    }
}

TEST_CASE("the legacy packaged layout has no VhdFileName and no Modern", "[fakes][hives]") {
    const FakeRegistry registry = hives::everything();
    const std::wstring key = std::wstring{hives::lxss} + L"\\" + hives::legacy_guid;

    const auto vhd = registry.read_string(key, L"VhdFileName");
    REQUIRE(vhd.has_value());
    CHECK_FALSE(vhd->has_value());

    const auto modern = registry.read_dword(key, L"Modern");
    REQUIRE(modern.has_value());
    CHECK_FALSE(modern->has_value());

    const auto base = registry.read_string(key, L"BasePath");
    REQUIRE(base.has_value());
    CHECK((*base)->find(L"\\Packages\\") != std::wstring::npos);
}

TEST_CASE("Flags is 15 on every healthy distribution, so it cannot mark sparse", "[fakes][hives]") {
    // Spike #4: this is why `list` reads sparseness from the file attributes.
    const FakeRegistry registry = hives::everything();
    for (const wchar_t* guid : {hives::modern_guid, hives::extended_guid, hives::legacy_guid}) {
        const std::wstring key = std::wstring{hives::lxss} + L"\\" + guid;
        const auto flags = registry.read_dword(key, L"Flags");
        REQUIRE(flags.has_value());
        CHECK(**flags == 15);
    }
}

TEST_CASE("the everything hive carries both broken keys", "[fakes][hives]") {
    const FakeRegistry registry = hives::everything();

    SECTION("one distribution points at a file that is not there") {
        const std::wstring key = std::wstring{hives::lxss} + L"\\" + hives::dangling_guid;
        const auto base = registry.read_string(key, L"BasePath");
        REQUIRE(base.has_value());
        CHECK((*base)->starts_with(L"D:\\gone"));
    }

    SECTION("one key has no DistributionName at all") {
        const std::wstring key = std::wstring{hives::lxss} + L"\\" + hives::nameless_guid;
        const auto name = registry.read_string(key, L"DistributionName");
        REQUIRE(name.has_value());
        CHECK_FALSE(name->has_value());
    }

    SECTION("the WSL1 distribution is present and marked version 1") {
        const std::wstring key = std::wstring{hives::lxss} + L"\\" + hives::wsl1_guid;
        const auto version = registry.read_dword(key, L"Version");
        REQUIRE(version.has_value());
        CHECK(**version == 1);
    }
}

TEST_CASE("the default marker can be absent or dangling", "[fakes][hives]") {
    SECTION("absent") {
        const FakeRegistry registry = hives::without_default();
        const auto def = registry.read_string(hives::lxss, L"DefaultDistribution");
        REQUIRE(def.has_value());
        CHECK_FALSE(def->has_value());
    }

    SECTION("naming a key that no longer exists") {
        const FakeRegistry registry = hives::dangling_default();
        const auto def = registry.read_string(hives::lxss, L"DefaultDistribution");
        REQUIRE(def.has_value());
        REQUIRE(def->has_value());
        const auto names = registry.subkeys(hives::lxss);
        REQUIRE(names.has_value());
        CHECK_FALSE(contains(*names, (*def)->c_str()));
    }
}

TEST_CASE("an empty hive enumerates to nothing", "[fakes][hives]") {
    const FakeRegistry registry = hives::empty();
    const auto names = registry.subkeys(hives::lxss);
    REQUIRE(names.has_value());
    CHECK(names->empty());
}
