#include "ops/move.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "errors.h"
#include "fake_filesystem.h"
#include "fake_registry.h"
#include "fake_wsl_host.h"
#include "lxss_hives.h"
#include "model/distro.h"
#include "ops/runner.h"
#include "recording_sink.h"

using wsldisk::ErrorCode;
using wsldisk::VolumeInfo;
using wsldisk::WslCommandResult;
using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::MoveOperation;
using wsldisk::ops::MoveOptions;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeWslHost;
using wsldisk::testing::RecordingSink;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

/// Ubuntu's disk in the canned hive.
[[nodiscard]] std::filesystem::path ubuntu_disk() {
    return std::filesystem::path{
        LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)"};
}

/// A machine where a move across volumes works: a 1 TiB disk occupying 12 GiB
/// on C:, and a roomy NTFS D: to move it to.
struct Machine {
    FakeRegistry registry = hives::everything();
    FakeFileSystem filesystem;
    FakeWslHost host;
    RecordingSink sink;

    Machine() {
        filesystem.add_file(ubuntu_disk(),
                            FakeFileSystem::File{.size = 1024 * gigabyte, .size_on_disk = 12 * gigabyte});
        filesystem.set_volume(LR"(C:\)", VolumeInfo{.filesystem_name = "NTFS",
                                                    .total_bytes = 512 * gigabyte,
                                                    .free_bytes = 100 * gigabyte});
        filesystem.set_volume(LR"(D:\)", VolumeInfo{.filesystem_name = "NTFS",
                                                    .total_bytes = 512 * gigabyte,
                                                    .free_bytes = 400 * gigabyte});
        // The smoke test: nothing running, and `/bin/sh -c :` succeeds.
        host.set_running({});
        host.on_command("/bin/sh", WslCommandResult{.exit_code = 0});
    }

    [[nodiscard]] Distro distro(std::string_view name) {
        const auto distros = enumerate(registry);
        REQUIRE(distros.has_value());
        const Distro* found = distros->find(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    [[nodiscard]] MoveOperation move(std::string_view name, std::wstring_view destination = LR"(D:\WSL)",
                                     MoveOptions options = {}) {
        return MoveOperation{registry, filesystem, host, distro(name), std::filesystem::path{destination},
                             options};
    }
};

[[nodiscard]] std::wstring base_path_of(const FakeRegistry& registry, const Distro& distro) {
    const auto value = registry.read_string(wsldisk::model::registry_key_for(distro), L"BasePath");
    REQUIRE(value.has_value());
    REQUIRE(value->has_value());
    return **value;
}

}  // namespace

TEST_CASE("move copies the disk, repoints the registry and removes the original", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    const std::filesystem::path target{LR"(D:\WSL\ext4.vhdx)"};
    CHECK(machine.filesystem.exists(target));
    // The original is gone, which is what makes it a move rather than a copy.
    CHECK_FALSE(machine.filesystem.exists(ubuntu_disk()));
    CHECK(base_path_of(machine.registry, machine.distro("Ubuntu")) == LR"(D:\WSL)");
    CHECK(operation.target() == target);
    CHECK_FALSE(operation.is_same_volume());
}

TEST_CASE("move keeps the original when asked", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu", LR"(D:\WSL)", MoveOptions{.keep_source = true});

    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    CHECK(machine.filesystem.exists(std::filesystem::path{LR"(D:\WSL\ext4.vhdx)"}));
    CHECK(machine.filesystem.exists(ubuntu_disk()));
}

TEST_CASE("a move within one volume renames instead of copying", "[ops][move]") {
    // A rename moves no bytes and needs no free space. Copying twelve gigabytes
    // to land in the same place would be a long wait for nothing.
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu", LR"(C:\WSL)");

    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    CHECK(operation.is_same_volume());
    CHECK(operation.was_renamed());
    CHECK(machine.filesystem.renamed().size() == 1);
    CHECK(machine.filesystem.copied().empty());
    CHECK(machine.filesystem.exists(std::filesystem::path{LR"(C:\WSL\ext4.vhdx)"}));
    CHECK_FALSE(machine.filesystem.exists(ubuntu_disk()));
}

TEST_CASE("keeping the source copies even within one volume", "[ops][move]") {
    // A rename cannot leave the original behind, so honouring `--keep-source`
    // means giving up the fast path. It used to take the rename anyway and the
    // flag did nothing: the user asked for two files and got one.
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu", LR"(C:\WSL)", MoveOptions{.keep_source = true});

    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    CHECK(operation.is_same_volume());
    CHECK_FALSE(operation.was_renamed());
    CHECK(machine.filesystem.renamed().empty());
    CHECK(machine.filesystem.copied().size() == 1);
    CHECK(machine.filesystem.exists(ubuntu_disk()));
    CHECK(machine.filesystem.exists(std::filesystem::path{LR"(C:\WSL\ext4.vhdx)"}));
}

TEST_CASE("keeping the source within one volume still checks there is room", "[ops][move]") {
    // The copy needs the space like any other copy, and the free-space check
    // used to be skipped whenever the volumes matched.
    Machine machine;
    machine.filesystem.set_volume(
        LR"(C:\)",
        VolumeInfo{.filesystem_name = "NTFS", .total_bytes = 20 * gigabyte, .free_bytes = 1 * gigabyte});
    MoveOperation operation = machine.move("Ubuntu", LR"(C:\WSL)", MoveOptions{.keep_source = true});

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("free") != std::string::npos);
}

TEST_CASE("a same-volume move plans no delete, because the rename left nothing", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu", LR"(C:\WSL)");

    const auto planned = operation.plan();

    REQUIRE(planned.has_value());
    for (const auto& step : planned->steps) {
        CHECK(step.description.find("delete") == std::string::npos);
    }
}

TEST_CASE("move preserves an extended-length BasePath", "[ops][move]") {
    // docker-desktop's entry uses `\\?\`. Rewriting it as a bare path would be
    // an unrequested change to a value Docker Desktop owns (spike #4).
    Machine machine;
    machine.filesystem.add_file(
        std::filesystem::path{LR"(C:\Users\example\AppData\Local\Docker\wsl\main\ext4.vhdx)"},
        FakeFileSystem::File{.size = 4096, .size_on_disk = 4096});
    MoveOperation operation = machine.move("docker-desktop");

    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    CHECK(base_path_of(machine.registry, machine.distro("docker-desktop")) == LR"(\\?\D:\WSL)");
}

TEST_CASE("move leaves a legacy entry without a VhdFileName alone", "[ops][move]") {
    // Adding the value to an entry that never had it would change its layout
    // rather than repair it.
    Machine machine;
    const std::filesystem::path legacy{
        LR"(C:\Users\example\AppData\Local\Packages\CanonicalGroupLimited.Ubuntu20.04LTS_79rhkp1fndgsc\LocalState\ext4.vhdx)"};
    machine.filesystem.add_file(legacy, FakeFileSystem::File{.size = 4096, .size_on_disk = 4096});
    MoveOperation operation = machine.move("Ubuntu-20.04");

    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    const auto value = machine.registry.read_string(
        wsldisk::model::registry_key_for(machine.distro("Ubuntu-20.04")), L"VhdFileName");
    REQUIRE(value.has_value());
    CHECK_FALSE(value->has_value());
}

TEST_CASE("move refuses a WSL1 distribution", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Legacy-WSL1");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::Preflight);
    CHECK(outcome.error().message.find("WSL1") != std::string::npos);
}

TEST_CASE("move refuses a distribution whose disk is not where the registry says", "[ops][move]") {
    // There is nothing to move, and `relink` is the command that fixes it.
    Machine machine;
    REQUIRE(machine.filesystem.remove(ubuntu_disk()).has_value());
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().remedy.find("relink") != std::string::npos);
}

TEST_CASE("move refuses to move a disk onto itself", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move(
        "Ubuntu", LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581})");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("already at") != std::string::npos);
}

TEST_CASE("move refuses to overwrite a disk already at the destination", "[ops][move]") {
    // The likeliest thing sitting at that path is the user's own previous
    // attempt, and overwriting it would destroy a disk.
    Machine machine;
    machine.filesystem.add_file(std::filesystem::path{LR"(D:\WSL\ext4.vhdx)"},
                                FakeFileSystem::File{.size = 4096});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("already exists") != std::string::npos);
}

TEST_CASE("move refuses a running distribution", "[ops][move]") {
    // Not caution for its own sake: the smoke test would run in the guest that
    // is already booted -- from the old disk -- and pass without testing
    // anything.
    Machine machine;
    machine.host.set_running({"Ubuntu"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::DistroBusy);
}

TEST_CASE("move refuses when it cannot tell whether the distribution is running", "[ops][move]") {
    Machine machine;
    machine.host.fail_running(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check the installation"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::Preflight);
}

TEST_CASE("move refuses a target volume that cannot hold a virtual disk", "[ops][move]") {
    Machine machine;
    machine.filesystem.set_volume(
        LR"(E:\)",
        VolumeInfo{.filesystem_name = "exFAT", .total_bytes = 512 * gigabyte, .free_bytes = 400 * gigabyte});
    MoveOperation operation = machine.move("Ubuntu", LR"(E:\WSL)");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("exFAT") != std::string::npos);
    CHECK(outcome.error().remedy.find("4 GB") != std::string::npos);
}

TEST_CASE("move refuses a target volume without room", "[ops][move]") {
    // Against what the file occupies, not its virtual size: judging a 1 TiB
    // VHDX by its virtual size would refuse every move anyone wanted to make.
    Machine machine;
    machine.filesystem.set_volume(
        LR"(E:\)",
        VolumeInfo{.filesystem_name = "NTFS", .total_bytes = 20 * gigabyte, .free_bytes = 8 * gigabyte});
    MoveOperation operation = machine.move("Ubuntu", LR"(E:\WSL)");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("free") != std::string::npos);
}

TEST_CASE("move reports a volume it could not read", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu", LR"(Z:\WSL)");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
}

TEST_CASE("move reports a size it could not measure", "[ops][move]") {
    Machine machine;
    machine.filesystem.fail_size_on_disk(
        wsldisk::Error{ErrorCode::Generic, "the volume went away", "check the drive"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
}

TEST_CASE("move reports a same-volume check it could not make", "[ops][move]") {
    Machine machine;
    machine.filesystem.fail_same_volume(
        wsldisk::Error{ErrorCode::Generic, "no such volume", "check the drive letter"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
}

TEST_CASE("a copy that fails leaves the original and the registry alone", "[ops][move]") {
    Machine machine;
    machine.filesystem.fail_copy(
        wsldisk::Error{ErrorCode::Generic, "the volume filled up", "free some space"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.filesystem.exists(ubuntu_disk()));
    CHECK(base_path_of(machine.registry, machine.distro("Ubuntu")).find(L"D:") == std::wstring::npos);
}

TEST_CASE("a rename that fails leaves the original alone", "[ops][move]") {
    Machine machine;
    machine.filesystem.fail_rename(
        wsldisk::Error{ErrorCode::Generic, "access denied", "close whatever has it open"});
    MoveOperation operation = machine.move("Ubuntu", LR"(C:\WSL)");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.filesystem.exists(ubuntu_disk()));
}

TEST_CASE("a destination directory that cannot be made stops the move", "[ops][move]") {
    // Before anything is copied, so there is nothing to undo -- but it has to be
    // reported rather than falling through to a copy into a directory that is
    // not there.
    Machine machine;
    machine.filesystem.fail_write(
        wsldisk::Error{ErrorCode::Generic, "access denied", "check the permissions on the parent"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.filesystem.exists(ubuntu_disk()));
    CHECK(machine.filesystem.copied().empty());
}

TEST_CASE("a VhdFileName write that fails puts the BasePath back", "[ops][move]") {
    // The second of the two registry writes. A fake that failed both could only
    // ever exercise the first, which is the case with nothing to undo.
    Machine machine;
    machine.registry.fail_write(
        L"VhdFileName", wsldisk::Error{ErrorCode::Generic, "access denied", "run as the key's owner"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(base_path_of(machine.registry, machine.distro("Ubuntu")).find(L"D:") == std::wstring::npos);
    CHECK(machine.filesystem.exists(ubuntu_disk()));
}

TEST_CASE("a distribution that will not start puts everything back", "[ops][move]") {
    // The whole reason the source is deleted last. A failed start has to leave a
    // working distribution behind, not two halves of one.
    Machine machine;
    machine.host.on_command("/bin/sh", WslCommandResult{.exit_code = 1});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("did not start") != std::string::npos);
    // The registry points where it did, the original is untouched, and the
    // half-made copy is gone.
    CHECK(base_path_of(machine.registry, machine.distro("Ubuntu")).find(L"D:") == std::wstring::npos);
    CHECK(machine.filesystem.exists(ubuntu_disk()));
    CHECK_FALSE(machine.filesystem.exists(std::filesystem::path{LR"(D:\WSL\ext4.vhdx)"}));
}

TEST_CASE("a rename is undone by moving the file back", "[ops][move]") {
    // Not by deleting it: after a rename the file at the target *is* the
    // original, and deleting it would destroy the disk.
    Machine machine;
    machine.host.on_command("/bin/sh", WslCommandResult{.exit_code = 1});
    MoveOperation operation = machine.move("Ubuntu", LR"(C:\WSL)");

    REQUIRE_FALSE(run(operation, machine.sink, RunOptions{}).has_value());

    CHECK(machine.filesystem.exists(ubuntu_disk()));
    CHECK_FALSE(machine.filesystem.exists(std::filesystem::path{LR"(C:\WSL\ext4.vhdx)"}));
}

TEST_CASE("a start that could not be attempted is reported", "[ops][move]") {
    Machine machine;
    machine.host.fail_command(wsldisk::Error{ErrorCode::Generic, "wsl.exe is not installed", "install WSL"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.filesystem.exists(ubuntu_disk()));
}

TEST_CASE("a registry write that fails puts the copy back", "[ops][move]") {
    Machine machine;
    machine.registry.fail_write(
        L"BasePath", wsldisk::Error{ErrorCode::Generic, "access denied", "run as the owner of the key"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.filesystem.exists(ubuntu_disk()));
    CHECK_FALSE(machine.filesystem.exists(std::filesystem::path{LR"(D:\WSL\ext4.vhdx)"}));
}

TEST_CASE("a source that will not delete is a warning, not a failure", "[ops][move]") {
    // The move worked. Failing an operation that achieved what it set out to,
    // because a leftover would not go away, would tell the user to undo
    // something that is fine.
    Machine machine;
    machine.filesystem.fail_remove(
        wsldisk::Error{ErrorCode::Generic, "access denied", "close whatever has it open"});
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK(machine.sink.said("could not be deleted"));
    CHECK(machine.sink.said("can be removed by hand"));
}

TEST_CASE("move reports progress while it copies", "[ops][move]") {
    Machine machine;
    machine.filesystem.set_copy_progress({4 * gigabyte, 8 * gigabyte, 12 * gigabyte});
    MoveOperation operation = machine.move("Ubuntu");

    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    REQUIRE(machine.sink.progress_reports.size() == 3);
    CHECK(machine.sink.progress_reports.back().current == 12 * gigabyte);
    CHECK(machine.sink.progress_reports.back().total == 12 * gigabyte);
}

TEST_CASE("a dry run changes nothing", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu");

    const auto outcome = run(operation, machine.sink, RunOptions{.dry_run = true});

    REQUIRE(outcome.has_value());
    CHECK(machine.filesystem.exists(ubuntu_disk()));
    CHECK(machine.filesystem.copied().empty());
    CHECK(machine.filesystem.renamed().empty());
    CHECK(base_path_of(machine.registry, machine.distro("Ubuntu")).find(L"D:") == std::wstring::npos);
    // And it says what it would have done, including the delete.
    REQUIRE(outcome->plan.steps.size() == 4);
    CHECK(outcome->plan.steps.back().description.find("delete") != std::string::npos);
    // The delete is the point of no return, so it carries no undo.
    CHECK(outcome->plan.steps.back().is_irreversible());
}

TEST_CASE("the plan warns that the original will be deleted", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu");

    const auto planned = operation.plan();

    REQUIRE(planned.has_value());
    bool warned = false;
    for (const auto& warning : planned->warnings) {
        if (warning.message.find("original disk is deleted") != std::string::npos) {
            warned = true;
            CHECK(warning.remedy.find("--keep-source") != std::string::npos);
        }
    }
    CHECK(warned);
}

TEST_CASE("verify notices a registry entry that is not what was written", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu");
    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    // Something else rewrote it after the move.
    REQUIRE(machine.registry
                .write_string(wsldisk::model::registry_key_for(machine.distro("Ubuntu")), L"BasePath",
                              LR"(E:\elsewhere)")
                .has_value());

    const auto verified = operation.verify();
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error().code == ErrorCode::Partial);
}

TEST_CASE("verify notices a disk that is not at the destination", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu");
    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    REQUIRE(machine.filesystem.remove(std::filesystem::path{LR"(D:\WSL\ext4.vhdx)"}).has_value());

    const auto verified = operation.verify();
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error().message.find("not there") != std::string::npos);
}

TEST_CASE("verify reports a registry it could not read", "[ops][move]") {
    Machine machine;
    MoveOperation operation = machine.move("Ubuntu");
    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    machine.registry.fail_value(
        L"BasePath", wsldisk::Error{ErrorCode::Generic, "the key went away", "check the registry"});

    const auto verified = operation.verify();
    REQUIRE_FALSE(verified.has_value());
}
