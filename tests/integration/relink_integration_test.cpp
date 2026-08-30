// Integration cases for `orphans --relink`, against real WSL2 and the real
// registry. They import a throwaway Alpine distribution, move its disk, point
// the registry at the new location and check it still boots.
//
// This is the first mutating command, so what is really being tested is the
// operation lifecycle: that a relink which cannot boot puts the registry back
// exactly as it was, and that the distribution still works afterwards.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "integration_fixture.h"
#include "model/distro.h"
#include "ops/relink.h"
#include "ops/runner.h"
#include "platform/filesystem.h"
#include "platform/registry.h"
#include "platform/wsl_host.h"

using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::RelinkOperation;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::platform::Win32FileSystem;
using wsldisk::platform::Win32Registry;
using wsldisk::platform::WslExeHost;
using wsldisk::testing::integration_enabled;
using wsldisk::testing::pinned_rootfs;
using wsldisk::testing::TempDistro;

namespace {

/// The real services, wired the way `app.cpp` wires them.
struct Machine {
    Win32Registry registry;
    Win32FileSystem filesystem;
    WslExeHost host;

    /// The registered distribution by name, or nothing if it is not there.
    [[nodiscard]] std::optional<Distro> distro(const std::string& name) {
        const auto distros = enumerate(registry);
        REQUIRE(distros.has_value());
        const Distro* found = distros->find(name);
        if (found == nullptr) {
            return std::nullopt;
        }
        return *found;
    }
};

/// A `ProgressSink` that ignores everything. The assertions are about the
/// registry and whether the distribution boots, not about the narration.
class QuietSink final : public wsldisk::ops::ProgressSink {
public:
    void step_started(std::size_t, const wsldisk::ops::StepPlan&) override {}

    void step_finished(std::size_t, const wsldisk::ops::StepPlan&) override {}

    void step_progress(const wsldisk::DiskProgress&) override {}

    void message(std::string_view) override {}
};

/// Skips unless real WSL and the pinned rootfs are both available.
[[nodiscard]] bool ready() {
    if (!integration_enabled()) {
        SKIP("set WSLDISK_INTEGRATION=1 to run integration tests");
    }
    if (pinned_rootfs().empty()) {
        SKIP("run scripts/fetch-fixtures.ps1 to download the pinned rootfs");
    }
    return true;
}

}  // namespace

TEST_CASE("relink follows a disk that moved and the distribution still boots", "[integration]") {
    if (!ready()) {
        return;
    }

    TempDistro distro{"relink"};
    REQUIRE(distro.valid());
    // Nothing may hold the disk open while it is moved.
    REQUIRE(distro.release_disk());

    const std::filesystem::path moved = distro.directory().parent_path() / (distro.name() + "-moved");
    distro.also_remove(moved);
    std::error_code failed;
    std::filesystem::create_directories(moved, failed);
    std::filesystem::rename(distro.vhdx(), moved / "ext4.vhdx", failed);
    REQUIRE_FALSE(failed);

    Machine machine;
    const auto registered = machine.distro(distro.name());
    REQUIRE(registered.has_value());

    RelinkOperation operation{machine.registry, machine.filesystem, machine.host, *registered,
                              moved / "ext4.vhdx"};
    QuietSink sink;
    const auto outcome = run(operation, sink, RunOptions{});

    if (!outcome.has_value()) {
        FAIL("relink failed: " << outcome.error().to_string());
    }

    // The proof: the distribution boots from where the registry now points.
    const auto guest = distro.run("/bin/true");
    CHECK(guest.exit_code == 0);

    const auto after = machine.distro(distro.name());
    REQUIRE(after.has_value());
    CHECK(after->vhdx_path == moved / "ext4.vhdx");
}

TEST_CASE("a relink that cannot boot leaves the distribution as it was", "[integration]") {
    if (!ready()) {
        return;
    }

    TempDistro distro{"rollback"};
    REQUIRE(distro.valid());
    REQUIRE(distro.release_disk());

    // A file with the right name and nothing else. WSL cannot start from it,
    // which is the whole point: the registry has to come back.
    const std::filesystem::path decoy = distro.directory().parent_path() / (distro.name() + "-decoy");
    distro.also_remove(decoy);
    std::error_code ignored;
    std::filesystem::create_directories(decoy, ignored);
    {
        std::ofstream stream(decoy / "ext4.vhdx", std::ios::binary);
        stream << "not a virtual disk";
    }

    Machine machine;
    const auto before = machine.distro(distro.name());
    REQUIRE(before.has_value());

    RelinkOperation operation{machine.registry, machine.filesystem, machine.host, *before,
                              decoy / "ext4.vhdx"};
    QuietSink sink;
    const auto outcome = run(operation, sink, RunOptions{});

    CHECK_FALSE(outcome.has_value());

    const auto after = machine.distro(distro.name());
    REQUIRE(after.has_value());
    CHECK(after->base_path == before->base_path);
    CHECK(after->vhd_file_name == before->vhd_file_name);

    // Not just "the values look right": it still runs.
    const auto guest = distro.run("/bin/true");
    CHECK(guest.exit_code == 0);
}
