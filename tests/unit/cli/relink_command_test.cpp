#include "relink_command.h"

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
using wsldisk::WslCommandResult;
using wsldisk::cli::GlobalOptions;
using wsldisk::cli::NullLogger;
using wsldisk::cli::RelinkOptions;
using wsldisk::cli::run_relink;
using wsldisk::cli::Services;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

/// Where `Moved-Away` in the canned hives is being pointed.
const std::filesystem::path moved_disk = LR"(D:\moved\ext4.vhdx)";

struct Machine {
    FakeRegistry registry = hives::everything();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    std::ostringstream errors;

    Machine() {
        filesystem.add_directory(moved_disk.parent_path());
        FakeFileSystem::File file;
        file.size = 3 * gigabyte;
        file.size_on_disk = 3 * gigabyte;
        filesystem.add_file(moved_disk, file);
    }

    [[nodiscard]] Services services() {
        return Services{.registry = &registry, .filesystem = &filesystem, .disks = &disks, .host = &host};
    }

    [[nodiscard]] int run(const GlobalOptions& global, std::ostream& out,
                          const RelinkOptions& options = {.name = "Moved-Away",
                                                          .path = R"(D:\moved\ext4.vhdx)"}) {
        NullLogger logger{errors};
        return run_relink(services(), options, global, logger, out, errors);
    }
};

}  // namespace

TEST_CASE("relink points a distribution at a disk", "[cli][relink]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_success);
    CHECK(out.str().find("Moved-Away now points at") != std::string::npos);
    // The smoke test, which is the only thing that proves the new path boots.
    CHECK(machine.host.commands().size() == 1);
}

TEST_CASE("relink --json describes what it wrote", "[cli][relink]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("distribution") == "Moved-Away");
    CHECK(object.at("vhdx_path") == R"(D:\moved\ext4.vhdx)");
    // The directory, not the file: `BasePath` names where the disk lives.
    CHECK(object.at("base_path") == R"(D:\moved)");
    CHECK(object.at("relinked") == true);
}

TEST_CASE("relink --json says nothing else on stdout", "[cli][relink]") {
    // Progress lines would corrupt a stdout something else is parsing, and the
    // human path prints them.
    Machine machine;
    std::ostringstream json;
    CHECK(machine.run(GlobalOptions{.json = true}, json) == exit_code_success);
    CHECK(json.str().find("point Moved-Away at") == std::string::npos);

    Machine plain;
    std::ostringstream text;
    CHECK(plain.run(GlobalOptions{}, text) == exit_code_success);
    CHECK(text.str().find("point Moved-Away at") != std::string::npos);
}

TEST_CASE("relink keeps the extended-length prefix a distribution already used", "[cli][relink]") {
    // Docker Desktop's entry spells its BasePath `\\?\C:\...`; rewriting it as a
    // bare path would be an unrequested change to a value it owns.
    Machine machine;
    std::ostringstream out;

    const RelinkOptions options{.name = "docker-desktop", .path = R"(D:\moved\ext4.vhdx)"};
    CHECK(machine.run(GlobalOptions{.json = true}, out, options) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("base_path") == R"(\\?\D:\moved)");
}

TEST_CASE("relink lists what it would do on a dry run", "[cli][relink]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.dry_run = true}, out) == exit_code_success);
    CHECK(out.str().find("--dry-run: nothing was changed") != std::string::npos);
    CHECK(machine.registry.writes().empty());
}

TEST_CASE("relink reports an unknown distribution", "[cli][relink]") {
    Machine machine;
    std::ostringstream out;

    const RelinkOptions options{.name = "nope", .path = R"(D:\moved\ext4.vhdx)"};
    CHECK(machine.run(GlobalOptions{}, out, options) == exit_code_for(ErrorCode::DistroNotFound));
    CHECK(machine.errors.str().find("wsldisk list") != std::string::npos);
}

TEST_CASE("relink reports a registry it cannot read", "[cli][relink]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_for(ErrorCode::NeedsElevation));
}

TEST_CASE("relink reports a distribution that will not start", "[cli][relink]") {
    Machine machine;
    machine.host.on_command("/bin/sh", WslCommandResult{.exit_code = 1});
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_for(ErrorCode::Preflight));
    CHECK(machine.errors.str().find("put back") != std::string::npos);
}

TEST_CASE("relink passes on the warnings enumeration produced", "[cli][relink]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_success);
    CHECK(machine.errors.str().find("DistributionName") != std::string::npos);
}
