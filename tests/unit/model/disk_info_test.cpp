#include "model/disk_info.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "errors.h"
#include "fake_filesystem.h"
#include "fake_virtual_disk.h"
#include "fake_wsl_host.h"

using wsldisk::AllocatedRange;
using wsldisk::ErrorCode;
using wsldisk::VirtualDiskInfo;
using wsldisk::WslCommandResult;
using wsldisk::model::DiskInfo;
using wsldisk::model::Distro;
using wsldisk::model::measure;
using wsldisk::model::parse_df;
using wsldisk::model::ProbeOptions;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

/// A `df -B1 /` reply with the given used and available byte counts.
WslCommandResult df_output(std::uint64_t used, std::uint64_t available) {
    WslCommandResult result;
    result.exit_code = 0;
    result.standard_output = "Filesystem 1B-blocks Used Available Use% Mounted on\n/dev/sdc 1099511627776 " +
                             std::to_string(used) + " " + std::to_string(available) + " 1% /\n";
    return result;
}

/// A reply that is not a df table at all.
WslCommandResult nonsense_output() {
    WslCommandResult result;
    result.exit_code = 0;
    result.standard_output = "not a table at all\n";
    return result;
}

/// The distribution every test here measures.
Distro ubuntu() {
    Distro distro;
    distro.name = "Ubuntu";
    distro.guid = "{4d1297e9}";
    distro.version = 2;
    distro.base_path = LR"(C:\wsl\Ubuntu)";
    distro.vhdx_path = LR"(C:\wsl\Ubuntu\ext4.vhdx)";
    return distro;
}

/// A machine where every measurement works: a 1 TiB disk occupying 14 GiB, with
/// the guest using 8 GiB of it.
struct Machine {
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;

    Machine() {
        FakeFileSystem::File file;
        file.size = 14 * gigabyte;
        file.size_on_disk = 14 * gigabyte;
        file.sparse = true;
        file.ranges.push_back(AllocatedRange{.offset = 0, .length = 14 * gigabyte});
        filesystem.add_file(ubuntu().vhdx_path, file);

        FakeVirtualDisk::Disk disk;
        disk.info.virtual_size = 1024 * gigabyte;
        disk.info.physical_size = 14 * gigabyte;
        disks.add_disk(ubuntu().vhdx_path, disk);

        host.set_running({"Ubuntu"});
        host.on_command("/bin/df", df_output(8 * gigabyte, 1090921693184ULL));
    }

    [[nodiscard]] DiskInfo run(const ProbeOptions& options = {}) const {
        return measure(ubuntu(), filesystem, disks, host, options);
    }
};

}  // namespace

TEST_CASE("measure reports every field when everything is readable", "[model][disk-info]") {
    const Machine machine;

    const DiskInfo info = machine.run();
    CHECK(info.virtual_size == 1024 * gigabyte);
    CHECK(info.file_size == 14 * gigabyte);
    CHECK(info.size_on_disk == 14 * gigabyte);
    CHECK(info.allocated_bytes == 14 * gigabyte);
    CHECK(info.is_sparse == true);
    CHECK(info.guest_used == 8 * gigabyte);
    CHECK(info.guest_free == 1090921693184ULL);
    CHECK(info.notes.empty());
}

TEST_CASE("reclaimable is the host size minus what the guest is using", "[model][disk-info]") {
    const Machine machine;

    const DiskInfo info = machine.run();
    CHECK(info.reclaimable() == 6 * gigabyte);
}

TEST_CASE("reclaimable is unknown without a guest measurement", "[model][disk-info]") {
    Machine machine;
    machine.host.set_running({});

    const DiskInfo info = machine.run();
    CHECK_FALSE(info.guest_used.has_value());
    CHECK_FALSE(info.reclaimable().has_value());
}

TEST_CASE("reclaimable is unknown without a host measurement", "[model][disk-info]") {
    DiskInfo info;
    info.guest_used = gigabyte;

    CHECK_FALSE(info.reclaimable().has_value());
}

TEST_CASE("reclaimable is zero rather than negative", "[model][disk-info]") {
    // The two numbers come from different layers, and on a compressed volume the
    // guest's can be the larger. That is no saving, not a negative one.
    DiskInfo info;
    info.size_on_disk = gigabyte;
    info.guest_used = 2 * gigabyte;

    CHECK(info.reclaimable() == 0);
}

TEST_CASE("a stopped distribution is not started to measure it", "[model][disk-info]") {
    Machine machine;
    machine.host.set_running({});

    const DiskInfo info = machine.run();
    CHECK(machine.host.commands().empty());
    REQUIRE(info.notes.size() == 1);
    CHECK(info.notes[0].find("--probe") != std::string::npos);
    // The host-side measurements are all still there.
    CHECK(info.file_size.has_value());
    CHECK(info.virtual_size.has_value());
}

TEST_CASE("probing reads a stopped distribution's guest usage", "[model][disk-info]") {
    Machine machine;
    machine.host.set_running({});

    const DiskInfo info = machine.run(ProbeOptions{.probe_guest = true});
    CHECK(info.guest_used == 8 * gigabyte);
    REQUIRE(machine.host.commands().size() == 1);
    CHECK(machine.host.commands()[0].argv[0] == "/bin/df");
}

TEST_CASE("the guest probe uses an absolute path", "[model][disk-info]") {
    // `wsl --exec` does not search PATH, and the fake refuses a relative program
    // for the same reason the real wrapper does.
    const Machine machine;

    const DiskInfo info = machine.run();
    REQUIRE(machine.host.commands().size() == 1);
    CHECK(machine.host.commands()[0].argv[0].starts_with('/'));
    CHECK(machine.host.commands()[0].distribution == "Ubuntu");
}

TEST_CASE("a locked disk leaves the virtual size unknown", "[model][disk-info]") {
    // What a running distribution looks like: the utility VM holds the file.
    Machine machine;
    machine.disks.fail_open(wsldisk::Error{ErrorCode::DistroBusy, "the disk is in use", "shut WSL down"});

    const DiskInfo info = machine.run();
    CHECK_FALSE(info.virtual_size.has_value());
    // Everything the filesystem can answer without the handle is still there.
    CHECK(info.file_size.has_value());
    CHECK(info.size_on_disk.has_value());
    CHECK(info.guest_used.has_value());
    REQUIRE(info.notes.size() == 1);
    CHECK(info.notes[0].find("in use") != std::string::npos);
}

TEST_CASE("a filesystem that cannot answer leaves those fields unknown", "[model][disk-info]") {
    Machine machine;
    machine.filesystem.fail_queries(
        wsldisk::Error{ErrorCode::Preflight, "the volume went away", "check the drive"});

    const DiskInfo info = machine.run();
    CHECK_FALSE(info.file_size.has_value());
    CHECK_FALSE(info.size_on_disk.has_value());
    CHECK_FALSE(info.is_sparse.has_value());
    CHECK_FALSE(info.allocated_bytes.has_value());
    // One note per failed measurement, so the output says what is missing.
    CHECK(info.notes.size() == 4);
}

TEST_CASE("a disk that will not describe itself leaves the size unknown", "[model][disk-info]") {
    // Openable and still unreadable: a handle is not an answer.
    Machine machine;
    machine.disks.fail_information(
        wsldisk::Error{ErrorCode::Generic, "the disk would not describe itself", "re-run"});

    const DiskInfo info = machine.run();
    CHECK_FALSE(info.virtual_size.has_value());
    REQUIRE(info.notes.size() == 1);
    CHECK(info.notes[0].find("describe itself") != std::string::npos);
}

TEST_CASE("a host that cannot list running distributions stops the probe", "[model][disk-info]") {
    Machine machine;
    machine.host.fail_running(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});

    const DiskInfo info = machine.run();
    CHECK_FALSE(info.guest_used.has_value());
    CHECK(machine.host.commands().empty());
    REQUIRE(info.notes.size() == 1);
    CHECK(info.notes[0].find("did not answer") != std::string::npos);
}

TEST_CASE("a guest command that could not be run leaves usage unknown", "[model][disk-info]") {
    // wsl.exe not answering at all, which is different from the command inside
    // returning non-zero -- and reported differently.
    Machine machine;
    machine.host.fail_command(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not start", "check WSL is installed"});

    const DiskInfo info = machine.run();
    CHECK_FALSE(info.guest_used.has_value());
    REQUIRE(info.notes.size() == 1);
    CHECK(info.notes[0].find("did not start") != std::string::npos);
}

TEST_CASE("a guest command that fails to run leaves usage unknown", "[model][disk-info]") {
    Machine machine;
    WslCommandResult failed;
    failed.exit_code = 1;
    machine.host.on_command("/bin/df", failed);

    const DiskInfo info = machine.run();
    CHECK_FALSE(info.guest_used.has_value());
    REQUIRE(info.notes.size() == 1);
    CHECK(info.notes[0].find("df exited 1") != std::string::npos);
}

TEST_CASE("unreadable df output leaves usage unknown", "[model][disk-info]") {
    Machine machine;
    machine.host.on_command("/bin/df", nonsense_output());

    const DiskInfo info = machine.run();
    CHECK_FALSE(info.guest_used.has_value());
    REQUIRE(info.notes.size() == 1);
    CHECK(info.notes[0].find("df output") != std::string::npos);
}

TEST_CASE("parse_df reads the columns from the right", "[model][disk-info]") {
    const std::string output =
        "Filesystem     1B-blocks       Used  Available Use% Mounted on\n"
        "/dev/sdc  1099511627776 8589934592 1090921693184   1% /\n";

    const auto usage = parse_df(output);

    REQUIRE(usage.has_value());
    CHECK(usage->used == 8589934592ULL);
    CHECK(usage->available == 1090921693184ULL);
}

TEST_CASE("parse_df survives a row that wrapped onto two lines", "[model][disk-info]") {
    // A long device name makes df put the name on its own line, which is why the
    // columns are counted from the right rather than the left.
    const std::string output =
        "Filesystem 1B-blocks Used Available Use% Mounted on\n"
        "/dev/mapper/a-very-long-device-name-indeed\n"
        "  1099511627776 8589934592 1090921693184 1% /\n";

    const auto usage = parse_df(output);

    REQUIRE(usage.has_value());
    CHECK(usage->used == 8589934592ULL);
}

TEST_CASE("parse_df ignores the header text", "[model][disk-info]") {
    // Headers are localized; nothing may depend on them reading as English.
    const std::string output =
        "Dateisystem 1B-Blöcke Benutzt Verfügbar Verw% Eingehängt auf\n"
        "/dev/sdc 1099511627776 8589934592 1090921693184 1% /\n";

    const auto usage = parse_df(output);

    REQUIRE(usage.has_value());
    CHECK(usage->used == 8589934592ULL);
}

TEST_CASE("parse_df rejects output with too few columns", "[model][disk-info]") {
    CHECK_FALSE(parse_df("Filesystem Used\n/dev/sdc 12\n").has_value());
}

TEST_CASE("parse_df rejects a row whose numbers are not numbers", "[model][disk-info]") {
    CHECK_FALSE(parse_df("a b c d e f\n").has_value());
}

TEST_CASE("parse_df rejects a partly numeric row", "[model][disk-info]") {
    // Used parses, available does not.
    CHECK_FALSE(parse_df("/dev/sdc 100 200 notanumber 1% /\n").has_value());
}

TEST_CASE("parse_df rejects a number with trailing rubbish", "[model][disk-info]") {
    // from_chars stops at the first byte it cannot use and reports success for
    // what it did read, so a column only counts when it is consumed entirely.
    CHECK_FALSE(parse_df("/dev/sdc 100 12abc 300 1% /\n").has_value());
}

TEST_CASE("parse_df rejects empty output", "[model][disk-info]") {
    CHECK_FALSE(parse_df("").has_value());
}

TEST_CASE("parse_df tolerates CRLF line endings", "[model][disk-info]") {
    const std::string output =
        "Filesystem 1B-blocks Used Available Use% Mounted on\r\n"
        "/dev/sdc 1099511627776 8589934592 1090921693184 1% /\r\n";

    const auto usage = parse_df(output);

    REQUIRE(usage.has_value());
    CHECK(usage->used == 8589934592ULL);
}
