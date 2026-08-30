#include "ops/relink.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "fake_filesystem.h"
#include "fake_registry.h"
#include "fake_wsl_host.h"
#include "lxss_hives.h"
#include "model/distro.h"
#include "ops/runner.h"
#include "recording_sink.h"

using wsldisk::ErrorCode;
using wsldisk::WslCommandResult;
using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::RelinkOperation;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeWslHost;
using wsldisk::testing::RecordingSink;
namespace hives = wsldisk::testing::hives;

namespace {

/// A registry, a filesystem and a host wired together, with the moved disk
/// already in place at `target`.
struct Machine {
    FakeRegistry registry = hives::everything();
    FakeFileSystem filesystem;
    FakeWslHost host;
    RecordingSink sink;

    Machine() { filesystem.add_file(target(), FakeFileSystem::File{.size = 4096}); }

    [[nodiscard]] static std::filesystem::path target() {
        return std::filesystem::path{LR"(D:\moved\Moved-Away\ext4.vhdx)"};
    }

    /// One distribution out of the canned hive, by name.
    [[nodiscard]] Distro distro(std::string_view name) {
        const auto distros = enumerate(registry);
        REQUIRE(distros.has_value());
        const Distro* found = distros->find(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    [[nodiscard]] RelinkOperation relink(std::string_view name, std::filesystem::path to = target()) {
        return RelinkOperation{registry, filesystem, host, distro(name), std::move(to)};
    }
};

/// The `BasePath` a distribution has now.
[[nodiscard]] std::wstring base_path_of(const FakeRegistry& registry, const Distro& distro) {
    const auto value = registry.read_string(wsldisk::model::registry_key_for(distro), L"BasePath");
    REQUIRE(value.has_value());
    REQUIRE(value->has_value());
    return **value;
}

}  // namespace

TEST_CASE("relink refuses a WSL1 distribution", "[ops][relink]") {
    Machine machine;
    RelinkOperation operation = machine.relink("Legacy-WSL1");

    const auto plan = operation.plan();

    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::Preflight);
    CHECK(plan.error().message.find("WSL1") != std::string::npos);
    CHECK(plan.error().remedy.find("--set-version") != std::string::npos);
}

TEST_CASE("relink refuses a target that is not there", "[ops][relink]") {
    Machine machine;
    RelinkOperation operation = machine.relink("Moved-Away", LR"(D:\nowhere\ext4.vhdx)");

    const auto plan = operation.plan();

    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::Preflight);
    CHECK(plan.error().message.find("does not exist") != std::string::npos);
}

TEST_CASE("relink refuses a disk the distribution already points at", "[ops][relink]") {
    Machine machine;
    // Ubuntu's own disk, spelled differently: the check is on the canonical
    // form, not on the characters.
    machine.filesystem.add_file(
        LR"(c:\users\example\appdata\local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)",
        FakeFileSystem::File{.size = 4096});
    RelinkOperation operation = machine.relink(
        "Ubuntu", LR"(c:\users\example\appdata\local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)");

    const auto plan = operation.plan();

    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().message.find("already points at") != std::string::npos);
}

TEST_CASE("relink refuses a distribution that is running", "[ops][relink]") {
    Machine machine;
    machine.host.set_running({"Moved-Away"});
    RelinkOperation operation = machine.relink("Moved-Away");

    const auto plan = operation.plan();

    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::DistroBusy);
    CHECK(plan.error().remedy.find("wsl --terminate Moved-Away") != std::string::npos);
}

TEST_CASE("relink plans a reversible write and a smoke test", "[ops][relink]") {
    Machine machine;
    RelinkOperation operation = machine.relink("Moved-Away");

    const auto plan = operation.plan();

    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 2);
    CHECK(plan->steps[0].mutates);
    CHECK(plan->steps[0].undo_description.has_value());
    // The smoke test only reads, so a failure there does not have to be undone.
    CHECK_FALSE(plan->steps[1].mutates);
    CHECK(plan->warnings.size() == 1);
}

TEST_CASE("relink writes the new location and starts the distribution", "[ops][relink]") {
    Machine machine;
    const Distro distro = machine.distro("Moved-Away");
    RelinkOperation operation = machine.relink("Moved-Away");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK(base_path_of(machine.registry, distro) == LR"(D:\moved\Moved-Away)");

    const auto vhd = machine.registry.read_string(wsldisk::model::registry_key_for(distro), L"VhdFileName");
    REQUIRE(vhd.has_value());
    REQUIRE(vhd->has_value());
    CHECK(**vhd == L"ext4.vhdx");

    // /bin/true does nothing in the guest on purpose: what is being tested is
    // that the distribution boots from the disk the registry now names.
    REQUIRE(machine.host.commands().size() == 1);
    CHECK(machine.host.commands().front().distribution == "Moved-Away");
    CHECK(machine.host.commands().front().argv == std::vector<std::string>{"/bin/true"});
}

TEST_CASE("relink keeps the extended-length form the distribution already used", "[ops][relink]") {
    Machine machine;
    const Distro distro = machine.distro("docker-desktop");
    RelinkOperation operation = machine.relink("docker-desktop");

    CHECK(operation.intended_base_path() == LR"(\\?\D:\moved\Moved-Away)");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    // Normalising this would be an unrequested change to a value Docker
    // Desktop, not this tool, owns.
    CHECK(base_path_of(machine.registry, distro) == LR"(\\?\D:\moved\Moved-Away)");
}

TEST_CASE("relink leaves a legacy entry without a VhdFileName", "[ops][relink]") {
    Machine machine;
    const Distro distro = machine.distro("Ubuntu-20.04");
    RelinkOperation operation = machine.relink("Ubuntu-20.04");

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    // Adding the value to an entry that never had it would change the layout,
    // not repair it.
    const auto vhd = machine.registry.read_string(wsldisk::model::registry_key_for(distro), L"VhdFileName");
    REQUIRE(vhd.has_value());
    CHECK_FALSE(vhd->has_value());
}

TEST_CASE("relink puts the registry back when the distribution will not start", "[ops][relink]") {
    Machine machine;
    const Distro distro = machine.distro("Moved-Away");
    const std::wstring before = base_path_of(machine.registry, distro);
    machine.host.on_command("/bin/true", WslCommandResult{.exit_code = 1});

    RelinkOperation operation = machine.relink("Moved-Away");
    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().remedy.find("put back") != std::string::npos);
    CHECK(base_path_of(machine.registry, distro) == before);
    const auto vhd = machine.registry.read_string(wsldisk::model::registry_key_for(distro), L"VhdFileName");
    REQUIRE(vhd.has_value());
    REQUIRE(vhd->has_value());
    CHECK(**vhd == L"ext4.vhdx");
}

TEST_CASE("relink puts the registry back when wsl.exe will not answer", "[ops][relink]") {
    Machine machine;
    const Distro distro = machine.distro("Moved-Away");
    const std::wstring before = base_path_of(machine.registry, distro);
    // wsl.exe failing to run is not the same as the command inside returning
    // non-zero, and both have to undo the write.
    machine.host.fail_command(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});

    RelinkOperation operation = machine.relink("Moved-Away");
    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(base_path_of(machine.registry, distro) == before);
}

TEST_CASE("relink reports a registry it cannot write", "[ops][relink]") {
    Machine machine;
    RelinkOperation operation = machine.relink("Moved-Away");
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot write", "run as the owning user"});

    const auto outcome = operation.execute(machine.sink);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::NeedsElevation);
    CHECK(machine.host.commands().empty());
}

TEST_CASE("relink reports a VhdFileName it cannot write", "[ops][relink]") {
    Machine machine;
    RelinkOperation operation = machine.relink("Moved-Away");
    // Only the second write fails, so the first one is already on the undo
    // stack when it does.
    machine.registry.fail_write(
        L"VhdFileName", wsldisk::Error{ErrorCode::NeedsElevation, "cannot write", "run as the owning user"});

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.host.commands().empty());
    // The BasePath write went in first and has to come back out.
    CHECK(base_path_of(machine.registry, machine.distro("Moved-Away")) == LR"(D:\gone\wsl\Moved-Away)");
}

TEST_CASE("relink verifies what it wrote", "[ops][relink]") {
    Machine machine;
    const Distro distro = machine.distro("Moved-Away");
    RelinkOperation operation = machine.relink("Moved-Away");

    REQUIRE(operation.execute(machine.sink).has_value());
    CHECK(operation.verify().has_value());

    // Something else moved it afterwards: verify has to notice.
    REQUIRE(machine.registry
                .write_string(wsldisk::model::registry_key_for(distro), L"BasePath", LR"(E:\elsewhere)")
                .has_value());
    const auto status = operation.verify();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Partial);
}

TEST_CASE("relink reports a registry it cannot read back", "[ops][relink]") {
    Machine machine;
    RelinkOperation operation = machine.relink("Moved-Away");

    REQUIRE(operation.execute(machine.sink).has_value());
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});

    const auto status = operation.verify();

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("relink verify fails before anything is written", "[ops][relink]") {
    Machine machine;
    const Distro distro = machine.distro("Moved-Away");
    RelinkOperation operation = machine.relink("Moved-Away");

    // Verify asks whether the intended value is there, not whether execute ran.
    const auto status = operation.verify();

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Partial);
    CHECK(base_path_of(machine.registry, distro) == LR"(D:\gone\wsl\Moved-Away)");
}

TEST_CASE("relink changes nothing on a dry run", "[ops][relink]") {
    Machine machine;
    const Distro distro = machine.distro("Moved-Away");
    RelinkOperation operation = machine.relink("Moved-Away");

    const auto outcome = run(operation, machine.sink, RunOptions{.dry_run = true});

    REQUIRE(outcome.has_value());
    CHECK(machine.registry.writes().empty());
    CHECK(machine.host.commands().empty());
    CHECK(base_path_of(machine.registry, distro) == LR"(D:\gone\wsl\Moved-Away)");
}

TEST_CASE("relink survives a running check it cannot make", "[ops][relink]") {
    Machine machine;
    // wsl.exe not answering the "what is running" question is not a reason to
    // refuse: the smoke test would find out anyway, and a machine with WSL
    // half-installed is exactly the one needing a repair.
    machine.host.fail_running(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});
    RelinkOperation operation = machine.relink("Moved-Away");

    CHECK(operation.plan().has_value());
}
