#include "ops/usage.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "errors.h"
#include "fake_registry.h"
#include "fake_wsl_host.h"
#include "lxss_hives.h"
#include "model/distro.h"

using wsldisk::ErrorCode;
using wsldisk::WslCommandResult;
using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::home_directories;
using wsldisk::ops::parse_du_line;
using wsldisk::ops::UsageEntry;
using wsldisk::ops::UsageOperation;
using wsldisk::ops::UsageOptions;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeWslHost;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;
constexpr std::uint64_t megabyte = 1024ULL * 1024;

[[nodiscard]] WslCommandResult du_says(std::uint64_t bytes, std::string_view path) {
    WslCommandResult result;
    result.exit_code = 0;
    result.standard_output = std::to_string(bytes) + "\t" + std::string{path} + "\n";
    return result;
}

[[nodiscard]] WslCommandResult df_says(std::uint64_t used, std::uint64_t available) {
    WslCommandResult result;
    result.exit_code = 0;
    result.standard_output = "Filesystem 1B-blocks Used Available Use% Mounted on\n/dev/sdc 1099511627776 " +
                             std::to_string(used) + " " + std::to_string(available) + " 1% /\n";
    return result;
}

[[nodiscard]] WslCommandResult passwd_says(std::string_view text) {
    WslCommandResult result;
    result.exit_code = 0;
    result.standard_output = std::string{text};
    return result;
}

/// A guest where `df` answers, one real user exists, and `du` reports nothing
/// unless a test says otherwise.
struct Machine {
    FakeRegistry registry = hives::everything();
    FakeWslHost host;

    Machine() {
        host.on_command("/bin/df", df_says(20 * gigabyte, 80 * gigabyte));
        host.on_command("/usr/bin/getent", passwd_says("root:x:0:0:root:/root:/bin/bash\n"
                                                       "example:x:1000:1000::/home/example:/bin/bash\n"
                                                       "daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n"));
        // Nothing found, which is what a path that is not there looks like.
        host.on_command("/usr/bin/du", WslCommandResult{.exit_code = 1});
    }

    [[nodiscard]] Distro distro(std::string_view name) {
        const auto distros = enumerate(registry);
        REQUIRE(distros.has_value());
        const Distro* found = distros->find(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    [[nodiscard]] UsageOperation usage(std::string_view name = "Ubuntu", UsageOptions options = {}) {
        return UsageOperation{host, distro(name), options};
    }
};

/// Ignores the progress callback, for tests that are not about it.
void ignore(std::string_view) {}

[[nodiscard]] const UsageEntry* find_path(const std::vector<UsageEntry>& entries, std::string_view path) {
    const auto found =
        std::ranges::find_if(entries, [path](const UsageEntry& entry) { return entry.path == path; });
    return found == entries.end() ? nullptr : &*found;
}

}  // namespace

TEST_CASE("du output is a byte count and a path", "[ops][usage]") {
    CHECK(parse_du_line("4096\t/tmp") == 4096ULL);
    CHECK(parse_du_line("0\t/var/log") == 0ULL);
}

TEST_CASE("anything that is not a du line is not a measurement", "[ops][usage]") {
    // A permission warning or a shell complaining the binary is missing must not
    // read as a size.
    CHECK_FALSE(parse_du_line("du: cannot read directory '/proc'").has_value());
    CHECK_FALSE(parse_du_line("").has_value());
    CHECK_FALSE(parse_du_line("\t/tmp").has_value());
    CHECK_FALSE(parse_du_line("4096").has_value());
    CHECK_FALSE(parse_du_line("4096 /tmp").has_value());
    CHECK_FALSE(parse_du_line("4o96\t/tmp").has_value());
    CHECK_FALSE(parse_du_line("-1\t/tmp").has_value());
}

TEST_CASE("home directories come from passwd, real users only", "[ops][usage]") {
    // Every system account has a home, and `du` on `/usr/sbin` or `/` once per
    // account would take minutes to report the same number a dozen times.
    const std::vector<std::string> homes = home_directories(
        "root:x:0:0:root:/root:/bin/bash\n"
        "example:x:1000:1000::/home/example:/bin/bash\n"
        "daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n"
        "nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n");

    REQUIRE(homes.size() == 2);
    CHECK(homes[0] == "/home/example");
    CHECK(homes[1] == "/root");
}

TEST_CASE("a duplicated home is listed once", "[ops][usage]") {
    // Two accounts can share one; measuring it twice would double-count it.
    const std::vector<std::string> homes = home_directories(
        "a:x:1:1::/home/shared:/bin/sh\n"
        "b:x:2:2::/home/shared:/bin/sh\n");

    CHECK(homes.size() == 1);
}

TEST_CASE("a passwd line with too few fields is skipped", "[ops][usage]") {
    CHECK(home_directories("broken\nroot:x:0:0:root:/root:/bin/bash\n").size() == 1);
}

TEST_CASE("a passwd entry with no home is skipped", "[ops][usage]") {
    // `getent` prints the field empty rather than omitting it, and an empty
    // home would expand `~/.cache` to `/.cache`.
    CHECK(home_directories("odd:x:1:1:::/bin/sh\nroot:x:0:0:root:/root:/bin/sh\n").size() == 1);
}

TEST_CASE("passwd output with carriage returns still parses", "[ops][usage]") {
    // The guest's output is UTF-8 and usually LF, but nothing guarantees it.
    CHECK(home_directories("root:x:0:0:root:/root:/bin/bash\r\n").size() == 1);
}

TEST_CASE("usage reports what du found, biggest first", "[ops][usage]") {
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/var/cache/apt/archives",
                                du_says(200 * megabyte, "/var/cache/apt/archives"));
    machine.host.on_command_for("/usr/bin/du", "/var/lib/docker", du_says(3 * gigabyte, "/var/lib/docker"));

    UsageOperation operation = machine.usage();
    const auto report = operation.measure(ignore);

    REQUIRE(report.has_value());
    REQUIRE(report->entries.size() == 2);
    CHECK(report->entries[0].path == "/var/lib/docker");
    CHECK(report->entries[0].bytes == 3 * gigabyte);
    CHECK(report->entries[1].path == "/var/cache/apt/archives");
    CHECK(report->guest_used == 20 * gigabyte);
    CHECK(report->guest_free == 80 * gigabyte);
}

TEST_CASE("usage carries the catalogue's judgement about each entry", "[ops][usage]") {
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/var/lib/docker", du_says(3 * gigabyte, "/var/lib/docker"));
    machine.host.on_command_for("/usr/bin/du", "/var/cache/apt/archives",
                                du_says(megabyte, "/var/cache/apt/archives"));

    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    const UsageEntry* docker = find_path(report->entries, "/var/lib/docker");
    REQUIRE(docker != nullptr);
    // Images the user built. wsldisk does not get to decide they are disposable.
    CHECK_FALSE(docker->safe);
    CHECK_FALSE(docker->note.empty());

    const UsageEntry* apt = find_path(report->entries, "/var/cache/apt/archives");
    REQUIRE(apt != nullptr);
    CHECK(apt->safe);
}

TEST_CASE("a path that measures zero is not reported", "[ops][usage]") {
    // An empty directory is not where the space went, and a row of `0 B` is a
    // line the user has to read to learn nothing.
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/tmp", du_says(0, "/tmp"));

    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    CHECK(report->entries.empty());
}

TEST_CASE("a top larger than the findings changes nothing", "[ops][usage]") {
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/tmp", du_says(megabyte, "/tmp"));

    const auto report = machine.usage("Ubuntu", UsageOptions{.top = 10}).measure(ignore);

    REQUIRE(report.has_value());
    CHECK(report->entries.size() == 1);
}

TEST_CASE("a getent that cannot be run still checks root", "[ops][usage]") {
    // Different from `getent` returning non-zero: this is wsl.exe not answering
    // at all, and the two take different paths.
    Machine machine;
    machine.host.fail_command_from(
        2, wsldisk::Error{ErrorCode::Generic, "wsl.exe stopped answering", "try again"});

    const auto report = machine.usage().measure(ignore);

    // The failure is swallowed for `getent` -- knowing the users is a
    // convenience -- and then the first `du` hits the same failure and is not.
    REQUIRE_FALSE(report.has_value());
}

TEST_CASE("a path that is not there is silently nothing", "[ops][usage]") {
    // No guest has every package manager, and a note per absent path would be
    // thirty lines of noise.
    Machine machine;
    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    CHECK(report->entries.empty());
    CHECK(report->notes.empty());
}

TEST_CASE("an entry inside another is counted once", "[ops][usage]") {
    // `/var/log/journal` is inside `/var/log`. Both are worth reporting; only
    // one of them can be added to a total.
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/var/log", du_says(900 * megabyte, "/var/log"));
    machine.host.on_command_for("/usr/bin/du", "/var/log/journal",
                                du_says(800 * megabyte, "/var/log/journal"));

    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    REQUIRE(report->entries.size() == 2);
    CHECK(report->counted == 900 * megabyte);

    const UsageEntry* logs = find_path(report->entries, "/var/log");
    REQUIRE(logs != nullptr);
    CHECK(logs->contains_others);
    const UsageEntry* journal = find_path(report->entries, "/var/log/journal");
    REQUIRE(journal != nullptr);
    CHECK_FALSE(journal->contains_others);
}

TEST_CASE("entries that do not overlap all count", "[ops][usage]") {
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/tmp", du_says(megabyte, "/tmp"));
    machine.host.on_command_for("/usr/bin/du", "/var/tmp", du_says(2 * megabyte, "/var/tmp"));

    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    CHECK(report->counted == 3 * megabyte);
}

TEST_CASE("a per-user path is measured once per real home", "[ops][usage]") {
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/home/example/.cache",
                                du_says(500 * megabyte, "/home/example/.cache"));
    machine.host.on_command_for("/usr/bin/du", "/root/.cache", du_says(megabyte, "/root/.cache"));

    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    REQUIRE(report->entries.size() == 2);
    // Same label for both, so it says whose is whose.
    CHECK(report->entries[0].label.find("/home/example/.cache") != std::string::npos);
    CHECK(report->entries[1].label.find("/root/.cache") != std::string::npos);
}

TEST_CASE("a guest whose users cannot be listed still checks root", "[ops][usage]") {
    // Better than nothing, and true on the great majority of guests -- but the
    // report says so rather than implying it looked everywhere.
    Machine machine;
    machine.host.on_command("/usr/bin/getent", WslCommandResult{.exit_code = 2});
    machine.host.on_command_for("/usr/bin/du", "/root/.cache", du_says(megabyte, "/root/.cache"));

    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    CHECK(find_path(report->entries, "/root/.cache") != nullptr);
    REQUIRE(report->notes.size() == 1);
    CHECK(report->notes[0].find("/root") != std::string::npos);
}

TEST_CASE("a guest with no real users falls back to root", "[ops][usage]") {
    // `getent` answered; it just had nothing but system accounts to say. An
    // appliance distribution with no login user looks exactly like this, and it
    // still has a /root worth measuring.
    Machine machine;
    machine.host.on_command("/usr/bin/getent",
                            passwd_says("daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n"
                                        "nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n"));
    machine.host.on_command_for("/usr/bin/du", "/root/.cache", du_says(megabyte, "/root/.cache"));

    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    CHECK(find_path(report->entries, "/root/.cache") != nullptr);
    REQUIRE(report->notes.size() == 1);
}

TEST_CASE("a guest whose df cannot be read still reports what du found", "[ops][usage]") {
    Machine machine;
    machine.host.on_command("/bin/df", WslCommandResult{.exit_code = 0, .standard_output = "nonsense\n"});
    machine.host.on_command_for("/usr/bin/du", "/tmp", du_says(megabyte, "/tmp"));

    const auto report = machine.usage().measure(ignore);

    REQUIRE(report.has_value());
    CHECK(report->guest_used == 0);
    CHECK(report->entries.size() == 1);
    REQUIRE(report->notes.size() == 1);
    CHECK(report->notes[0].find("df") != std::string::npos);
}

TEST_CASE("usage says which path it is measuring", "[ops][usage]") {
    // `du` over a large filesystem takes minutes, and silence reads as a hang.
    Machine machine;
    std::vector<std::string> announced;
    const auto record = [&announced](std::string_view path) { announced.emplace_back(path); };

    REQUIRE(machine.usage().measure(record).has_value());

    CHECK(announced.size() > 1);
    CHECK(std::ranges::find(announced, "/var/lib/docker") != announced.end());
}

TEST_CASE("top keeps the largest entries", "[ops][usage]") {
    Machine machine;
    machine.host.on_command_for("/usr/bin/du", "/var/lib/docker", du_says(3 * gigabyte, "/var/lib/docker"));
    machine.host.on_command_for("/usr/bin/du", "/tmp", du_says(megabyte, "/tmp"));
    machine.host.on_command_for("/usr/bin/du", "/var/tmp", du_says(2 * megabyte, "/var/tmp"));

    const auto report = machine.usage("Ubuntu", UsageOptions{.top = 2}).measure(ignore);

    REQUIRE(report.has_value());
    REQUIRE(report->entries.size() == 2);
    CHECK(report->entries[0].path == "/var/lib/docker");
    // The total still counts everything found, not just what is shown: a
    // `--top 2` that changed the arithmetic would be lying about the rest.
    CHECK(report->counted == 3 * gigabyte + 3 * megabyte);
}

TEST_CASE("usage refuses a WSL1 distribution", "[ops][usage]") {
    Machine machine;
    const auto report = machine.usage("Legacy-WSL1").measure(ignore);

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == ErrorCode::Preflight);
}

TEST_CASE("usage reports a guest it could not reach", "[ops][usage]") {
    Machine machine;
    machine.host.fail_command(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check the installation"});

    const auto report = machine.usage().measure(ignore);

    REQUIRE_FALSE(report.has_value());
}

TEST_CASE("usage reports a du it could not run", "[ops][usage]") {
    // The df and getent calls come first, so failing from the third exercises
    // the du path rather than the one before it.
    Machine machine;
    machine.host.fail_command_from(
        3, wsldisk::Error{ErrorCode::Generic, "wsl.exe stopped answering", "try again"});

    const auto report = machine.usage().measure(ignore);

    REQUIRE_FALSE(report.has_value());
}

TEST_CASE("usage measures with -x so mounted drives are not counted", "[ops][usage]") {
    // `/mnt/c` is the Windows disk. Counting it as part of this distribution's
    // usage would report hundreds of gigabytes that are not on the VHDX at all.
    Machine machine;
    REQUIRE(machine.usage().measure(ignore).has_value());

    bool checked = false;
    for (const auto& call : machine.host.commands()) {
        if (call.argv.front() == "/usr/bin/du") {
            CHECK(call.argv[1] == "-sbx");
            checked = true;
        }
    }
    CHECK(checked);
}
