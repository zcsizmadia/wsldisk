#include "list_command.h"

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
#include "golden.h"
#include "logger.h"
#include "lxss_hives.h"

using wsldisk::AllocatedRange;
using wsldisk::ErrorCode;
using wsldisk::VirtualDiskInfo;
using wsldisk::cli::gather;
using wsldisk::cli::ListOptions;
using wsldisk::cli::ListRow;
using wsldisk::cli::NullLogger;
using wsldisk::cli::render_json;
using wsldisk::cli::render_table;
using wsldisk::cli::Services;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
using wsldisk::testing::Golden;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

/// A machine wired from the canned hives, with disks on the filesystem for the
/// distributions that have one.
struct Machine {
    FakeRegistry registry = hives::measured();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    std::ostringstream errors;

    /// Gives `path` a disk of `size`, occupying `on_disk`.
    void with_disk(const std::filesystem::path& path, std::uint64_t on_disk) {
        FakeFileSystem::File file;
        file.size = on_disk;
        file.size_on_disk = on_disk;
        file.sparse = true;
        file.ranges.push_back(AllocatedRange{.offset = 0, .length = on_disk});
        filesystem.add_file(path, file);

        FakeVirtualDisk::Disk disk;
        disk.info.virtual_size = 1024 * gigabyte;
        disk.info.physical_size = on_disk;
        disks.add_disk(path, disk);
    }

    [[nodiscard]] Services services() {
        return Services{.registry = &registry, .filesystem = &filesystem, .disks = &disks, .host = &host};
    }

    [[nodiscard]] wsldisk::Result<std::vector<ListRow>> run(const ListOptions& options = {}) {
        NullLogger logger{errors};
        return gather(services(), options, logger);
    }
};

/// The `measured` hive with both disks present and Ubuntu running.
Machine ordinary() {
    Machine machine;
    machine.with_disk(
        LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)",
        14 * gigabyte);
    machine.with_disk(LR"(C:\Users\example\AppData\Local\Docker\wsl\main\ext4.vhdx)", 2 * gigabyte);
    machine.host.set_running({"Ubuntu"});

    wsldisk::WslCommandResult df;
    df.exit_code = 0;
    df.standard_output =
        "Filesystem 1B-blocks Used Available Use% Mounted on\n/dev/sdc 1099511627776 8589934592 "
        "1090921693184 1% /\n";
    machine.host.on_command("/bin/df", df);
    return machine;
}

std::string table_of(const std::vector<ListRow>& rows) {
    std::ostringstream out;
    render_table(rows, out);
    return out.str();
}

}  // namespace

TEST_CASE("list reports every registered distribution", "[cli][list]") {
    Machine machine = ordinary();

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    REQUIRE(rows->size() == 2);
    CHECK((*rows)[0].distro.name == "Ubuntu");
    CHECK((*rows)[0].running == true);
    CHECK((*rows)[1].running == false);
}

TEST_CASE("the list table marks the default distribution", "[cli][list]") {
    Machine machine = ordinary();

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    Golden{"list-command-table.txt"}.check(table_of(*rows));
}

TEST_CASE("the list table shows a stopped distribution with unknown guest usage", "[cli][list]") {
    // Nothing is started to measure it, so those columns are dashes rather than
    // zeroes -- and the difference matters to anyone reading the output.
    Machine machine = ordinary();
    machine.host.set_running({});

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    CHECK_FALSE((*rows)[0].info.guest_used.has_value());
    CHECK(machine.host.commands().empty());
}

TEST_CASE("probing reads guest usage for a stopped distribution", "[cli][list]") {
    Machine machine = ordinary();
    machine.host.set_running({});

    const auto rows = machine.run(ListOptions{.probe = true});

    REQUIRE(rows.has_value());
    CHECK((*rows)[0].info.guest_used == 8 * gigabyte);
}

TEST_CASE("a WSL1 distribution is listed rather than refused", "[cli][list]") {
    // D8: this is the one command that shows them, because being shown is how
    // the user learns which distribution is the odd one.
    Machine machine;
    machine.registry = hives::everything();
    machine.host.set_running({});

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    const auto wsl1 =
        std::ranges::find_if(*rows, [](const ListRow& row) { return row.distro.name == "Legacy-WSL1"; });
    REQUIRE(wsl1 != rows->end());
    CHECK(wsl1->distro.version == 1);
    // No disk was measured, because a WSL1 distribution has none.
    CHECK_FALSE(wsl1->info.file_size.has_value());
    CHECK(wsl1->info.notes.empty());
}

TEST_CASE("a distribution whose disk is missing is still listed", "[cli][list]") {
    // Exactly what `orphans --relink` repairs; hiding the row would hide the
    // problem.
    Machine machine;
    machine.registry = hives::everything();
    machine.host.set_running({});

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    const auto moved =
        std::ranges::find_if(*rows, [](const ListRow& row) { return row.distro.name == "Moved-Away"; });
    REQUIRE(moved != rows->end());
    CHECK_FALSE(moved->info.file_size.has_value());
    CHECK_FALSE(moved->info.notes.empty());
}

TEST_CASE("a skipped registry key is reported as a warning", "[cli][list]") {
    // The key with no DistributionName. `list` still works; the user hears
    // about it.
    Machine machine;
    machine.registry = hives::everything();
    machine.host.set_running({});

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    CHECK(machine.errors.str().find("DistributionName") != std::string::npos);
}

TEST_CASE("list fails only when the registry cannot be read", "[cli][list]") {
    Machine machine = ordinary();
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::Preflight, "the hive is gone", "check WSL is installed"});

    const auto rows = machine.run();

    REQUIRE_FALSE(rows.has_value());
    CHECK(rows.error().code == ErrorCode::Preflight);
}

TEST_CASE("a host that cannot answer leaves the state unknown", "[cli][list]") {
    // "Stopped" and "we could not ask" lead to different next steps, so they
    // are different answers.
    Machine machine = ordinary();
    machine.host.fail_running(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    CHECK_FALSE((*rows)[0].running.has_value());
    CHECK(machine.errors.str().find("did not answer") != std::string::npos);
}

TEST_CASE("the state column shows unknown as a dash", "[cli][list]") {
    Machine machine = ordinary();
    machine.host.fail_running(wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "."});

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    CHECK(table_of(*rows).find("  -  ") != std::string::npos);
}

TEST_CASE("an empty machine prints only the headers", "[cli][list]") {
    Machine machine;
    machine.registry = FakeRegistry{};
    machine.registry.add_key(std::wstring{hives::lxss});
    machine.host.set_running({});

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    CHECK(rows->empty());
    Golden{"list-command-empty.txt"}.check(table_of(*rows));
}

TEST_CASE("the json listing is one object per line", "[cli][list]") {
    // A caller can process the first row without waiting for the last, and
    // `wsldisk list --json | head -1` does something sensible.
    Machine machine = ordinary();
    const auto rows = machine.run();
    REQUIRE(rows.has_value());

    std::ostringstream out;
    render_json(*rows, out);

    std::istringstream lines{out.str()};
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        INFO("line: " << line);
        CHECK_NOTHROW(nlohmann::json::parse(line));
        ++count;
    }
    CHECK(count == 2);
}

TEST_CASE("the json listing carries the measured sizes", "[cli][list]") {
    Machine machine = ordinary();
    const auto rows = machine.run();
    REQUIRE(rows.has_value());

    std::ostringstream out;
    render_json(*rows, out);
    std::istringstream lines{out.str()};
    std::string first;
    std::getline(lines, first);

    const auto object = nlohmann::json::parse(first);
    CHECK(object["name"] == "Ubuntu");
    CHECK(object["default"] == true);
    CHECK(object["size_on_disk"].get<std::uint64_t>() == 14 * gigabyte);
    CHECK(object["guest_used"].get<std::uint64_t>() == 8 * gigabyte);
}

TEST_CASE("an extended-length path is shown in its display form", "[cli][list]") {
    // Docker Desktop stores its BasePath with the \\?\ prefix; the stored form
    // is kept for `relink`, but nobody wants to read it.
    Machine machine = ordinary();

    const auto rows = machine.run();

    REQUIRE(rows.has_value());
    const auto docker =
        std::ranges::find_if(*rows, [](const ListRow& row) { return row.distro.name == "docker-desktop"; });
    REQUIRE(docker != rows->end());
    CHECK_FALSE(docker->distro.vhdx_path.wstring().starts_with(LR"(\\?\)"));
    CHECK(docker->distro.base_path.starts_with(LR"(\\?\)"));
}

TEST_CASE("run_list prints the table and exits zero", "[cli][list]") {
    Machine machine = ordinary();
    NullLogger logger{machine.errors};
    std::ostringstream out;
    std::ostringstream err;

    const int code = wsldisk::cli::run_list(machine.services(), {}, {}, logger, out, err);

    CHECK(code == 0);
    CHECK(out.str().find("NAME") != std::string::npos);
    CHECK(err.str().empty());
}

TEST_CASE("run_list prints json when asked", "[cli][list]") {
    Machine machine = ordinary();
    NullLogger logger{machine.errors};
    std::ostringstream out;
    std::ostringstream err;

    const int code = wsldisk::cli::run_list(machine.services(), {}, wsldisk::cli::GlobalOptions{.json = true},
                                            logger, out, err);

    CHECK(code == 0);
    CHECK(out.str().starts_with("{"));
    CHECK(out.str().find("NAME") == std::string::npos);
}

TEST_CASE("run_list reports a registry it cannot read", "[cli][list]") {
    // Which outcome happens here would otherwise depend on whether the machine
    // running the tests has WSL installed.
    Machine machine = ordinary();
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::Preflight, "the hive is gone", "check WSL is installed"});
    NullLogger logger{machine.errors};
    std::ostringstream out;
    std::ostringstream err;

    const int code = wsldisk::cli::run_list(machine.services(), {}, {}, logger, out, err);

    CHECK(code == 3);
    CHECK(err.str().find("the hive is gone") != std::string::npos);
    CHECK(out.str().empty());
}
