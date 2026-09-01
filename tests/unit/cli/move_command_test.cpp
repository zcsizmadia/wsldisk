#include "move_command.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>

#include "app.h"
#include "errors.h"
#include "fake_filesystem.h"
#include "fake_registry.h"
#include "fake_virtual_disk.h"
#include "fake_wsl_host.h"
#include "logger.h"
#include "lxss_hives.h"

using wsldisk::ErrorCode;
using wsldisk::exit_code_for;
using wsldisk::exit_code_success;
using wsldisk::VolumeInfo;
using wsldisk::WslCommandResult;
using wsldisk::cli::GlobalOptions;
using wsldisk::cli::MoveOptions;
using wsldisk::cli::NullLogger;
using wsldisk::cli::run_move;
using wsldisk::cli::Services;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

const std::filesystem::path ubuntu_disk =
    LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)";

struct Machine {
    FakeRegistry registry = hives::everything();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    std::ostringstream errors;

    Machine() {
        filesystem.add_file(ubuntu_disk,
                            FakeFileSystem::File{.size = 1024 * gigabyte, .size_on_disk = 12 * gigabyte});
        filesystem.set_volume(LR"(C:\)", VolumeInfo{.filesystem_name = "NTFS",
                                                    .total_bytes = 512 * gigabyte,
                                                    .free_bytes = 100 * gigabyte});
        filesystem.set_volume(LR"(D:\)", VolumeInfo{.filesystem_name = "NTFS",
                                                    .total_bytes = 512 * gigabyte,
                                                    .free_bytes = 400 * gigabyte});
        host.set_running({});
        host.on_command("/bin/sh", WslCommandResult{.exit_code = 0});
    }

    [[nodiscard]] Services services() {
        return Services{.registry = &registry, .filesystem = &filesystem, .disks = &disks, .host = &host};
    }

    [[nodiscard]] int run(const GlobalOptions& global, std::ostream& out,
                          const MoveOptions& options = {.name = "Ubuntu", .destination = R"(D:\WSL)"}) {
        NullLogger logger{errors};
        return run_move(services(), options, global, logger, out, errors);
    }
};

}  // namespace

TEST_CASE("move relocates a disk and says where it went", "[cli][move]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_success);
    CHECK(out.str().find(R"(Ubuntu now lives at D:\WSL\ext4.vhdx)") != std::string::npos);
    // The size, because "it moved" and "it moved twelve gigabytes" are
    // different amounts of reassurance.
    CHECK(out.str().find("12.0 GiB") != std::string::npos);
    CHECK(machine.filesystem.exists(std::filesystem::path{LR"(D:\WSL\ext4.vhdx)"}));
}

TEST_CASE("move --json describes what it did", "[cli][move]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("distribution") == "Ubuntu");
    CHECK(object.at("vhdx_path") == R"(D:\WSL\ext4.vhdx)");
    CHECK(object.at("base_path") == R"(D:\WSL)");
    CHECK(object.at("moved") == true);
    // A rename and a copy take wildly different amounts of time; something
    // scripting this deserves to be able to tell them apart.
    CHECK(object.at("same_volume") == false);
    CHECK(object.at("renamed") == false);
    CHECK(object.at("kept_source") == false);
    CHECK(object.at("size_on_disk") == 12 * gigabyte);
}

TEST_CASE("move --json says nothing else on stdout", "[cli][move]") {
    // Progress lines would corrupt a stdout something else is parsing.
    Machine machine;
    std::ostringstream json;
    CHECK(machine.run(GlobalOptions{.json = true}, json) == exit_code_success);
    CHECK(json.str().find("copy ") == std::string::npos);
    CHECK(json.str().find("now lives at") == std::string::npos);
}

TEST_CASE("move --keep-source says so in the JSON", "[cli][move]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out,
                      MoveOptions{.name = "Ubuntu", .destination = R"(D:\WSL)", .keep_source = true}) ==
          exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("kept_source") == true);
    CHECK(machine.filesystem.exists(ubuntu_disk));
    // Kept means copied, whatever the volumes say.
    CHECK(object.at("renamed") == false);
}

TEST_CASE("move reports a same-volume move as one", "[cli][move]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out,
                      MoveOptions{.name = "Ubuntu", .destination = R"(C:\WSL)"}) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("same_volume") == true);
    CHECK(object.at("renamed") == true);
}

TEST_CASE("move --dry-run changes nothing", "[cli][move]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.dry_run = true}, out) == exit_code_success);

    CHECK(out.str().find("--dry-run") != std::string::npos);
    CHECK(machine.filesystem.exists(ubuntu_disk));
    CHECK_FALSE(machine.filesystem.exists(std::filesystem::path{LR"(D:\WSL\ext4.vhdx)"}));
}

TEST_CASE("move --dry-run --json is machine-readable", "[cli][move]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true, .dry_run = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("dry_run") == true);
}

TEST_CASE("move names a distribution that is not registered", "[cli][move]") {
    Machine machine;
    std::ostringstream out;

    const int code =
        machine.run(GlobalOptions{}, out, MoveOptions{.name = "nope", .destination = R"(D:\WSL)"});

    CHECK(code == exit_code_for(ErrorCode::DistroNotFound));
}

TEST_CASE("move suggests a near miss", "[cli][move]") {
    // Through the shared lookup helper, so the wording matches every other
    // command rather than drifting into a second version of it.
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out, MoveOptions{.name = "ubunto", .destination = R"(D:\WSL)"}) ==
          exit_code_for(ErrorCode::DistroNotFound));
    CHECK(machine.errors.str().find("Ubuntu") != std::string::npos);
}

TEST_CASE("move reports a registry it could not enumerate", "[cli][move]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::Generic, "the Lxss key is gone", "check that WSL is installed"});
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) != exit_code_success);
}

TEST_CASE("move reports a refusal with the operation's exit code", "[cli][move]") {
    Machine machine;
    machine.host.set_running({"Ubuntu"});
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_for(ErrorCode::DistroBusy));
}
