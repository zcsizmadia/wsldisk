// Integration cases for `move`, against real WSL2, the real registry and real
// files. They import a throwaway Alpine distribution, relocate its disk and
// check it still boots with its contents intact.
//
// This is the operation where the ordering matters most: the source file is the
// user's only copy until the new one has been proved to work, so what is really
// under test is that a failure leaves a working distribution rather than two
// halves of one.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "integration_fixture.h"
#include "model/distro.h"
#include "ops/move.h"
#include "ops/runner.h"
#include "platform/filesystem.h"
#include "platform/registry.h"
#include "platform/wsl_host.h"

using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::MoveOperation;
using wsldisk::ops::MoveOptions;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::platform::Win32FileSystem;
using wsldisk::platform::Win32Registry;
using wsldisk::platform::WslExeHost;
using wsldisk::testing::integration_blocker;
using wsldisk::testing::ScratchDistro;

namespace {

struct Machine {
    Win32Registry registry;
    Win32FileSystem filesystem;
    WslExeHost host;

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

[[nodiscard]] bool ready() {
    if (const auto blocker = integration_blocker(); blocker.has_value()) {
        SKIP(*blocker);
    }
    return true;
}

}  // namespace

TEST_CASE("move relocates a disk and the distribution still boots", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"move"};
    REQUIRE(distro.valid());
    // Something to check afterwards: a move that damaged the filesystem would be
    // a far worse failure than one that did not happen.
    const auto before_hash = distro.file_hash("/etc/alpine-release");
    REQUIRE(before_hash.has_value());
    REQUIRE(distro.release_disk());

    const std::filesystem::path destination = distro.directory().parent_path() / (distro.name() + "-moved");
    distro.also_remove(destination);

    Machine machine;
    const auto registered = machine.distro(distro.name());
    REQUIRE(registered.has_value());
    const std::filesystem::path source = registered->vhdx_path;

    MoveOperation operation{machine.registry, machine.filesystem, machine.host, *registered, destination};
    wsldisk::ops::NullSink sink;
    const auto outcome = run(operation, sink, RunOptions{});
    if (!outcome.has_value()) {
        FAIL("move failed: " << outcome.error().to_string());
    }

    // The registry follows the file.
    const auto after = machine.distro(distro.name());
    REQUIRE(after.has_value());
    CHECK(after->vhdx_path == destination / "ext4.vhdx");
    CHECK(std::filesystem::exists(destination / "ext4.vhdx"));
    // And the original is gone, which is what makes it a move.
    CHECK_FALSE(std::filesystem::exists(source));

    // The proof: it boots from the new place, with the same contents.
    CHECK(distro.boots());
    CHECK(distro.file_hash("/etc/alpine-release") == before_hash);
}

TEST_CASE("move keeps the original when asked, and both disks are real", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"movekeep"};
    REQUIRE(distro.valid());
    REQUIRE(distro.release_disk());

    const std::filesystem::path destination = distro.directory().parent_path() / (distro.name() + "-kept");
    distro.also_remove(destination);

    Machine machine;
    const auto registered = machine.distro(distro.name());
    REQUIRE(registered.has_value());
    const std::filesystem::path source = registered->vhdx_path;

    MoveOperation operation{machine.registry, machine.filesystem, machine.host,
                            *registered,      destination,        MoveOptions{.keep_source = true}};
    wsldisk::ops::NullSink sink;
    const auto outcome = run(operation, sink, RunOptions{});
    if (!outcome.has_value()) {
        FAIL("move failed: " << outcome.error().to_string());
    }

    CHECK(std::filesystem::exists(destination / "ext4.vhdx"));
    // The point of the flag. %TEMP% and its sibling are one volume, so this
    // would otherwise have been a rename -- which cannot leave anything behind.
    CHECK(std::filesystem::exists(source));
    CHECK_FALSE(operation.was_renamed());
    CHECK(distro.boots());
}

TEST_CASE("a copy keeps both disks the same size", "[integration]") {
    // `--keep-source`, so both files exist afterwards and neither has been
    // booted from since the copy. Comparing a pre-move measurement with a
    // post-move one does not work: the move starts the distribution as its
    // smoke test, and booting grows the disk by tens of megabytes.
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"movesize"};
    REQUIRE(distro.valid());
    REQUIRE(distro.release_disk());

    const std::filesystem::path destination = distro.directory().parent_path() / (distro.name() + "-size");
    distro.also_remove(destination);

    Machine machine;
    const auto registered = machine.distro(distro.name());
    REQUIRE(registered.has_value());

    MoveOperation operation{machine.registry, machine.filesystem, machine.host,
                            *registered,      destination,        MoveOptions{.keep_source = true}};
    wsldisk::ops::NullSink sink;
    REQUIRE(run(operation, sink, RunOptions{}).has_value());

    const auto source = machine.filesystem.file_size_on_disk(registered->vhdx_path);
    const auto copy = machine.filesystem.file_size_on_disk(destination / "ext4.vhdx");
    REQUIRE(source.has_value());
    REQUIRE(copy.has_value());
    INFO("source " << *source << ", copy " << *copy);
    // The copy walks the source's allocated ranges, so it occupies what the
    // source does. Booting from the copy grew it, hence the slack; what this
    // catches is a copy that wrote the file's whole logical length regardless.
    CHECK(*copy <= *source + (64ULL * 1024 * 1024));
}

TEST_CASE("a move within one volume is a rename", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"movesame"};
    REQUIRE(distro.valid());
    REQUIRE(distro.release_disk());

    const std::filesystem::path destination = distro.directory().parent_path() / (distro.name() + "-same");
    distro.also_remove(destination);

    Machine machine;
    const auto registered = machine.distro(distro.name());
    REQUIRE(registered.has_value());

    MoveOperation operation{machine.registry, machine.filesystem, machine.host, *registered, destination};
    const auto planned = operation.plan();
    REQUIRE(planned.has_value());
    // %TEMP% and its sibling are the same volume, so this is the fast path: no
    // bytes move and there is no source left to delete.
    CHECK(operation.is_same_volume());
    CHECK(planned->steps.size() == 3);

    wsldisk::ops::NullSink sink;
    REQUIRE(run(operation, sink, RunOptions{}).has_value());
    CHECK(distro.boots());
}

TEST_CASE("move refuses a running distribution and changes nothing", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"moverunning"};
    REQUIRE(distro.valid());
    // Left running on purpose.
    REQUIRE(distro.boots());

    const std::filesystem::path destination = distro.directory().parent_path() / (distro.name() + "-never");
    distro.also_remove(destination);

    Machine machine;
    const auto registered = machine.distro(distro.name());
    REQUIRE(registered.has_value());
    const std::filesystem::path source = registered->vhdx_path;

    MoveOperation operation{machine.registry, machine.filesystem, machine.host, *registered, destination};
    wsldisk::ops::NullSink sink;
    const auto outcome = run(operation, sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == wsldisk::ErrorCode::DistroBusy);
    // Nothing was copied and nothing was repointed.
    CHECK_FALSE(std::filesystem::exists(destination / "ext4.vhdx"));
    CHECK(std::filesystem::exists(source));
    const auto after = machine.distro(distro.name());
    REQUIRE(after.has_value());
    CHECK(after->vhdx_path == source);
}

TEST_CASE("a move that is only planned changes nothing", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"movedry"};
    REQUIRE(distro.valid());
    REQUIRE(distro.release_disk());

    const std::filesystem::path destination = distro.directory().parent_path() / (distro.name() + "-dry");
    distro.also_remove(destination);

    Machine machine;
    const auto registered = machine.distro(distro.name());
    REQUIRE(registered.has_value());
    const std::filesystem::path source = registered->vhdx_path;

    MoveOperation operation{machine.registry, machine.filesystem, machine.host, *registered, destination};
    wsldisk::ops::NullSink sink;
    REQUIRE(run(operation, sink, RunOptions{.dry_run = true}).has_value());

    // Not even the destination directory, because a dry run has to be free of
    // side effects rather than merely free of mutations.
    CHECK_FALSE(std::filesystem::exists(destination));
    CHECK(std::filesystem::exists(source));
}
