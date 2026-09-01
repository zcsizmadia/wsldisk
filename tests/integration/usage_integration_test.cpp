// Integration cases for `usage`, against real WSL2 and a real guest.
//
// Read-only throughout: these run `du` and `df` inside a throwaway Alpine and
// check nothing was written. What the fakes cannot show is that the guest
// commands exist at the paths the operation uses and answer in the format it
// parses -- which is exactly the kind of thing that is right in a unit test and
// wrong against a real distribution.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "integration_fixture.h"
#include "model/distro.h"
#include "ops/usage.h"
#include "platform/registry.h"
#include "platform/wsl_host.h"

using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::UsageOperation;
using wsldisk::ops::UsageOptions;
using wsldisk::platform::Win32Registry;
using wsldisk::platform::WslExeHost;
using wsldisk::testing::integration_blocker;
using wsldisk::testing::ScratchDistro;

namespace {

[[nodiscard]] bool ready() {
    if (const auto blocker = integration_blocker(); blocker.has_value()) {
        SKIP(*blocker);
    }
    return true;
}

[[nodiscard]] std::optional<Distro> registered(const Win32Registry& registry, const std::string& name) {
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());
    const Distro* found = distros->find(name);
    if (found == nullptr) {
        return std::nullopt;
    }
    return *found;
}

void ignore(std::string_view) {}

}  // namespace

TEST_CASE("usage reads real totals out of a real guest", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"usage"};
    REQUIRE(distro.valid());
    REQUIRE(distro.boots());

    const Win32Registry registry;
    const WslExeHost host;
    const auto found = registered(registry, distro.name());
    REQUIRE(found.has_value());

    UsageOperation operation{host, *found};
    const auto report = operation.measure(ignore);
    if (!report.has_value()) {
        FAIL("usage failed: " << report.error().to_string());
    }

    // `df` answered, which is what proves the command exists where the operation
    // looks for it and prints what the parser expects.
    CHECK(report->guest_used > 0);
    CHECK(report->guest_free > 0);
    CHECK(report->distribution == distro.name());
}

TEST_CASE("usage finds the caches a real guest actually has", "[integration]") {
    // Alpine has an apk cache directory and a /tmp. It has no apt, no dnf and no
    // docker, and the absent ones must not turn into rows of zero or notes.
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"usagefind"};
    REQUIRE(distro.valid());
    REQUIRE(distro.boots());
    // Something to find, in a place the catalogue knows about. As an argv
    // rather than `sh -c`: the guest shell is busybox ash and a command
    // assembled into one string behaves according to what happens to be in it.
    const std::vector<std::string> make_dir{"/bin/mkdir", "-p", "/var/tmp"};
    REQUIRE(distro.run(make_dir).exit_code == 0);
    const std::vector<std::string> write_junk{"/bin/dd", "if=/dev/zero", "of=/var/tmp/wsldisk-usage-probe",
                                              "bs=1M", "count=8"};
    REQUIRE(distro.run(write_junk).exit_code == 0);

    const Win32Registry registry;
    const WslExeHost host;
    const auto found = registered(registry, distro.name());
    REQUIRE(found.has_value());

    const auto report = UsageOperation{host, *found}.measure(ignore);
    REQUIRE(report.has_value());

    const auto var_tmp =
        std::ranges::find_if(report->entries, [](const auto& entry) { return entry.path == "/var/tmp"; });
    REQUIRE(var_tmp != report->entries.end());
    // The eight megabytes just written, at least.
    CHECK(var_tmp->bytes >= 8 * 1024 * 1024);

    // Every reported entry has a real size. A path that is not there is left out
    // rather than reported as nothing.
    for (const auto& entry : report->entries) {
        INFO(entry.path);
        CHECK(entry.bytes > 0);
    }
}

TEST_CASE("usage is sorted biggest first and counts without double-counting", "[integration]") {
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"usagesort"};
    REQUIRE(distro.valid());
    REQUIRE(distro.boots());

    const Win32Registry registry;
    const WslExeHost host;
    const auto found = registered(registry, distro.name());
    REQUIRE(found.has_value());

    const auto report = UsageOperation{host, *found}.measure(ignore);
    REQUIRE(report.has_value());

    CHECK(std::ranges::is_sorted(
        report->entries, [](const auto& left, const auto& right) { return left.bytes > right.bytes; }));
    // The catalogue accounts for some of the disk and never more than all of it.
    CHECK(report->counted <= report->guest_used);
}

TEST_CASE("usage says which path it is measuring", "[integration]") {
    // `du` over a real filesystem is slow enough that silence reads as a hang.
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"usageprogress"};
    REQUIRE(distro.valid());
    REQUIRE(distro.boots());

    const Win32Registry registry;
    const WslExeHost host;
    const auto found = registered(registry, distro.name());
    REQUIRE(found.has_value());

    std::vector<std::string> announced;
    const auto record = [&announced](std::string_view path) { announced.emplace_back(path); };
    REQUIRE(UsageOperation{host, *found}.measure(record).has_value());

    CHECK(announced.size() > 5);
}

TEST_CASE("usage leaves the guest alone", "[integration]") {
    // The point of the command being read-only. If it ever grew a step that
    // wrote, this is what would notice.
    if (!ready()) {
        return;
    }

    ScratchDistro distro{"usagereadonly"};
    REQUIRE(distro.valid());
    REQUIRE(distro.boots());
    const auto before = distro.file_hash("/etc/alpine-release");
    REQUIRE(before.has_value());

    const Win32Registry registry;
    const WslExeHost host;
    const auto found = registered(registry, distro.name());
    REQUIRE(found.has_value());
    REQUIRE(UsageOperation{host, *found}.measure(ignore).has_value());

    CHECK(distro.file_hash("/etc/alpine-release") == before);
    // And the registry still points where it did.
    const auto after = registered(registry, distro.name());
    REQUIRE(after.has_value());
    CHECK(after->vhdx_path == found->vhdx_path);
}
