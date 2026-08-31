#include "orphans_command.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "errors.h"
#include "fake_filesystem.h"
#include "fake_registry.h"
#include "fake_virtual_disk.h"
#include "fake_wsl_host.h"
#include "golden.h"
#include "logger.h"
#include "lxss_hives.h"

using wsldisk::ErrorCode;
using wsldisk::exit_code_for;
using wsldisk::exit_code_success;
using wsldisk::WslCommandResult;
using wsldisk::cli::ask;
using wsldisk::cli::Confirm;
using wsldisk::cli::console_confirm;
using wsldisk::cli::delete_orphans;
using wsldisk::cli::GlobalOptions;
using wsldisk::cli::NullLogger;
using wsldisk::cli::OrphansOptions;
using wsldisk::cli::run_orphans;
using wsldisk::cli::scan_orphans;
using wsldisk::cli::Services;
using wsldisk::cli::StreamLogger;
using wsldisk::model::Orphan;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
using wsldisk::testing::Golden;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

const std::filesystem::path ubuntu_disk =
    LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)";
const std::filesystem::path stale_disk = LR"(C:\Users\example\AppData\Local\wsl\Removed-Distro\ext4.vhdx)";
const std::filesystem::path docker_data =
    LR"(C:\Users\example\AppData\Local\Docker\wsl\disk\docker_data.vhdx)";

/// A machine wired from the canned hives, with `%LOCALAPPDATA%` pointing at the
/// tree those hives describe so the default scan lands on it.
struct Machine {
    FakeRegistry registry = hives::measured();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    std::ostringstream errors;

    Machine() {
        filesystem.set_variable(L"LOCALAPPDATA", LR"(C:\Users\example\AppData\Local)");
        filesystem.add_directory(ubuntu_disk.parent_path());
        add_disk(ubuntu_disk, 14 * gigabyte);
        filesystem.add_directory(LR"(C:\Users\example\AppData\Local\Docker\wsl\main)");
        add_disk(LR"(C:\Users\example\AppData\Local\Docker\wsl\main\ext4.vhdx)", 2 * gigabyte);
    }

    void add_disk(const std::filesystem::path& path, std::uint64_t size) {
        FakeFileSystem::File file;
        file.size = size;
        file.size_on_disk = size;
        filesystem.add_file(path, file);
    }

    /// A disk in the scanned tree that nothing in the registry claims.
    void add_orphan(const std::filesystem::path& path, std::uint64_t size) {
        filesystem.add_directory(path.parent_path());
        add_disk(path, size);
    }

    [[nodiscard]] Services services() {
        return Services{.registry = &registry, .filesystem = &filesystem, .disks = &disks, .host = &host};
    }

    [[nodiscard]] int run(const OrphansOptions& options, const GlobalOptions& global, std::ostream& out,
                          const Confirm& confirm = refuse) {
        NullLogger logger{errors};
        return run_orphans(services(), options, global, logger, confirm, out, errors);
    }

    /// The default answer in a test: nothing is deleted unless a test says so.
    static bool refuse(std::string_view /*question*/) { return false; }

    static bool accept(std::string_view /*question*/) { return true; }
};

/// Everything `run_orphans` wrote to stdout.
[[nodiscard]] std::string output_of(Machine& machine, const OrphansOptions& options,
                                    const GlobalOptions& global = {},
                                    const Confirm& confirm = Machine::refuse) {
    std::ostringstream out;
    const int code = machine.run(options, global, out, confirm);
    CHECK(code == exit_code_success);
    return out.str();
}

}  // namespace

TEST_CASE("ask treats anything but yes as no", "[cli][orphans]") {
    for (const std::string answer : {"y", "Y", "yes", "YES", "Yes"}) {
        std::istringstream in{answer};
        std::ostringstream out;
        CHECK(ask(in, out, "delete?"));
    }
    for (const std::string answer : {"n", "no", "", "yep", "sure", "yes please"}) {
        std::istringstream in{answer};
        std::ostringstream out;
        CHECK_FALSE(ask(in, out, "delete?"));
    }
}

TEST_CASE("ask says no when there is nothing to answer with", "[cli][orphans]") {
    // A piped command with a closed stdin. Silence is not consent.
    std::istringstream in;
    in.setstate(std::ios::eofbit);
    std::ostringstream out;

    CHECK_FALSE(ask(in, out, "delete?"));
    CHECK(out.str().find("[y/N]") != std::string::npos);
}

TEST_CASE("scan_orphans leaves the disks a distribution claims alone", "[cli][orphans]") {
    Machine machine;
    NullLogger logger{machine.errors};

    const auto orphans = scan_orphans(machine.services(), OrphansOptions{}, logger);

    REQUIRE(orphans.has_value());
    CHECK(orphans->empty());
}

TEST_CASE("scan_orphans finds a disk nothing claims", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);
    NullLogger logger{machine.errors};

    const auto orphans = scan_orphans(machine.services(), OrphansOptions{}, logger);

    REQUIRE(orphans.has_value());
    REQUIRE(orphans->size() == 1);
    CHECK(orphans->front().path == stale_disk);
}

TEST_CASE("scan_orphans searches the extra directories it is given", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(LR"(D:\backups\ext4.vhdx)", gigabyte);
    NullLogger logger{machine.errors};

    const OrphansOptions options{.scan_dirs = {R"(D:\backups)"}};
    const auto orphans = scan_orphans(machine.services(), options, logger);

    REQUIRE(orphans.has_value());
    REQUIRE(orphans->size() == 1);
    CHECK(orphans->front().path == std::filesystem::path{LR"(D:\backups\ext4.vhdx)"});
}

TEST_CASE("scan_orphans reports a registry it cannot read", "[cli][orphans]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});
    NullLogger logger{machine.errors};

    const auto orphans = scan_orphans(machine.services(), OrphansOptions{}, logger);

    REQUIRE_FALSE(orphans.has_value());
    CHECK(orphans.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("scan_orphans reports an environment it cannot expand", "[cli][orphans]") {
    Machine machine;
    machine.filesystem.fail_queries(
        wsldisk::Error{ErrorCode::Preflight, "no environment", "check the shell"});
    NullLogger logger{machine.errors};

    const auto orphans = scan_orphans(machine.services(), OrphansOptions{}, logger);

    REQUIRE_FALSE(orphans.has_value());
    CHECK(orphans.error().code == ErrorCode::Preflight);
}

TEST_CASE("scan_orphans logs a directory it could not read as detail", "[cli][orphans]") {
    Machine machine;
    machine.filesystem.fail_directory(
        LR"(D:\backups)", wsldisk::Error{ErrorCode::Preflight, "no such directory", "nothing to do"});
    std::ostringstream errors;
    StreamLogger logger{errors, true, {}};

    const OrphansOptions options{.scan_dirs = {R"(D:\backups)"}};
    const auto orphans = scan_orphans(machine.services(), options, logger);

    // Verbose rather than a warning: a scan pattern naming a directory this
    // machine does not have is the normal case, not a problem.
    REQUIRE(orphans.has_value());
    CHECK(errors.str().find("no such directory") != std::string::npos);
}

TEST_CASE("scan_orphans passes on the warnings enumeration produced", "[cli][orphans]") {
    Machine machine;
    machine.registry = hives::everything();
    NullLogger logger{machine.errors};

    const auto orphans = scan_orphans(machine.services(), OrphansOptions{}, logger);

    REQUIRE(orphans.has_value());
    // The nameless key. A broken entry is reported, not fatal.
    CHECK(machine.errors.str().find("DistributionName") != std::string::npos);
}

TEST_CASE("orphans says so when it finds nothing", "[cli][orphans]") {
    Machine machine;

    CHECK(output_of(machine, OrphansOptions{}) == "no orphaned disks found\n");
}

TEST_CASE("orphans renders what it found", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);
    machine.add_orphan(docker_data, 68 * gigabyte);

    Golden{"orphans-table.txt"}.check(output_of(machine, OrphansOptions{}));
}

TEST_CASE("orphans renders one json object per disk", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);

    const std::string text = output_of(machine, OrphansOptions{}, GlobalOptions{.json = true});

    const nlohmann::json object = nlohmann::json::parse(text);
    CHECK(object["path"] == stale_disk.string());
    CHECK(object["size_on_disk"] == 3 * gigabyte);
}

TEST_CASE("orphans json prints nothing when there is nothing", "[cli][orphans]") {
    Machine machine;

    CHECK(output_of(machine, OrphansOptions{}, GlobalOptions{.json = true}).empty());
}

TEST_CASE("orphans json leaves out a size it could not read", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);
    machine.filesystem.fail_size_on_disk(
        wsldisk::Error{ErrorCode::Preflight, "no such file", "re-run the scan"});

    const std::string text = output_of(machine, OrphansOptions{}, GlobalOptions{.json = true});

    const nlohmann::json object = nlohmann::json::parse(text);
    CHECK(object["path"] == stale_disk.string());
    CHECK_FALSE(object.contains("size_on_disk"));
}

TEST_CASE("orphans reports a scan it could not do", "[cli][orphans]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});
    std::ostringstream out;

    const int code = machine.run(OrphansOptions{}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::NeedsElevation));
}

TEST_CASE("orphans --delete says so when there is nothing to delete", "[cli][orphans]") {
    Machine machine;

    CHECK(output_of(machine, OrphansOptions{.remove = true}) == "nothing to delete\n");
}

TEST_CASE("orphans --delete warns before it asks", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(docker_data, 68 * gigabyte);

    Golden{"orphans-delete-prompt.txt"}.check(output_of(machine, OrphansOptions{.remove = true}));
    // Refused, so nothing went.
    CHECK(machine.filesystem.removed().empty());
}

TEST_CASE("orphans --delete deletes once the answer is yes", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);

    const std::string text =
        output_of(machine, OrphansOptions{.remove = true}, GlobalOptions{}, Machine::accept);

    CHECK(text.find("deleted " + stale_disk.string()) != std::string::npos);
    CHECK(machine.filesystem.removed() == std::vector<std::wstring>{stale_disk.wstring()});
}

TEST_CASE("orphans --delete --yes does not ask", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);

    bool asked = false;
    const Confirm confirm = [&asked](std::string_view) {
        asked = true;
        return false;
    };
    std::ostringstream out;
    const int code =
        machine.run(OrphansOptions{.remove = true}, GlobalOptions{.assume_yes = true}, out, confirm);

    CHECK(code == exit_code_success);
    CHECK_FALSE(asked);
    CHECK(machine.filesystem.removed().size() == 1);
}

TEST_CASE("orphans --delete --dry-run deletes nothing and never asks", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);

    bool asked = false;
    const Confirm confirm = [&asked](std::string_view) {
        asked = true;
        return true;
    };
    std::ostringstream out;
    const int code =
        machine.run(OrphansOptions{.remove = true}, GlobalOptions{.dry_run = true}, out, confirm);

    CHECK(code == exit_code_success);
    CHECK_FALSE(asked);
    CHECK(machine.filesystem.removed().empty());
    CHECK(out.str().find("--dry-run: nothing was deleted") != std::string::npos);
}

TEST_CASE("orphans --delete refuses a disk something else has open", "[cli][orphans]") {
    // Docker Desktop keeps a docker_data.vhdx that no distribution claims and
    // that holds every volume the user has. Deleting it would be catastrophic
    // and the registry says nothing about it either way.
    Machine machine;
    machine.add_orphan(docker_data, 68 * gigabyte);
    machine.filesystem.lock_file(docker_data);
    std::ostringstream out;

    const int code =
        machine.run(OrphansOptions{.remove = true}, GlobalOptions{.assume_yes = true}, out, Machine::accept);

    CHECK(code == exit_code_for(ErrorCode::DistroBusy));
    CHECK(machine.filesystem.removed().empty());
    CHECK(machine.errors.str().find("in use by another process") != std::string::npos);
}

TEST_CASE("orphans --delete reports a lock check it could not make", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);
    // The file is listed by the scan and gone by the time the lock is checked.
    std::ostringstream out;
    const auto orphans = std::vector<Orphan>{Orphan{.path = LR"(D:\vanished\ext4.vhdx)"}};

    const int code = delete_orphans(machine.services(), orphans, GlobalOptions{.assume_yes = true},
                                    Machine::accept, out, machine.errors);

    CHECK(code == exit_code_for(ErrorCode::DistroBusy));
    CHECK(machine.filesystem.removed().empty());
}

TEST_CASE("orphans --delete keeps going past a file it cannot delete", "[cli][orphans]") {
    Machine machine;
    machine.add_orphan(stale_disk, 3 * gigabyte);
    machine.filesystem.fail_remove(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot delete", "run as the owning user"});
    std::ostringstream out;

    const int code =
        machine.run(OrphansOptions{.remove = true}, GlobalOptions{.assume_yes = true}, out, Machine::accept);

    CHECK(code == exit_code_for(ErrorCode::DistroBusy));
    CHECK(machine.errors.str().find("cannot delete") != std::string::npos);
}

TEST_CASE("orphans --relink is the relink command reached from the other side", "[cli][orphans]") {
    // The behaviour lives in relink_command_test.cpp, next to the code. What
    // this checks is the delegation: that `--relink` still runs the operation
    // rather than falling through to the scan.
    Machine machine;
    machine.registry = hives::everything();
    machine.add_orphan(LR"(D:\moved\ext4.vhdx)", 3 * gigabyte);
    std::ostringstream out;

    const OrphansOptions options{.relink_distro = "Moved-Away", .relink_path = R"(D:\moved\ext4.vhdx)"};
    const int code = machine.run(options, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str().find("Moved-Away now points at") != std::string::npos);
    // The smoke test ran, and nothing was scanned.
    CHECK(machine.host.commands().size() == 1);
    CHECK(out.str().find("orphan") == std::string::npos);
}

TEST_CASE("console_confirm reads the answer from the stream it was given", "[cli][orphans]") {
    // What `wsldisk orphans --delete` uses in production, with std::cin. The
    // only part of the command that is not driven from an interface.
    std::istringstream yes{"y"};
    std::istringstream no{"n"};
    std::ostringstream out;

    CHECK(console_confirm(yes, out)("delete 1 file(s)?"));
    CHECK_FALSE(console_confirm(no, out)("delete 1 file(s)?"));
    CHECK(out.str().find("delete 1 file(s)? [y/N]") != std::string::npos);
}

TEST_CASE("OrphansOptions knows when it is relinking", "[cli][orphans]") {
    CHECK_FALSE(OrphansOptions{}.relinking());
    CHECK(OrphansOptions{.relink_distro = "Ubuntu"}.relinking());
}
