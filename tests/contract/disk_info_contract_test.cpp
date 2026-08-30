// Contract test: measure a real VHDX, sparse and dense.
//
// The virtual-disk helper makes the disks, so this needs no WSL and no
// elevation. What it proves is that the numbers `list` will print come from the
// real APIs and relate to each other the way the output claims they do.

#include <windows.h>
#include <winioctl.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "model/disk_info.h"
#include "platform/clock.h"
#include "platform/filesystem.h"
#include "platform/virtual_disk.h"
#include "platform/wsl_host.h"

using wsldisk::model::DiskInfo;
using wsldisk::model::Distro;
using wsldisk::model::measure;
using wsldisk::model::ProbeOptions;
using wsldisk::platform::Win32FileSystem;
using wsldisk::platform::Win32VirtualDisk;
using wsldisk::platform::WslExeHost;

namespace {

constexpr std::uint64_t small_disk = 64ULL * 1024 * 1024;

/// A VHDX under %TEMP% that deletes itself.
class TempDisk {
public:
    TempDisk() : path_(make_path()) {
        const Win32VirtualDisk disks;
        REQUIRE(disks.create(path_, small_disk).has_value());
    }

    ~TempDisk() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempDisk(const TempDisk&) = delete;
    TempDisk& operator=(const TempDisk&) = delete;
    TempDisk(TempDisk&&) = delete;
    TempDisk& operator=(TempDisk&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// A distribution pointing at this disk, under a name no real machine has,
    /// so the guest probe finds nothing running and never starts anything.
    [[nodiscard]] Distro as_distro() const {
        Distro distro;
        distro.name = "wsldisk-contract-not-a-real-distro";
        distro.guid = "{00000000-0000-0000-0000-00000000c07e}";
        distro.version = 2;
        distro.base_path = path_.parent_path().wstring();
        distro.vhdx_path = path_;
        return distro;
    }

private:
    static std::filesystem::path make_path() {
        static int counter = 0;
        return std::filesystem::temp_directory_path() /
               ("wsldisk-info-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(++counter) +
                ".vhdx");
    }

    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("measuring a real disk fills in every host-side field", "[contract][disk-info]") {
    const TempDisk disk;
    const Win32FileSystem filesystem;
    const Win32VirtualDisk disks;
    const WslExeHost host;

    const DiskInfo info = measure(disk.as_distro(), filesystem, disks, host, ProbeOptions{});
    REQUIRE(info.virtual_size.has_value());
    CHECK(*info.virtual_size == small_disk);
    REQUIRE(info.file_size.has_value());
    REQUIRE(info.size_on_disk.has_value());
    REQUIRE(info.allocated_bytes.has_value());
    REQUIRE(info.is_sparse.has_value());

    // A freshly created dynamic disk holds far less than its maximum.
    CHECK(*info.file_size < small_disk);
    // Allocation cannot exceed the file it is inside.
    CHECK(*info.allocated_bytes <= *info.file_size);
}

TEST_CASE("a distribution that is not running is not started to measure it", "[contract][disk-info]") {
    // The name belongs to nothing, so this also proves the code takes the "not
    // running" path rather than failing when the distribution does not exist.
    const TempDisk disk;
    const Win32FileSystem filesystem;
    const Win32VirtualDisk disks;
    const WslExeHost host;

    const DiskInfo info = measure(disk.as_distro(), filesystem, disks, host, ProbeOptions{});
    CHECK_FALSE(info.guest_used.has_value());
    CHECK_FALSE(info.reclaimable().has_value());
    // And it said why, rather than leaving a silently blank column.
    CHECK_FALSE(info.notes.empty());
}

TEST_CASE("a missing disk leaves every host field unknown but still answers", "[contract][disk-info]") {
    // `orphans` is built on exactly this: the registry names a disk that is not
    // there, and `list` still prints the row.
    const Win32FileSystem filesystem;
    const Win32VirtualDisk disks;
    const WslExeHost host;

    Distro missing;
    missing.name = "wsldisk-contract-missing";
    missing.guid = "{00000000-0000-0000-0000-00000000c07f}";
    missing.version = 2;
    missing.base_path = LR"(C:\wsldisk-does-not-exist)";
    missing.vhdx_path = LR"(C:\wsldisk-does-not-exist\ext4.vhdx)";

    const DiskInfo info = measure(missing, filesystem, disks, host, ProbeOptions{});
    CHECK_FALSE(info.file_size.has_value());
    CHECK_FALSE(info.virtual_size.has_value());
    CHECK_FALSE(info.notes.empty());
}
