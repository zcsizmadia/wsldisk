#include "model/orphans.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "fake_filesystem.h"
#include "fake_registry.h"
#include "lxss_hives.h"
#include "model/distro.h"

using wsldisk::ErrorCode;
using wsldisk::model::canonical_path;
using wsldisk::model::default_scan_patterns;
using wsldisk::model::enumerate;
using wsldisk::model::expand_scan_pattern;
using wsldisk::model::find_orphans;
using wsldisk::model::Orphan;
using wsldisk::model::same_path;
using wsldisk::testing::FakeFileSystem;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

/// A filesystem with `%LOCALAPPDATA%` pointing where the canned hives put
/// their disks, so the default patterns land on the fixture tree.
FakeFileSystem local_appdata() {
    FakeFileSystem filesystem;
    filesystem.set_variable(L"LOCALAPPDATA", LR"(C:\Users\example\AppData\Local)");
    return filesystem;
}

void add_disk(FakeFileSystem& filesystem, const std::filesystem::path& path, std::uint64_t size) {
    FakeFileSystem::File file;
    file.size = size;
    file.size_on_disk = size;
    filesystem.add_file(path, file);
}

[[nodiscard]] std::vector<std::string> paths_of(const std::vector<Orphan>& orphans) {
    std::vector<std::string> names;
    names.reserve(orphans.size());
    for (const Orphan& orphan : orphans) {
        names.push_back(orphan.path.string());
    }
    return names;
}

}  // namespace

TEST_CASE("canonical_path reduces the spellings of one file to one", "[model][orphans]") {
    const std::wstring expected = LR"(c:\users\example\appdata\local\wsl\ext4.vhdx)";

    CHECK(canonical_path(LR"(C:\Users\example\AppData\Local\wsl\ext4.vhdx)") == expected);
    // The extended-length prefix: Docker Desktop stores BasePath this way and
    // Ubuntu does not, on the same machine (spike #4).
    CHECK(canonical_path(LR"(\\?\C:\Users\example\AppData\Local\wsl\ext4.vhdx)") == expected);
    CHECK(canonical_path(LR"(C:/Users/example/AppData/Local/wsl/ext4.vhdx)") == expected);
}

TEST_CASE("canonical_path drops trailing separators but keeps a bare root", "[model][orphans]") {
    CHECK(canonical_path(LR"(C:\wsl\)") == LR"(c:\wsl)");
    CHECK(canonical_path(LR"(C:\wsl\\)") == LR"(c:\wsl)");
    CHECK(canonical_path(LR"(C:/wsl/)") == LR"(c:\wsl)");
    // One character is left alone: trimming it would turn a root into an empty
    // string, which compares equal to everything else that trimmed away.
    CHECK(canonical_path(LR"(\)") == LR"(\)");
    CHECK(canonical_path(L"").empty());
}

TEST_CASE("same_path compares two spellings of the same disk", "[model][orphans]") {
    CHECK(same_path(LR"(\\?\C:\wsl\Ubuntu\ext4.vhdx)", LR"(C:\WSL\ubuntu\ext4.vhdx)"));
    CHECK_FALSE(same_path(LR"(C:\wsl\Ubuntu\ext4.vhdx)", LR"(C:\wsl\Debian\ext4.vhdx)"));
}

TEST_CASE("expand_scan_pattern returns a pattern with no wildcard unchanged", "[model][orphans]") {
    const FakeFileSystem filesystem;
    const auto directories = expand_scan_pattern(filesystem, LR"(D:\disks)");

    REQUIRE(directories.size() == 1);
    CHECK(directories.front() == std::filesystem::path{LR"(D:\disks)"});
}

TEST_CASE("expand_scan_pattern lists the subdirectories a trailing star names", "[model][orphans]") {
    FakeFileSystem filesystem;
    filesystem.add_directory(LR"(C:\wsl\Ubuntu)");
    filesystem.add_directory(LR"(C:\wsl\Debian)");
    add_disk(filesystem, LR"(C:\wsl\notes.txt)", 10);

    const auto directories = expand_scan_pattern(filesystem, LR"(C:\wsl\*)");

    REQUIRE(directories.size() == 2);
    // A file is not a directory to descend into.
    CHECK(std::ranges::find(directories, std::filesystem::path{LR"(C:\wsl\Debian)"}) != directories.end());
    CHECK(std::ranges::find(directories, std::filesystem::path{LR"(C:\wsl\Ubuntu)"}) != directories.end());
}

TEST_CASE("expand_scan_pattern keeps what follows the star", "[model][orphans]") {
    FakeFileSystem filesystem;
    filesystem.add_directory(LR"(C:\Packages\Ubuntu20.04)");

    const auto directories = expand_scan_pattern(filesystem, LR"(C:\Packages\*\LocalState)");

    REQUIRE(directories.size() == 1);
    CHECK(directories.front() == std::filesystem::path{LR"(C:\Packages\Ubuntu20.04\LocalState)"});
}

TEST_CASE("expand_scan_pattern expands to nothing when the root is unreadable", "[model][orphans]") {
    FakeFileSystem filesystem;
    filesystem.fail_queries(wsldisk::Error{ErrorCode::NeedsElevation, "denied", "run as the owning user"});

    CHECK(expand_scan_pattern(filesystem, LR"(C:\wsl\*)").empty());
}

TEST_CASE("default_scan_patterns covers the layouts that exist in the wild", "[model][orphans]") {
    const FakeFileSystem filesystem = local_appdata();

    const auto patterns = default_scan_patterns(filesystem);

    REQUIRE(patterns.has_value());
    REQUIRE(patterns->size() == 3);
    CHECK((*patterns)[0] == std::filesystem::path{LR"(C:\Users\example\AppData\Local\wsl\*)"});
    CHECK((*patterns)[1] ==
          std::filesystem::path{LR"(C:\Users\example\AppData\Local\Packages\*\LocalState)"});
    CHECK((*patterns)[2] == std::filesystem::path{LR"(C:\Users\example\AppData\Local\Docker\wsl\*)"});
}

TEST_CASE("default_scan_patterns reports an expansion it cannot do", "[model][orphans]") {
    FakeFileSystem filesystem;
    filesystem.fail_queries(wsldisk::Error{ErrorCode::Preflight, "no environment", "check the shell"});

    const auto patterns = default_scan_patterns(filesystem);

    REQUIRE_FALSE(patterns.has_value());
    CHECK(patterns.error().code == ErrorCode::Preflight);
}

TEST_CASE("find_orphans leaves the disks a distribution claims alone", "[model][orphans]") {
    auto registry = hives::measured();
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());

    FakeFileSystem filesystem = local_appdata();
    filesystem.add_directory(LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581})");
    add_disk(filesystem,
             LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)",
             14 * gigabyte);
    filesystem.add_directory(LR"(C:\Users\example\AppData\Local\Docker\wsl\main)");
    // Stored with the extended-length prefix in the registry and plainly here,
    // which is the case canonical_path exists for.
    add_disk(filesystem, LR"(C:\Users\example\AppData\Local\Docker\wsl\main\ext4.vhdx)", 2 * gigabyte);

    const auto patterns = default_scan_patterns(filesystem);
    REQUIRE(patterns.has_value());

    std::vector<std::string> warnings;
    const auto orphans = find_orphans(filesystem, *distros, *patterns, warnings);

    CHECK(orphans.empty());
    CHECK(warnings.empty());
}

TEST_CASE("find_orphans finds a disk no distribution claims", "[model][orphans]") {
    auto registry = hives::measured();
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());

    FakeFileSystem filesystem = local_appdata();
    filesystem.add_directory(LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581})");
    add_disk(filesystem,
             LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)",
             14 * gigabyte);
    filesystem.add_directory(LR"(C:\Users\example\AppData\Local\wsl\Unregistered)");
    add_disk(filesystem, LR"(C:\Users\example\AppData\Local\wsl\Unregistered\ext4.vhdx)", 3 * gigabyte);

    const auto patterns = default_scan_patterns(filesystem);
    REQUIRE(patterns.has_value());

    std::vector<std::string> warnings;
    const auto orphans = find_orphans(filesystem, *distros, *patterns, warnings);

    REQUIRE(orphans.size() == 1);
    CHECK(orphans.front().path ==
          std::filesystem::path{LR"(C:\Users\example\AppData\Local\wsl\Unregistered\ext4.vhdx)"});
    REQUIRE(orphans.front().size_on_disk.has_value());
    CHECK(*orphans.front().size_on_disk == 3 * gigabyte);
}

TEST_CASE("find_orphans reports a disk whose size it cannot read", "[model][orphans]") {
    auto registry = hives::empty();
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());

    FakeFileSystem filesystem;
    filesystem.add_directory(LR"(D:\disks)");
    add_disk(filesystem, LR"(D:\disks\ext4.vhdx)", gigabyte);
    // Listed by the scan and absent from the size lookup: a file deleted
    // between the two calls. It is still reported, without a size.
    filesystem.fail_size_on_disk(wsldisk::Error{ErrorCode::Preflight, "no such file", "re-run the scan"});

    std::vector<std::string> warnings;
    const auto orphans =
        find_orphans(filesystem, *distros, {std::filesystem::path{LR"(D:\disks)"}}, warnings);

    REQUIRE(orphans.size() == 1);
    CHECK_FALSE(orphans.front().size_on_disk.has_value());
}

TEST_CASE("find_orphans reports each disk once however many patterns reach it", "[model][orphans]") {
    auto registry = hives::empty();
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());

    FakeFileSystem filesystem;
    filesystem.add_directory(LR"(D:\disks)");
    add_disk(filesystem, LR"(D:\disks\ext4.vhdx)", gigabyte);

    const std::vector<std::filesystem::path> patterns{LR"(D:\disks)", LR"(D:\disks)"};

    std::vector<std::string> warnings;
    const auto orphans = find_orphans(filesystem, *distros, patterns, warnings);

    CHECK(orphans.size() == 1);
}

TEST_CASE("find_orphans ignores anything that is not a .vhdx", "[model][orphans]") {
    auto registry = hives::empty();
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());

    FakeFileSystem filesystem;
    filesystem.add_directory(LR"(D:\disks)");
    // The filesystem matches *.vhdx against the 8.3 short name too, so a backup
    // copy comes back from the scan and has to be dropped here.
    add_disk(filesystem, LR"(D:\disks\ext4.vhdx.bak)", gigabyte);
    // A directory named like a disk is not a disk.
    filesystem.add_directory(LR"(D:\disks\nested.vhdx)");

    std::vector<std::string> warnings;
    const auto orphans =
        find_orphans(filesystem, *distros, {std::filesystem::path{LR"(D:\disks)"}}, warnings);

    CHECK(orphans.empty());
}

TEST_CASE("find_orphans warns about a directory it cannot read and keeps going", "[model][orphans]") {
    auto registry = hives::empty();
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());

    FakeFileSystem filesystem;
    filesystem.fail_queries(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read D:\\disks", "run as the owning user"});

    std::vector<std::string> warnings;
    const auto orphans =
        find_orphans(filesystem, *distros, {std::filesystem::path{LR"(D:\disks)"}}, warnings);

    CHECK(orphans.empty());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("cannot read") != std::string::npos);
}

TEST_CASE("find_orphans does not treat a WSL1 directory as claimed", "[model][orphans]") {
    // A WSL1 distribution has no virtual disk, so a .vhdx sitting beside it is
    // not something it claims.
    auto registry = hives::everything();
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());

    FakeFileSystem filesystem;
    filesystem.add_directory(LR"(C:\Users\example\AppData\Local\lxss)");
    add_disk(filesystem, LR"(C:\Users\example\AppData\Local\lxss\leftover.vhdx)", gigabyte);

    std::vector<std::string> warnings;
    const auto orphans = find_orphans(
        filesystem, *distros, {std::filesystem::path{LR"(C:\Users\example\AppData\Local\lxss)"}}, warnings);

    CHECK(paths_of(orphans) ==
          std::vector<std::string>{R"(C:\Users\example\AppData\Local\lxss\leftover.vhdx)"});
}
