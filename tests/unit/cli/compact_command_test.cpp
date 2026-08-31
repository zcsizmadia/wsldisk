#include "compact_command.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "errors.h"
#include "fake_clock.h"
#include "fake_filesystem.h"
#include "fake_registry.h"
#include "fake_virtual_disk.h"
#include "fake_wsl_host.h"
#include "logger.h"
#include "lxss_hives.h"

using wsldisk::ErrorCode;
using wsldisk::exit_code_for;
using wsldisk::exit_code_success;
using wsldisk::cli::CompactCommandOptions;
using wsldisk::cli::GlobalOptions;
using wsldisk::cli::NullLogger;
using wsldisk::cli::run_compact;
using wsldisk::cli::Services;
using wsldisk::testing::FakeClock;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

const std::filesystem::path ubuntu_disk =
    LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)";
const std::filesystem::path docker_disk = LR"(C:\Users\example\AppData\Local\Docker\wsl\main\ext4.vhdx)";

struct Machine {
    FakeRegistry registry = hives::measured();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    FakeClock clock;
    std::ostringstream errors;

    Machine() {
        set_size(ubuntu_disk, 14 * gigabyte, 9 * gigabyte);
        set_size(docker_disk, 4 * gigabyte, 3 * gigabyte);
        // A compaction shrinks the file on the host volume, which is the number
        // the command reports.
        disks.on_compact(
            [this](const std::filesystem::path& path, std::uint64_t size) { set_size(path, size, size); });
    }

    void set_size(const std::filesystem::path& path, std::uint64_t size, std::uint64_t after) {
        FakeFileSystem::File file;
        file.size = size;
        file.size_on_disk = size;
        filesystem.add_file(path, file);

        FakeVirtualDisk::Disk disk;
        disk.info.virtual_size = 1024 * gigabyte;
        disk.info.physical_size = size;
        disk.physical_size_after_compact = after;
        disks.add_disk(path, disk);
    }

    /// The settings `config.toml` would have supplied. Defaults unless a test
    /// says otherwise, which is what every existing case relies on.
    wsldisk::model::Config config;

    [[nodiscard]] Services services() {
        return Services{.registry = &registry,
                        .filesystem = &filesystem,
                        .disks = &disks,
                        .host = &host,
                        .clock = &clock,
                        .config = config};
    }

    [[nodiscard]] int run(const CompactCommandOptions& options, const GlobalOptions& global,
                          std::ostream& out) {
        NullLogger logger{errors};
        return run_compact(services(), options, global, logger, out, errors);
    }
};

}  // namespace

TEST_CASE("compact needs exactly one target", "[cli][compact]") {
    CHECK_FALSE(CompactCommandOptions{}.targets_one_thing());
    CHECK(CompactCommandOptions{.name = "Ubuntu"}.targets_one_thing());
    CHECK(CompactCommandOptions{.all = true}.targets_one_thing());
    CHECK(CompactCommandOptions{.file = R"(D:\a.vhdx)"}.targets_one_thing());
    CHECK_FALSE(CompactCommandOptions{.name = "Ubuntu", .all = true}.targets_one_thing());
    CHECK_FALSE(CompactCommandOptions{.all = true, .file = R"(D:\a.vhdx)"}.targets_one_thing());
}

TEST_CASE("compact with no target is a usage error", "[cli][compact]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Usage));
    CHECK(machine.errors.str().find("name one distribution") != std::string::npos);
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact reports what it reclaimed", "[cli][compact]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str().find("Ubuntu: 5.0 GiB reclaimed (14.0 GiB to 9.0 GiB)") != std::string::npos);
    CHECK(machine.disks.compacted().size() == 1);
}

TEST_CASE("compact reports itself as one json object", "[cli][compact]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{.json = true}, out);

    CHECK(code == exit_code_success);
    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object["target"] == "Ubuntu");
    CHECK(object["compacted"] == true);
    CHECK(object["size_before"] == 14 * gigabyte);
    CHECK(object["size_after"] == 9 * gigabyte);
    CHECK(object["reclaimed"] == 5 * gigabyte);
}

TEST_CASE("compact refuses a WSL1 distribution", "[cli][compact]") {
    Machine machine;
    machine.registry = hives::everything();
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.name = "Legacy-WSL1"}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Preflight));
    CHECK(machine.errors.str().find("--set-version") != std::string::npos);
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact reports an unknown distribution with the closest match", "[cli][compact]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.name = "Ubunt"}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::DistroNotFound));
    CHECK(machine.errors.str().find("did you mean Ubuntu?") != std::string::npos);
}

TEST_CASE("compact reports a registry it cannot read", "[cli][compact]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});
    std::ostringstream out;

    CHECK(machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{}, out) ==
          exit_code_for(ErrorCode::NeedsElevation));
}

TEST_CASE("compact --all does every WSL2 distribution and totals them", "[cli][compact]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.all = true}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(machine.disks.compacted().size() == 2);
    CHECK(out.str().find("6.0 GiB reclaimed in total") != std::string::npos);
}

TEST_CASE("compact --all skips WSL1 rather than failing on it", "[cli][compact]") {
    // A WSL1 distribution has no virtual disk. One refusal per WSL1 entry on
    // every run would be noise, so it is verbose detail instead.
    Machine machine;
    machine.registry = hives::everything();
    std::ostringstream out;
    std::ostringstream errors;
    wsldisk::cli::StreamLogger logger{errors, true, {}};

    static_cast<void>(run_compact(machine.services(), CompactCommandOptions{.all = true}, GlobalOptions{},
                                  logger, out, machine.errors));

    // Moved-Away's disk is not there, so that one fails; Ubuntu and
    // docker-desktop compact. What matters here is that WSL1 was skipped as
    // detail rather than reported as a failure.
    CHECK(errors.str().find("Legacy-WSL1 is WSL1") != std::string::npos);
    CHECK(machine.errors.str().find("Legacy-WSL1") == std::string::npos);
}

TEST_CASE("compact --all keeps going past one that fails", "[cli][compact]") {
    // Stopping at the first failure leaves the user to work out how far it got.
    Machine machine;
    machine.registry = hives::everything();
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.all = true}, GlobalOptions{}, out);

    // Moved-Away points at a disk that is not there.
    CHECK(code == exit_code_for(ErrorCode::Preflight));
    CHECK(machine.disks.compacted().size() == 2);
    CHECK(machine.errors.str().find("Moved-Away") != std::string::npos);
}

TEST_CASE("compact --all says so when there is nothing to do", "[cli][compact]") {
    Machine machine;
    machine.registry = hives::empty();
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.all = true}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str() == "no WSL2 distributions to compact\n");
}

TEST_CASE("compact --file compacts a disk no distribution claims", "[cli][compact]") {
    Machine machine;
    machine.set_size(LR"(D:\disks\docker_data.vhdx)", 68 * gigabyte, 20 * gigabyte);
    std::ostringstream out;

    const int code =
        machine.run(CompactCommandOptions{.file = R"(D:\disks\docker_data.vhdx)"}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str().find("48.0 GiB reclaimed") != std::string::npos);
    // No guest to trim and nothing to stop.
    CHECK(machine.host.commands().empty());
    CHECK(machine.host.terminated().empty());
}

TEST_CASE("compact --file refuses a disk that is not there", "[cli][compact]") {
    Machine machine;
    std::ostringstream out;

    const int code =
        machine.run(CompactCommandOptions{.file = R"(D:\disks\gone.vhdx)"}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Preflight));
    CHECK(machine.errors.str().find("does not exist") != std::string::npos);
}

TEST_CASE("compact --file refuses one something else has open", "[cli][compact]") {
    Machine machine;
    machine.set_size(LR"(D:\disks\docker_data.vhdx)", 68 * gigabyte, 20 * gigabyte);
    machine.filesystem.lock_file(LR"(D:\disks\docker_data.vhdx)");
    std::ostringstream out;

    const int code =
        machine.run(CompactCommandOptions{.file = R"(D:\disks\docker_data.vhdx)"}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::DistroBusy));
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact --no-trim runs nothing in the guest", "[cli][compact]") {
    Machine machine;
    std::ostringstream out;

    const int code =
        machine.run(CompactCommandOptions{.name = "Ubuntu", .no_trim = true}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(machine.host.commands().empty());
    CHECK(machine.disks.compacted().size() == 1);
}

TEST_CASE("compact refuses rather than shutting WSL down on its own", "[cli][compact]") {
    // Decision D9: the utility VM holds every disk while any distribution runs,
    // so releasing one means stopping them all -- which the user has to ask for.
    Machine machine;
    machine.host.set_running({"Ubuntu", "docker-desktop"});
    machine.filesystem.lock_file(ubuntu_disk);
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::DistroBusy));
    CHECK(machine.host.shutdowns() == 0);
    CHECK(machine.errors.str().find("--shutdown") != std::string::npos);
}

TEST_CASE("compact --shutdown stops everything", "[cli][compact]") {
    Machine machine;
    machine.host.set_running({"Ubuntu", "docker-desktop"});
    std::ostringstream out;

    const int code =
        machine.run(CompactCommandOptions{.name = "Ubuntu", .shutdown = true}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(machine.host.shutdowns() == 1);
}

TEST_CASE("compact --dry-run changes nothing and says what it would do", "[cli][compact]") {
    Machine machine;
    std::ostringstream out;

    const int code =
        machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{.dry_run = true}, out);

    CHECK(code == exit_code_success);
    CHECK(machine.disks.compacted().empty());
    CHECK(machine.host.commands().empty());
    CHECK(out.str().find("--dry-run: nothing was changed") != std::string::npos);
    CHECK(out.str().find("run fstrim in Ubuntu") != std::string::npos);
    CHECK(out.str().find("compact ") != std::string::npos);
}

TEST_CASE("compact --file --dry-run changes nothing", "[cli][compact]") {
    Machine machine;
    machine.set_size(LR"(D:\disks\docker_data.vhdx)", 68 * gigabyte, 20 * gigabyte);
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.file = R"(D:\disks\docker_data.vhdx)"},
                                 GlobalOptions{.dry_run = true}, out);

    CHECK(code == exit_code_success);
    CHECK(machine.disks.compacted().empty());
    CHECK(out.str().find("--dry-run: nothing was changed") != std::string::npos);
}

TEST_CASE("compact --dry-run reports one that would refuse", "[cli][compact]") {
    Machine machine;
    machine.registry = hives::everything();
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.all = true}, GlobalOptions{.dry_run = true}, out);

    // Moved-Away's disk is not there, and a dry run that reported success would
    // be telling the user something that is not going to happen.
    CHECK(code == exit_code_for(ErrorCode::Preflight));
    CHECK(machine.errors.str().find("Moved-Away") != std::string::npos);
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact reports a compaction that failed", "[cli][compact]") {
    Machine machine;
    machine.disks.fail_compact(
        wsldisk::Error{ErrorCode::Generic, "the compaction failed", "try again after a shutdown"});
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Generic));
    CHECK(machine.errors.str().find("the compaction failed") != std::string::npos);
}

TEST_CASE("compact json reports a failure as an object too", "[cli][compact]") {
    Machine machine;
    machine.disks.fail_compact(
        wsldisk::Error{ErrorCode::Generic, "the compaction failed", "try again after a shutdown"});
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{.json = true}, out);

    CHECK(code == exit_code_for(ErrorCode::Generic));
    // The promise `--json` makes: stdout is parseable whatever happened.
    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object["compacted"] == false);
    CHECK(object["error"] == "the compaction failed");
    CHECK(object["exit_code"] == exit_code_for(ErrorCode::Generic));
}

TEST_CASE("compact says so when it could not measure the saving", "[cli][compact]") {
    Machine machine;
    machine.filesystem.fail_size_on_disk(
        wsldisk::Error{ErrorCode::Preflight, "no such file", "check the path"});
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    // Better than printing a number that was never taken.
    CHECK(out.str().find("Its size could not be measured") != std::string::npos);
}

TEST_CASE("compact reports progress unless the output is json", "[cli][compact]") {
    Machine machine;
    std::ostringstream plain;
    CHECK(machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{}, plain) == exit_code_success);
    CHECK(plain.str().find("run fstrim in Ubuntu") != std::string::npos);

    Machine quiet;
    std::ostringstream json;
    CHECK(quiet.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{.json = true}, json) ==
          exit_code_success);
    // A progress line would make stdout unparseable.
    CHECK(json.str().find("run fstrim") == std::string::npos);
}

TEST_CASE("compact --restart starts the distribution again", "[cli][compact]") {
    Machine machine;
    machine.host.set_running({"Ubuntu"});
    std::ostringstream out;

    const int code =
        machine.run(CompactCommandOptions{.name = "Ubuntu", .restart = true}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    // fstrim, then the restart.
    REQUIRE(machine.host.commands().size() == 2);
    CHECK(machine.host.commands()[1].argv == std::vector<std::string>{"/bin/true"});
}

TEST_CASE("compact --all reports a registry it cannot read", "[cli][compact]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.all = true}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::NeedsElevation));
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact --file reports itself as json too", "[cli][compact]") {
    Machine machine;
    machine.set_size(LR"(D:\disks\docker_data.vhdx)", 68 * gigabyte, 20 * gigabyte);
    std::ostringstream out;

    const int code = machine.run(CompactCommandOptions{.file = R"(D:\disks\docker_data.vhdx)"},
                                 GlobalOptions{.json = true}, out);

    CHECK(code == exit_code_success);
    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object["target"] == R"(D:\disks\docker_data.vhdx)");
    CHECK(object["reclaimed"] == 48 * gigabyte);
}

// The settings below were parsed, validated, written by `config set` and shown
// by `config` for the whole of M1 while nothing read them: `load_config` had
// exactly one caller, `config_command.cpp`. `wsldisk config set compact.restart
// true` is a README example that did nothing. These assert the values reach the
// operation, which is the part that was missing rather than the parsing.

TEST_CASE("compact.restart in the config restarts without the flag", "[cli][compact][config]") {
    Machine machine;
    machine.config.compact_restart = true;
    machine.host.set_running({"Ubuntu"});
    std::ostringstream out;

    CHECK(machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{}, out) == exit_code_success);

    const auto& commands = machine.host.commands();
    const bool restarted = std::ranges::any_of(commands, [](const auto& invocation) {
        return invocation.distribution == "Ubuntu" && !invocation.argv.empty() &&
               invocation.argv.front() == "/bin/true";
    });
    CHECK(restarted);
}

TEST_CASE("compact.trim false in the config skips the trim", "[cli][compact][config]") {
    Machine machine;
    machine.config.compact_trim = false;
    std::ostringstream out;

    CHECK(machine.run(CompactCommandOptions{.name = "Ubuntu"}, GlobalOptions{}, out) == exit_code_success);

    const auto& commands = machine.host.commands();
    const bool trimmed = std::ranges::any_of(commands, [](const auto& invocation) {
        return !invocation.argv.empty() && invocation.argv.front().find("fstrim") != std::string::npos;
    });
    CHECK_FALSE(trimmed);
}

TEST_CASE("the --no-trim flag still wins over a config that asks for trimming", "[cli][compact][config]") {
    // Both flags are one-way, which is what lets the command fold them into the
    // configured defaults without asking CLI11 whether they were given.
    Machine machine;
    machine.config.compact_trim = true;
    std::ostringstream out;

    CHECK(machine.run(CompactCommandOptions{.name = "Ubuntu", .no_trim = true}, GlobalOptions{}, out) ==
          exit_code_success);

    const bool trimmed = std::ranges::any_of(machine.host.commands(), [](const auto& invocation) {
        return !invocation.argv.empty() && invocation.argv.front().find("fstrim") != std::string::npos;
    });
    CHECK_FALSE(trimmed);
}
