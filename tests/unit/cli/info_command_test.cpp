#include "info_command.h"

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
using wsldisk::cli::gather_one;
using wsldisk::cli::GlobalOptions;
using wsldisk::cli::InfoOptions;
using wsldisk::cli::NullLogger;
using wsldisk::cli::render_details;
using wsldisk::cli::render_details_json;
using wsldisk::cli::run_info;
using wsldisk::cli::Services;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
using wsldisk::testing::Golden;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

struct Machine {
    FakeRegistry registry = hives::everything();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    std::ostringstream errors;

    Machine() {
        const std::filesystem::path ubuntu =
            LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)";

        FakeFileSystem::File file;
        file.size = 14 * gigabyte;
        file.size_on_disk = 9 * gigabyte;
        file.sparse = true;
        file.ranges.push_back(AllocatedRange{.offset = 0, .length = 9 * gigabyte});
        filesystem.add_file(ubuntu, file);

        FakeVirtualDisk::Disk disk;
        disk.info.virtual_size = 1024 * gigabyte;
        disk.info.physical_size = 9 * gigabyte;
        disk.info.block_size = 2 * 1024 * 1024;
        disk.info.sector_size = 512;
        disks.add_disk(ubuntu, disk);

        host.set_running({"Ubuntu"});

        wsldisk::WslCommandResult df;
        df.exit_code = 0;
        df.standard_output =
            "Filesystem 1B-blocks Used Available Use% Mounted on\n/dev/sdc 1099511627776 8589934592 "
            "1090921693184 1% /\n";
        host.on_command("/bin/df", df);
    }

    [[nodiscard]] Services services() {
        return Services{.registry = &registry, .filesystem = &filesystem, .disks = &disks, .host = &host};
    }
};

std::string details_of(Machine& machine, const std::string& name) {
    std::ostringstream errors;
    NullLogger logger{errors};
    const auto row = gather_one(machine.services(), InfoOptions{.name = name}, logger);
    REQUIRE(row.has_value());

    std::ostringstream out;
    render_details(*row, out);
    return out.str();
}

}  // namespace

TEST_CASE("info describes a modern distribution", "[cli][info]") {
    Machine machine;

    Golden{"info-modern.txt"}.check(details_of(machine, "Ubuntu"));
}

TEST_CASE("info shows an absent VhdFileName as absent", "[cli][info]") {
    // The legacy MSIX layout has no such value. "Absent" and "set to
    // ext4.vhdx" are different registry states, and `info` is where the
    // difference is visible.
    Machine machine;

    const std::string details = details_of(machine, "Ubuntu-20.04");

    CHECK(details.find("vhd file name: - (absent; defaults to ext4.vhdx)") != std::string::npos);
    CHECK(details.find("modern layout: no") != std::string::npos);
}

TEST_CASE("info shows the stored BasePath with its prefix", "[cli][info]") {
    // The one place a user can see that their BasePath is the extended-length
    // kind, which is why `relink` has to write back the same form.
    Machine machine;

    const std::string details = details_of(machine, "docker-desktop");

    CHECK(details.find(R"(base path:     \\?\C:\Users\example)") != std::string::npos);
    // And the resolved path, which is the one anything else can open.
    CHECK(details.find(R"(disk path:     C:\Users\example)") != std::string::npos);
}

TEST_CASE("info decodes the flags and keeps the raw value", "[cli][info]") {
    // 15 on every distribution spike #4 measured: the three documented flags
    // plus a bit no published header names.
    Machine machine;

    const std::string details = details_of(machine, "Ubuntu");

    CHECK(details.find("flags:         15 (interop, append-nt-path, drive-mounting, undocumented(0x8))") !=
          std::string::npos);
}

TEST_CASE("info reports a WSL1 distribution without inventing disk numbers", "[cli][info]") {
    Machine machine;

    const std::string details = details_of(machine, "Legacy-WSL1");

    CHECK(details.find("wsl version:   1") != std::string::npos);
    CHECK(details.find("virtual size:  -") != std::string::npos);
    CHECK(details.find("size on disk:  -") != std::string::npos);
}

TEST_CASE("info matches a name case-insensitively", "[cli][info]") {
    // `wsl.exe` matches names that way; a tool beside it that did not would be
    // its own kind of surprise.
    Machine machine;
    std::ostringstream errors;
    NullLogger logger{errors};

    CHECK(gather_one(machine.services(), InfoOptions{.name = "ubuntu"}, logger).has_value());
    CHECK(gather_one(machine.services(), InfoOptions{.name = "UBUNTU"}, logger).has_value());
}

TEST_CASE("an unknown name exits 10 and suggests the closest", "[cli][info]") {
    Machine machine;
    std::ostringstream errors;
    NullLogger logger{errors};

    const auto row = gather_one(machine.services(), InfoOptions{.name = "Ubunt"}, logger);

    REQUIRE_FALSE(row.has_value());
    CHECK(row.error().code == ErrorCode::DistroNotFound);
    CHECK(wsldisk::exit_code_for(row.error().code) == 10);
    CHECK(row.error().remedy.find("Ubuntu") != std::string::npos);
}

TEST_CASE("a name nothing resembles falls back to listing", "[cli][info]") {
    // A suggestion that is not a plausible correction stops looking like help
    // and starts looking like the tool guessing.
    Machine machine;
    std::ostringstream errors;
    NullLogger logger{errors};

    const auto row = gather_one(machine.services(), InfoOptions{.name = "completely-different"}, logger);

    REQUIRE_FALSE(row.has_value());
    CHECK(row.error().remedy == "run `wsldisk list` to see what is registered");
}

TEST_CASE("the info json object is a superset of the list line", "[cli][info]") {
    // `info --json` and `list --json` must describe the same distribution the
    // same way, or a script cannot use them interchangeably.
    Machine machine;
    std::ostringstream errors;
    NullLogger logger{errors};
    const auto row = gather_one(machine.services(), InfoOptions{.name = "Ubuntu"}, logger);
    REQUIRE(row.has_value());

    std::ostringstream out;
    render_details_json(*row, out);
    const auto object = nlohmann::json::parse(out.str());

    CHECK(object["name"] == "Ubuntu");
    CHECK(object["size_on_disk"].get<std::uint64_t>() == 9 * gigabyte);
    // The fields only `info` reports.
    CHECK(object["registry_key"].get<std::string>().find("CurrentVersion") != std::string::npos);
    CHECK(object["flags"] == 15);
    CHECK(object["flags_decoded"].get<std::string>().find("interop") != std::string::npos);
    CHECK(object["running"] == true);
    CHECK(object["block_size"] == 2 * 1024 * 1024);
    CHECK(object["sector_size"] == 512);
}

TEST_CASE("the info json omits a parent path when there is none", "[cli][info]") {
    // No WSL distribution is a differencing disk unless someone built a chain
    // by hand.
    Machine machine;
    std::ostringstream errors;
    NullLogger logger{errors};
    const auto row = gather_one(machine.services(), InfoOptions{.name = "Ubuntu"}, logger);
    REQUIRE(row.has_value());

    std::ostringstream out;
    render_details_json(*row, out);

    CHECK_FALSE(nlohmann::json::parse(out.str()).contains("parent_path"));
}

TEST_CASE("run_info prints the details and exits zero", "[cli][info]") {
    Machine machine;
    NullLogger logger{machine.errors};
    std::ostringstream out;
    std::ostringstream err;

    const int code =
        run_info(machine.services(), InfoOptions{.name = "Ubuntu"}, GlobalOptions{}, logger, out, err);

    CHECK(code == 0);
    CHECK(out.str().find("name:") != std::string::npos);
}

TEST_CASE("run_info prints json when asked", "[cli][info]") {
    Machine machine;
    NullLogger logger{machine.errors};
    std::ostringstream out;
    std::ostringstream err;

    const int code = run_info(machine.services(), InfoOptions{.name = "Ubuntu"}, GlobalOptions{.json = true},
                              logger, out, err);

    CHECK(code == 0);
    CHECK_NOTHROW(nlohmann::json::parse(out.str()));
}

TEST_CASE("run_info reports an unknown distribution", "[cli][info]") {
    Machine machine;
    NullLogger logger{machine.errors};
    std::ostringstream out;
    std::ostringstream err;

    const int code =
        run_info(machine.services(), InfoOptions{.name = "nope"}, GlobalOptions{}, logger, out, err);

    CHECK(code == 10);
    CHECK(err.str().find("no distribution named nope") != std::string::npos);
}

TEST_CASE("info reports a registry it cannot read", "[cli][info]") {
    Machine machine;
    machine.registry.fail_with(
        wsldisk::Error{ErrorCode::Preflight, "the hive is gone", "check WSL is installed"});
    NullLogger logger{machine.errors};
    std::ostringstream out;
    std::ostringstream err;

    const int code =
        run_info(machine.services(), InfoOptions{.name = "Ubuntu"}, GlobalOptions{}, logger, out, err);

    CHECK(code == 3);
}

TEST_CASE("info prints the notes explaining a blank column", "[cli][info]") {
    // The point of `info` is to say what is and is not known, so a blank field
    // comes with its reason rather than leaving the user guessing.
    Machine machine;
    machine.host.set_running({});
    NullLogger logger{machine.errors};
    std::ostringstream out;
    std::ostringstream err;

    std::ignore =
        run_info(machine.services(), InfoOptions{.name = "Ubuntu"}, GlobalOptions{}, logger, out, err);

    CHECK(out.str().find("note:") != std::string::npos);
    CHECK(out.str().find("--probe") != std::string::npos);
}

namespace {

/// A row whose only interesting property is its flags, for the decoder.
std::string flags_line(std::uint32_t flags) {
    wsldisk::cli::ListRow row;
    row.distro.name = "Ubuntu";
    row.distro.flags = flags;

    std::ostringstream out;
    render_details(row, out);

    std::istringstream lines{out.str()};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.starts_with("flags:")) {
            return line;
        }
    }
    return {};
}

}  // namespace

TEST_CASE("no flags reads as none rather than an empty list", "[cli][info]") {
    CHECK(flags_line(0).find("0 (none)") != std::string::npos);
}

TEST_CASE("each documented flag is named on its own", "[cli][info]") {
    CHECK(flags_line(0x1).find("(interop)") != std::string::npos);
    CHECK(flags_line(0x2).find("(append-nt-path)") != std::string::npos);
    CHECK(flags_line(0x4).find("(drive-mounting)") != std::string::npos);
}

TEST_CASE("an undocumented bit is reported by number", "[cli][info]") {
    // Rather than given a name this project invented. wslapi.h documents three
    // flags; bit 3 is set on every distribution spike #4 measured and is in no
    // published header.
    CHECK(flags_line(0x8).find("(undocumented(0x8))") != std::string::npos);
    CHECK(flags_line(0x10).find("(undocumented(0x10))") != std::string::npos);
}

TEST_CASE("two plausible corrections are both offered", "[cli][info]") {
    // `Ubuntu-20` is three edits from both `Ubuntu` and `Ubuntu-20.04`, so the
    // remedy lists them rather than picking one and hoping.
    Machine machine;
    std::ostringstream errors;
    NullLogger logger{errors};

    const auto row = gather_one(machine.services(), InfoOptions{.name = "Ubuntu-20"}, logger);

    REQUIRE_FALSE(row.has_value());
    CHECK(row.error().remedy.find("Ubuntu-20.04") != std::string::npos);
    CHECK(row.error().remedy.find(", ") != std::string::npos);
}

TEST_CASE("a disk that is not sparse says so", "[cli][info]") {
    // Three states, not two: sparse, not sparse, and not measurable. A blank
    // and a "no" mean different things.
    wsldisk::cli::ListRow row;
    row.distro.name = "Ubuntu";
    row.info.is_sparse = false;

    std::ostringstream out;
    render_details(row, out);

    CHECK(out.str().find("sparse:        no") != std::string::npos);
}

TEST_CASE("a differencing disk names its parent", "[cli][info]") {
    // No WSL distribution is one unless somebody built a chain by hand, but if
    // they did, `info` is where they would look for the parent.
    wsldisk::cli::ListRow row;
    row.distro.name = "Ubuntu";
    row.info.parent_path = LR"(C:\wsl\base.vhdx)";

    std::ostringstream out;
    render_details(row, out);
    CHECK(out.str().find(R"(parent disk:   C:\wsl\base.vhdx)") != std::string::npos);

    std::ostringstream json;
    render_details_json(row, json);
    CHECK(nlohmann::json::parse(json.str())["parent_path"] == R"(C:\wsl\base.vhdx)");
}
