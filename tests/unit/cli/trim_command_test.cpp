#include "trim_command.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

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
using wsldisk::cli::run_trim;
using wsldisk::cli::Services;
using wsldisk::cli::TrimOptions;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
namespace hives = wsldisk::testing::hives;

namespace {

struct Machine {
    FakeRegistry registry = hives::everything();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    std::ostringstream errors;

    [[nodiscard]] Services services() {
        return Services{.registry = &registry, .filesystem = &filesystem, .disks = &disks, .host = &host};
    }

    [[nodiscard]] int run(const std::string& name, const GlobalOptions& global, std::ostream& out) {
        NullLogger logger{errors};
        return run_trim(services(), TrimOptions{.name = name}, global, logger, out, errors);
    }
};

/// A guest whose fstrim reports the free extent of a 1 TB disk, which is what
/// spike #1 actually measured after freeing 1 GiB.
[[nodiscard]] WslCommandResult trims_a_terabyte() {
    return WslCommandResult{.standard_output = "/: 1078939029504 bytes trimmed\n"};
}

}  // namespace

TEST_CASE("trim says what fstrim offered and that it is not what compaction reclaims", "[cli][trim]") {
    Machine machine;
    machine.host.on_command("/sbin/fstrim", trims_a_terabyte());
    std::ostringstream out;

    const int code = machine.run("Ubuntu", GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str().find("Ubuntu: trimmed. fstrim reported 1004.8 GiB.") != std::string::npos);
    // The figure on its own is three orders of magnitude out. Every place that
    // shows it says so.
    CHECK(out.str().find("not space reclaimed") != std::string::npos);
    CHECK(out.str().find("run `wsldisk compact Ubuntu`") != std::string::npos);
}

TEST_CASE("trim says so when fstrim did not report a figure", "[cli][trim]") {
    Machine machine;
    machine.host.on_command("/sbin/fstrim", WslCommandResult{});
    std::ostringstream out;

    const int code = machine.run("Ubuntu", GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    // Better than a zero, which would read as "nothing was freed".
    CHECK(out.str().find("fstrim did not say how much") != std::string::npos);
    // No figure, so nothing that could be misread as space reclaimed.
    CHECK(out.str().find("not space reclaimed") == std::string::npos);
    CHECK(out.str().find("run `wsldisk compact Ubuntu`") != std::string::npos);
}

TEST_CASE("trim reports itself as one json object", "[cli][trim]") {
    Machine machine;
    machine.host.on_command("/sbin/fstrim", trims_a_terabyte());
    std::ostringstream out;

    const int code = machine.run("Ubuntu", GlobalOptions{.json = true}, out);

    CHECK(code == exit_code_success);
    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object["distribution"] == "Ubuntu");
    CHECK(object["trimmed"] == true);
    // Named for what it is: `bytes_freed` would be read as space reclaimed.
    CHECK(object["bytes_offered"] == 1078939029504ULL);
    CHECK(object["note"].get<std::string>().find("not space reclaimed") != std::string::npos);
}

TEST_CASE("trim json leaves out a figure the guest did not give", "[cli][trim]") {
    Machine machine;
    machine.host.on_command("/sbin/fstrim", WslCommandResult{});
    std::ostringstream out;

    CHECK(machine.run("Ubuntu", GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK_FALSE(object.contains("bytes_offered"));
}

TEST_CASE("trim refuses a WSL1 distribution", "[cli][trim]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run("Legacy-WSL1", GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Preflight));
    CHECK(machine.errors.str().find("--set-version") != std::string::npos);
    CHECK(machine.host.commands().empty());
}

TEST_CASE("trim reports an unknown distribution with the closest match", "[cli][trim]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run("Ubunt", GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::DistroNotFound));
    CHECK(machine.errors.str().find("did you mean Ubuntu?") != std::string::npos);
}

TEST_CASE("trim reports a registry it cannot read", "[cli][trim]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});
    std::ostringstream out;

    CHECK(machine.run("Ubuntu", GlobalOptions{}, out) == exit_code_for(ErrorCode::NeedsElevation));
}

TEST_CASE("trim reports a guest that would not trim", "[cli][trim]") {
    Machine machine;
    machine.host.on_command(
        "/sbin/fstrim",
        WslCommandResult{.exit_code = 1,
                         .standard_error = "fstrim: /: FITRIM ioctl failed: Operation not supported\n"});
    std::ostringstream out;

    const int code = machine.run("Ubuntu", GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Generic));
    CHECK(machine.errors.str().find("FITRIM ioctl failed") != std::string::npos);
}

TEST_CASE("trim --dry-run runs nothing and says what it would do", "[cli][trim]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run("Ubuntu", GlobalOptions{.dry_run = true}, out);

    CHECK(code == exit_code_success);
    CHECK(machine.host.commands().empty());
    CHECK(out.str().find("--dry-run: nothing was changed") != std::string::npos);
    CHECK(out.str().find("/sbin/fstrim / in Ubuntu") != std::string::npos);
}

TEST_CASE("trim reports progress unless the output is json", "[cli][trim]") {
    Machine machine;
    machine.host.on_command("/sbin/fstrim", trims_a_terabyte());

    std::ostringstream plain;
    CHECK(machine.run("Ubuntu", GlobalOptions{}, plain) == exit_code_success);
    CHECK(plain.str().find("run /sbin/fstrim / in Ubuntu") != std::string::npos);

    Machine quiet;
    quiet.host.on_command("/sbin/fstrim", trims_a_terabyte());
    std::ostringstream json;
    CHECK(quiet.run("Ubuntu", GlobalOptions{.json = true}, json) == exit_code_success);
    // A progress line would make stdout unparseable.
    CHECK(json.str().find("run /sbin/fstrim") == std::string::npos);
}
