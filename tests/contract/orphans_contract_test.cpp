// Contract tests for the orphan scan against the real filesystem: a real
// directory tree under %TEMP%, real wildcard expansion, and a real file held
// open by this process.
//
// The lock check is the one that has to be true rather than plausible.
// `orphans --delete` will not touch a file it reports as locked, and Docker
// Desktop's `docker_data.vhdx` -- which no distribution claims and which holds
// every volume the user has -- is exactly the file that has to come back
// locked. A fake agreeing with the implementation would prove nothing.

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "errors.h"
#include "model/orphans.h"
#include "platform/filesystem.h"
#include "platform/registry.h"

using wsldisk::model::canonical_path;
using wsldisk::model::default_scan_patterns;
using wsldisk::model::DistroList;
using wsldisk::model::expand_scan_pattern;
using wsldisk::model::find_orphans;
using wsldisk::model::Orphan;
using wsldisk::platform::Win32FileSystem;

namespace {

/// A directory under %TEMP% that removes itself and everything in it.
class TempTree {
public:
    TempTree()
        : path_(std::filesystem::temp_directory_path() /
                ("wsldisk-orphans-" + std::to_string(::GetCurrentProcessId()) + "-" +
                 std::to_string(++counter))) {
        std::filesystem::create_directories(path_);
    }

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Creates `relative` and every directory above it, with `size` bytes in it.
    [[nodiscard]] std::filesystem::path write(const std::string& relative, std::size_t size) const {
        const std::filesystem::path file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream stream(file, std::ios::binary);
        const std::vector<char> zeros(size, '\0');
        stream.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        return file;
    }

private:
    static int counter;
    std::filesystem::path path_;
};

int TempTree::counter = 0;

/// Whether the scan found `path`, however either side spelled it.
[[nodiscard]] bool found(const std::vector<Orphan>& orphans, const std::filesystem::path& path) {
    const std::wstring wanted = canonical_path(path.wstring());
    return std::ranges::any_of(
        orphans, [&wanted](const Orphan& orphan) { return canonical_path(orphan.path.wstring()) == wanted; });
}

}  // namespace

TEST_CASE("the scan finds every disk under a wildcard and nothing else", "[contract][orphans]") {
    const TempTree tree;
    const std::filesystem::path ubuntu = tree.write(R"(wsl\Ubuntu\ext4.vhdx)", 4096);
    const std::filesystem::path stale = tree.write(R"(wsl\Removed\ext4.vhdx)", 8192);
    // Neither of these is a virtual disk, and the scan must not report either.
    // The second is the case that motivated re-checking the extension: the
    // filesystem matches `*.vhdx` against the 8.3 short name too.
    const std::filesystem::path notes = tree.write(R"(wsl\Ubuntu\notes.txt)", 16);
    const std::filesystem::path backup = tree.write(R"(wsl\Ubuntu\ext4.vhdx.bak)", 32);

    const Win32FileSystem fs;
    const auto directories = expand_scan_pattern(fs, tree.path() / LR"(wsl\*)");
    REQUIRE(directories.size() == 2);

    // An empty distribution list: nothing is claimed, so every disk is orphaned.
    std::vector<std::string> warnings;
    const auto orphans = find_orphans(fs, DistroList{}, directories, warnings);

    CHECK(warnings.empty());
    CHECK(orphans.size() == 2);
    CHECK(found(orphans, ubuntu));
    CHECK(found(orphans, stale));
    CHECK_FALSE(found(orphans, notes));
    CHECK_FALSE(found(orphans, backup));
}

TEST_CASE("the scan measures what a disk is really costing", "[contract][orphans]") {
    const TempTree tree;
    const std::filesystem::path disk = tree.write(R"(wsl\Removed\ext4.vhdx)", 65536);

    const Win32FileSystem fs;
    std::vector<std::string> warnings;
    const auto orphans = find_orphans(fs, DistroList{}, {disk.parent_path()}, warnings);

    REQUIRE(orphans.size() == 1);
    REQUIRE(orphans.front().size_on_disk.has_value());
    // Not the logical size: what the volume gave it, which is what deleting
    // would actually recover.
    CHECK(*orphans.front().size_on_disk > 0);
}

TEST_CASE("the scan skips a directory that is not there", "[contract][orphans]") {
    const TempTree tree;

    const Win32FileSystem fs;
    std::vector<std::string> warnings;
    const auto orphans = find_orphans(fs, DistroList{}, {tree.path() / L"missing"}, warnings);

    // One unreadable directory must not hide the rest, so it warns and carries
    // on rather than failing the scan.
    CHECK(orphans.empty());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("missing") != std::string::npos);
}

TEST_CASE("a wildcard over a directory that is not there expands to nothing", "[contract][orphans]") {
    const TempTree tree;
    const Win32FileSystem fs;

    CHECK(expand_scan_pattern(fs, tree.path() / LR"(missing\*)").empty());
}

TEST_CASE("is_locked tells a free file from one this process holds open", "[contract][orphans]") {
    const TempTree tree;
    const std::filesystem::path disk = tree.write(R"(wsl\Removed\ext4.vhdx)", 4096);

    const Win32FileSystem fs;

    const auto before = fs.is_locked(disk);
    REQUIRE(before.has_value());
    CHECK_FALSE(*before);

    // Opened for writing with no sharing -- the way another process holding a
    // mounted VHDX has it open.
    const HANDLE held =
        ::CreateFileW(disk.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);

    const auto during = fs.is_locked(disk);
    REQUIRE(::CloseHandle(held) != FALSE);

    REQUIRE(during.has_value());
    CHECK(*during);

    const auto after = fs.is_locked(disk);
    REQUIRE(after.has_value());
    CHECK_FALSE(*after);
}

TEST_CASE("is_locked reports a file that is not there rather than answering", "[contract][orphans]") {
    const TempTree tree;

    const Win32FileSystem fs;
    const auto locked = fs.is_locked(tree.path() / L"missing.vhdx");

    // "Gone" is not "free": deleting on that answer would hide the real reason.
    REQUIRE_FALSE(locked.has_value());
    CHECK(locked.error().message.find("is in use") != std::string::npos);
}

TEST_CASE("the default scan patterns resolve against the real environment", "[contract][orphans]") {
    const Win32FileSystem fs;
    const auto patterns = default_scan_patterns(fs);

    REQUIRE(patterns.has_value());
    REQUIRE(patterns->size() == 3);
    for (const std::filesystem::path& pattern : *patterns) {
        // %LOCALAPPDATA% really expanded: an unexpanded pattern would send the
        // scan somewhere that cannot exist.
        CHECK(pattern.wstring().find(L'%') == std::wstring::npos);
        CHECK(pattern.is_absolute());
    }
}
