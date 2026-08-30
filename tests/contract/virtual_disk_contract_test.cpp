// Contract tests: Win32VirtualDisk against the real Virtual Disk Service, on
// real VHDX files under %TEMP%. No WSL, no elevation, no diskpart -- the create
// helper on the interface exists so these can make their own disk.

#include <windows.h>
#include <virtdisk.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "errors.h"
#include "platform/virtual_disk.h"

using wsldisk::DiskProgress;
using wsldisk::ErrorCode;
using wsldisk::platform::vhdx_storage_type;
using wsldisk::platform::Win32VirtualDisk;

namespace {

/// A VHDX under %TEMP% that deletes itself.
class TempDisk {
public:
    explicit TempDisk(std::uint64_t maximum_size) : path_(make_path()) {
        const Win32VirtualDisk disks;
        const auto created = disks.create(path_, maximum_size);
        INFO("create failed: " << (created.has_value() ? "" : created.error().to_string()));
        REQUIRE(created.has_value());
        REQUIRE(std::filesystem::exists(path_));
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

private:
    static std::filesystem::path make_path() {
        static int counter = 0;
        return std::filesystem::temp_directory_path() /
               ("wsldisk-contract-" + std::to_string(::GetCurrentProcessId()) + "-" +
                std::to_string(++counter) + ".vhdx");
    }

    std::filesystem::path path_;
};

/// The minimum a VHDX can be; small enough that these tests stay fast.
constexpr std::uint64_t small_disk = 64ULL * 1024 * 1024;

}  // namespace

TEST_CASE("a created disk reports the maximum it was asked for", "[contract][vdisk]") {
    const TempDisk disk{small_disk};

    const Win32VirtualDisk disks;
    const auto handle = disks.open(disk.path());
    REQUIRE(handle.has_value());

    const auto info = (*handle)->information();
    REQUIRE(info.has_value());
    CHECK(info->virtual_size == small_disk);
    CHECK(info->sector_size > 0);
    CHECK(info->block_size > 0);
    // A freshly created dynamic disk holds almost nothing.
    CHECK(info->physical_size < small_disk);
    // Nothing built by hand has a parent.
    CHECK(info->parent_path.empty());
}

TEST_CASE("compacting a real disk succeeds and reports progress", "[contract][vdisk]") {
    const TempDisk disk{small_disk};

    const Win32VirtualDisk disks;
    const auto handle = disks.open(disk.path());
    REQUIRE(handle.has_value());

    const auto before = (*handle)->information();
    REQUIRE(before.has_value());

    int reports = 0;
    const auto status = (*handle)->compact([&reports](const DiskProgress& progress) {
        ++reports;
        CHECK(progress.current <= progress.total);
        return true;
    });

    INFO("compact failed: " << (status.has_value() ? "" : status.error().to_string()));
    REQUIRE(status.has_value());
    // Progress is always reported at least once, even for a disk with nothing
    // to reclaim, so a caller never leaves a bar part-drawn.
    CHECK(reports >= 1);

    const auto after = (*handle)->information();
    REQUIRE(after.has_value());
    CHECK(after->virtual_size == before->virtual_size);
    CHECK(after->physical_size <= before->physical_size);
}

TEST_CASE("the disk is still usable after being compacted", "[contract][vdisk]") {
    const TempDisk disk{small_disk};
    const Win32VirtualDisk disks;

    {
        const auto handle = disks.open(disk.path());
        REQUIRE(handle.has_value());
        REQUIRE((*handle)->compact([](const DiskProgress&) { return true; }).has_value());
    }

    // Reopening proves the handle from the first block was closed, and that
    // compaction left a readable disk behind.
    const auto reopened = disks.open(disk.path());
    REQUIRE(reopened.has_value());
    CHECK((*reopened)->information().has_value());
}

TEST_CASE("opening a disk that does not exist is a preflight failure", "[contract][vdisk]") {
    const Win32VirtualDisk disks;
    const auto missing = std::filesystem::temp_directory_path() / "wsldisk-no-such-disk.vhdx";
    REQUIRE_FALSE(std::filesystem::exists(missing));

    const auto handle = disks.open(missing);

    REQUIRE_FALSE(handle.has_value());
    CHECK(handle.error().code == ErrorCode::Preflight);
}

TEST_CASE("opening a file that is not a VHDX fails rather than misbehaving", "[contract][vdisk]") {
    const auto path = std::filesystem::temp_directory_path() /
                      ("wsldisk-not-a-disk-" + std::to_string(::GetCurrentProcessId()) + ".vhdx");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "this is not a virtual disk";
    }

    const Win32VirtualDisk disks;
    const auto handle = disks.open(path);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    REQUIRE_FALSE(handle.has_value());
    CHECK_FALSE(handle.error().message.empty());
}

TEST_CASE("the V2 open parameters accept exactly one access mask", "[contract][vdisk]") {
    // This test exists to stop the parameter shape being "simplified". It calls
    // the raw API deliberately: Win32VirtualDisk hard-codes the working shape,
    // so the alternatives are not reachable through the interface.
    //
    // What D10 rests on is that V2 has one valid spelling and rejects the rest
    // at open, while V1 accepts a mask that opens and only fails once the
    // compaction is already under way. If either of those stops being true,
    // these fail, and that is the signal to revisit D10 rather than a reason to
    // delete the test.
    const TempDisk disk{small_disk};
    VIRTUAL_STORAGE_TYPE storage_type = vhdx_storage_type();

    SECTION("V1 with the wrong mask opens and then fails the compaction") {
        // The late failure the V2 shape avoids: by the time this is discovered,
        // a preflight has already told the user the disk is about to shrink.
        OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
        parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
        parameters.Version1.RWDepth = 1;

        HANDLE handle = nullptr;
        const DWORD opened = ::OpenVirtualDisk(&storage_type, disk.path().c_str(), VIRTUAL_DISK_ACCESS_NONE,
                                               OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &handle);
        REQUIRE(opened == ERROR_SUCCESS);

        COMPACT_VIRTUAL_DISK_PARAMETERS compact{};
        compact.Version = COMPACT_VIRTUAL_DISK_VERSION_1;
        const DWORD compacted =
            ::CompactVirtualDisk(handle, COMPACT_VIRTUAL_DISK_FLAG_NONE, &compact, nullptr);
        ::CloseHandle(handle);

        INFO("compaction returned " << compacted);
        CHECK(compacted == ERROR_ACCESS_DENIED);
    }

    SECTION("V1 with METAOPS compacts, so the mask is what V1 keys off") {
        // Recorded because docs/RESEARCH.md said the opposite until 2026-08-30:
        // the spike behind it declared METAOPS as 0x00020000, which is
        // ATTACH_RW, and measured the elevation that attaching read-write needs.
        OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
        parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
        parameters.Version1.RWDepth = 1;

        HANDLE handle = nullptr;
        const DWORD opened =
            ::OpenVirtualDisk(&storage_type, disk.path().c_str(), VIRTUAL_DISK_ACCESS_METAOPS,
                              OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &handle);
        REQUIRE(opened == ERROR_SUCCESS);

        COMPACT_VIRTUAL_DISK_PARAMETERS compact{};
        compact.Version = COMPACT_VIRTUAL_DISK_VERSION_1;
        const DWORD compacted =
            ::CompactVirtualDisk(handle, COMPACT_VIRTUAL_DISK_FLAG_NONE, &compact, nullptr);
        ::CloseHandle(handle);

        INFO("compaction returned " << compacted);
        CHECK(compacted == ERROR_SUCCESS);
    }

    SECTION("METAOPS with V2 parameters does not even open") {
        OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
        parameters.Version = OPEN_VIRTUAL_DISK_VERSION_2;

        HANDLE handle = nullptr;
        const DWORD opened =
            ::OpenVirtualDisk(&storage_type, disk.path().c_str(), VIRTUAL_DISK_ACCESS_METAOPS,
                              OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &handle);
        if (opened == ERROR_SUCCESS) {
            ::CloseHandle(handle);
        }
        INFO("open returned " << opened);
        CHECK(opened == ERROR_INVALID_PARAMETER);
    }
}

TEST_CASE("the working shape needs no administrator rights", "[contract][vdisk]") {
    // The premise the whole project rests on: CI does not run elevated, and this
    // passes there.
    const bool elevated = []() {
        HANDLE token = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
            return false;
        }
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        const BOOL got = ::GetTokenInformation(token, TokenElevation, &elevation, size, &size);
        ::CloseHandle(token);
        return got != FALSE && elevation.TokenIsElevated != 0;
    }();
    INFO("running elevated: " << elevated);

    const TempDisk disk{small_disk};
    const Win32VirtualDisk disks;
    const auto handle = disks.open(disk.path());
    REQUIRE(handle.has_value());
    CHECK((*handle)->compact([](const DiskProgress&) { return true; }).has_value());
}
