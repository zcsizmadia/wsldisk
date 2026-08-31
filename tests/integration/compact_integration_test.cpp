// Integration cases for `compact`, against real WSL2 and a real VHDX. This is
// the milestone's acceptance test: the whole premise of the project is that a
// Windows Home user with no Hyper-V module can reclaim space, and only a real
// compaction of a real disk can show that.
//
// Everything here runs against a throwaway distribution imported from the
// pinned rootfs. Nothing ever names a distribution the developer actually uses.
//
// `--shutdown` is used deliberately: the WSL utility VM holds every attached
// disk for as long as any distribution runs (D9), so this is the only way to
// compact anything at all, and it is what the real command does when asked.
// That does stop the developer's other distributions, which is why the whole
// suite is behind WSLDISK_INTEGRATION=1.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

#include "integration_fixture.h"
#include "model/distro.h"
#include "ops/compact.h"
#include "ops/runner.h"
#include "platform/clock.h"
#include "platform/filesystem.h"
#include "platform/registry.h"
#include "platform/virtual_disk.h"
#include "platform/wsl_host.h"

using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::CompactOperation;
using wsldisk::ops::CompactOptions;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::platform::SystemClock;
using wsldisk::platform::Win32FileSystem;
using wsldisk::platform::Win32Registry;
using wsldisk::platform::Win32VirtualDisk;
using wsldisk::platform::WslExeHost;
using wsldisk::testing::integration_blocker;
using wsldisk::testing::ScratchDistro;

namespace {

constexpr std::uint64_t mebibyte = 1024ULL * 1024;

/// Big enough that a compaction has something obvious to reclaim, small enough
/// that writing it does not dominate the run.
constexpr std::uint64_t junk_megabytes = 512;

[[nodiscard]] bool ready() {
    if (const auto blocker = integration_blocker(); blocker.has_value()) {
        SKIP(*blocker);
    }
    return true;
}

/// The registered form of a throwaway distribution.
[[nodiscard]] Distro registered(const Win32Registry& registry, const std::string& name) {
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());
    const Distro* found = distros->find(name);
    REQUIRE(found != nullptr);
    return *found;
}

}  // namespace

TEST_CASE("compact reclaims what was freed inside the guest", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"compact"};
    REQUIRE(distro.valid());

    // The case `compact` exists for: a disk that only ever grows. WSL 2.5+
    // creates disks sparse on some machines, and a sparse disk gives its space
    // back by itself, which is a different scenario. Not fatal if this build
    // will not do it -- the growth check below decides either way.
    static_cast<void>(distro.set_sparse(false));

    // Grow the disk, then free the space inside the guest. The fixture's
    // `write_junk` is what carries the `conv=fsync` this needs; without it the
    // guest page cache absorbs the write and the .vhdx never grows.
    REQUIRE(distro.write_junk(junk_megabytes));

    // Something to compare afterwards: a compaction that damaged the filesystem
    // would be a far worse failure than one that reclaimed nothing.
    const auto before_hash = distro.file_hash("/etc/os-release");
    REQUIRE(before_hash.has_value());

    // ext4 does not return the blocks to the host file on its own, even mounted
    // with `discard` -- measured in spike #1 and again here. That is the whole
    // reason `compact` runs fstrim first.
    REQUIRE(distro.delete_junk());

    const Win32FileSystem filesystem;
    REQUIRE(distro.release_disk());
    const auto grown = filesystem.file_size_on_disk(distro.vhdx());
    REQUIRE(grown.has_value());
    if (*grown <= junk_megabytes * mebibyte / 2) {
        // The space came back without us. Nothing to reclaim means nothing to
        // assert; saying so beats a failure that blames the code.
        SKIP("the disk released the space by itself; there is nothing to compact");
    }

    const Win32Registry registry;
    const Win32VirtualDisk disks;
    const WslExeHost host;
    const SystemClock clock;
    CompactOperation operation{disks,
                               filesystem,
                               host,
                               clock,
                               registered(registry, distro.name()),
                               CompactOptions{.shutdown = true}};
    wsldisk::ops::NullSink sink;

    const auto outcome = run(operation, sink, RunOptions{});
    if (!outcome.has_value()) {
        FAIL("compact failed: " << outcome.error().to_string());
    }

    REQUIRE(operation.size_before().has_value());
    REQUIRE(operation.size_after().has_value());
    REQUIRE(operation.reclaimed().has_value());
    INFO("before " << *operation.size_before() << ", after " << *operation.size_after());
    // Half of what was written, as a floor. Spike #1 reclaimed all of it, but a
    // test that pins the exact figure would be pinning the guest's allocator.
    CHECK(*operation.reclaimed() >= junk_megabytes * mebibyte / 2);

    // The disk still works, which is the part that matters more than the
    // saving -- and the file that was there before is byte for byte the file
    // that is there now.
    CHECK(distro.boots());
    CHECK(distro.file_hash("/etc/os-release") == before_hash);
}

TEST_CASE("compact refuses rather than stopping WSL on its own", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"compactbusy"};
    REQUIRE(distro.valid());
    // Started and left running, so the utility VM holds its disk open.
    REQUIRE(distro.boots());

    const Win32Registry registry;
    const Win32FileSystem filesystem;
    const Win32VirtualDisk disks;
    const WslExeHost host;
    const SystemClock clock;
    CompactOperation operation{disks, filesystem, host, clock, registered(registry, distro.name())};
    wsldisk::ops::NullSink sink;

    const auto outcome = run(operation, sink, RunOptions{});

    // Decision D9. Terminating the target is not enough, and stopping every
    // other distribution to get around that is the user's call, not ours.
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == wsldisk::ErrorCode::DistroBusy);
    CHECK(outcome.error().remedy.find("--shutdown") != std::string::npos);
}

TEST_CASE("compact changes nothing on a dry run against a real disk", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"compactdry"};
    REQUIRE(distro.valid());

    const Win32Registry registry;
    const Win32FileSystem filesystem;
    const Win32VirtualDisk disks;
    const WslExeHost host;
    const SystemClock clock;
    const auto before = filesystem.file_size_on_disk(distro.vhdx());
    REQUIRE(before.has_value());

    CompactOperation operation{disks, filesystem, host, clock, registered(registry, distro.name())};
    wsldisk::ops::NullSink sink;

    const auto outcome = run(operation, sink, RunOptions{.dry_run = true});

    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->report.has_value());
    const auto after = filesystem.file_size_on_disk(distro.vhdx());
    REQUIRE(after.has_value());
    CHECK(*after == *before);
}
