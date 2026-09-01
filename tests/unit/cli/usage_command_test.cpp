#include "usage_command.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
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
using wsldisk::cli::run_usage;
using wsldisk::cli::Services;
using wsldisk::cli::UsageCommandOptions;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;
constexpr std::uint64_t megabyte = 1024ULL * 1024;

[[nodiscard]] WslCommandResult du_says(std::uint64_t bytes, std::string_view path) {
    return WslCommandResult{.exit_code = 0,
                            .standard_output = std::to_string(bytes) + "\t" + std::string{path} + "\n"};
}

struct Machine {
    FakeRegistry registry = hives::everything();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    std::ostringstream errors;

    Machine() {
        host.on_command(
            "/bin/df",
            WslCommandResult{.exit_code = 0,
                             .standard_output = "Filesystem 1B-blocks Used Available Use% Mounted on\n"
                                                "/dev/sdc 1099511627776 21474836480 85899345920 1% /\n"});
        host.on_command(
            "/usr/bin/getent",
            WslCommandResult{.exit_code = 0, .standard_output = "root:x:0:0:root:/root:/bin/bash\n"});
        // Nothing found unless a test says otherwise, which is what an absent
        // path looks like.
        host.on_command("/usr/bin/du", WslCommandResult{.exit_code = 1});
    }

    /// The usual two findings: something wsldisk cannot judge and something it
    /// can.
    void with_findings() {
        host.on_command_for("/usr/bin/du", "/var/lib/docker", du_says(3 * gigabyte, "/var/lib/docker"));
        host.on_command_for("/usr/bin/du", "/var/cache/apt/archives",
                            du_says(200 * megabyte, "/var/cache/apt/archives"));
    }

    [[nodiscard]] Services services() {
        return Services{.registry = &registry, .filesystem = &filesystem, .disks = &disks, .host = &host};
    }

    [[nodiscard]] int run(const GlobalOptions& global, std::ostream& out,
                          const UsageCommandOptions& options = {.name = "Ubuntu"}) {
        NullLogger logger{errors};
        return run_usage(services(), options, global, logger, out, errors);
    }
};

}  // namespace

TEST_CASE("usage prints a table of where the space went", "[cli][usage]") {
    Machine machine;
    machine.with_findings();
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_success);

    CHECK(out.str().find("docker storage") != std::string::npos);
    CHECK(out.str().find("/var/lib/docker") != std::string::npos);
    CHECK(out.str().find("3.0 GiB") != std::string::npos);
    // The comparison that makes the numbers mean something.
    CHECK(out.str().find("of 20.0 GiB the guest reports in use") != std::string::npos);
}

TEST_CASE("usage explains what it cannot judge", "[cli][usage]") {
    // `no` in the clearable column is "wsldisk cannot tell", not "dangerous",
    // and a bare column heading does not say that.
    Machine machine;
    machine.with_findings();
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_success);
    CHECK(out.str().find("cannot judge") != std::string::npos);
}

TEST_CASE("usage says when a row contains another", "[cli][usage]") {
    Machine machine;
    machine.with_findings();
    machine.host.on_command_for("/usr/bin/du", "/var/log", du_says(900 * megabyte, "/var/log"));
    machine.host.on_command_for("/usr/bin/du", "/var/log/journal",
                                du_says(800 * megabyte, "/var/log/journal"));
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_success);
    CHECK(out.str().find("not added twice") != std::string::npos);
}

TEST_CASE("usage says so when it found nothing", "[cli][usage]") {
    // An empty table with a total under it looks like a failure. Saying it
    // plainly does not.
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_success);
    CHECK(out.str().find("nothing in the cache catalogue is using space") != std::string::npos);
}

TEST_CASE("usage as JSON describes every entry", "[cli][usage]") {
    Machine machine;
    machine.with_findings();
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("distribution") == "Ubuntu");
    CHECK(object.at("guest_used") == 20 * gigabyte);
    CHECK(object.at("guest_free") == 80 * gigabyte);
    CHECK(object.at("counted") == 3 * gigabyte + 200 * megabyte);

    const nlohmann::json& entries = object.at("entries");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].at("path") == "/var/lib/docker");
    CHECK(entries[0].at("bytes") == 3 * gigabyte);
    CHECK(entries[0].at("safe") == false);
    CHECK(entries[0].at("contains_others") == false);
    CHECK(entries[0].contains("note"));
}

TEST_CASE("usage as JSON keeps progress off stdout", "[cli][usage]") {
    // The progress lines say which path is being measured, and a stdout
    // something is parsing must not carry them.
    Machine machine;
    machine.with_findings();
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out) == exit_code_success);

    CHECK(out.str().find("measuring") == std::string::npos);
    CHECK(machine.errors.str().find("measuring") != std::string::npos);
    CHECK_NOTHROW(nlohmann::json::parse(out.str()));
}

TEST_CASE("usage in verbose mode says what it is measuring", "[cli][usage]") {
    // On stderr, so `-v` and `--json` can be used together.
    Machine machine;
    machine.with_findings();
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.verbose = true}, out) == exit_code_success);
    CHECK(machine.errors.str().find("measuring /var/lib/docker") != std::string::npos);
}

TEST_CASE("usage with a top limit shortens the table", "[cli][usage]") {
    Machine machine;
    machine.with_findings();
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out, UsageCommandOptions{.name = "Ubuntu", .top = 1}) ==
          exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("entries").size() == 1);
    // The total still counts what was left out.
    CHECK(object.at("counted") == 3 * gigabyte + 200 * megabyte);
}

TEST_CASE("usage on a dry run says there was nothing to withhold", "[cli][usage]") {
    // It reads and nothing else. Refusing the flag would be pedantic; pretending
    // something was held back would be a lie.
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.dry_run = true}, out) == exit_code_success);
    CHECK(out.str().find("only reads") != std::string::npos);
    CHECK(machine.host.commands().empty());
}

TEST_CASE("usage on a dry run as JSON is machine-readable", "[cli][usage]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true, .dry_run = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object.at("dry_run") == true);
}

TEST_CASE("usage names a distribution that is not registered", "[cli][usage]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out, UsageCommandOptions{.name = "nope"}) ==
          exit_code_for(ErrorCode::DistroNotFound));
}

TEST_CASE("usage suggests a near miss", "[cli][usage]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out, UsageCommandOptions{.name = "ubunto"}) ==
          exit_code_for(ErrorCode::DistroNotFound));
    CHECK(machine.errors.str().find("Ubuntu") != std::string::npos);
}

TEST_CASE("usage reports a registry it could not enumerate", "[cli][usage]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::Generic, "the Lxss key is gone", "check that WSL is installed"});
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) != exit_code_success);
}

TEST_CASE("usage reports a guest it could not reach", "[cli][usage]") {
    Machine machine;
    machine.host.fail_command(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check the installation"});
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) != exit_code_success);
}

TEST_CASE("usage refuses a WSL1 distribution", "[cli][usage]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out, UsageCommandOptions{.name = "Legacy-WSL1"}) ==
          exit_code_for(ErrorCode::Preflight));
}

TEST_CASE("usage as JSON omits a note the catalogue does not carry", "[cli][usage]") {
    // Most entries have one; a few do not, and an empty string in the JSON
    // would be a field that says nothing.
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/var/cache/zypp", du_says(megabyte, "/var/cache/zypp"));
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    REQUIRE(object.at("entries").size() == 1);
    CHECK_FALSE(object.at("entries")[0].contains("note"));
}

TEST_CASE("usage as JSON carries the notes it collected", "[cli][usage]") {
    Machine machine;
    machine.host.on_command("/usr/bin/getent", WslCommandResult{.exit_code = 2});
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    REQUIRE(object.contains("notes"));
    CHECK(object.at("notes").size() == 1);
}

TEST_CASE("usage reports the notes it collected", "[cli][usage]") {
    Machine machine;
    machine.host.on_command("/usr/bin/getent", WslCommandResult{.exit_code = 2});
    std::ostringstream out;

    CHECK(machine.run(GlobalOptions{}, out) == exit_code_success);
    CHECK(out.str().find("note: ") != std::string::npos);
}
