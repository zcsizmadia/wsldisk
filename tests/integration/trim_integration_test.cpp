// Integration cases for `trim`, against real WSL2 and a real busybox guest.
//
// The fixture is Alpine, so `fstrim` here is busybox's. That is the point: the
// option handling in TrimOperation exists because busybox rejects options
// util-linux accepts (spike #1), and a fake cannot prove which spellings a real
// guest takes.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "integration_fixture.h"
#include "model/distro.h"
#include "ops/runner.h"
#include "ops/trim.h"
#include "platform/registry.h"
#include "platform/wsl_host.h"

using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::ops::TrimOperation;
using wsldisk::platform::Win32Registry;
using wsldisk::platform::WslExeHost;
using wsldisk::testing::integration_enabled;
using wsldisk::testing::pinned_rootfs;
using wsldisk::testing::TempDistro;

namespace {

/// A sink that ignores everything: the assertions are about what the guest did.
class QuietSink final : public wsldisk::ops::ProgressSink {
public:
    void step_started(std::size_t, const wsldisk::ops::StepPlan&) override {}

    void step_finished(std::size_t, const wsldisk::ops::StepPlan&) override {}

    void step_progress(const wsldisk::DiskProgress&) override {}

    void message(std::string_view) override {}
};

[[nodiscard]] bool ready() {
    if (!integration_enabled()) {
        SKIP("set WSLDISK_INTEGRATION=1 to run integration tests");
    }
    if (pinned_rootfs().empty()) {
        SKIP("run scripts/fetch-fixtures.ps1 to download the pinned rootfs");
    }
    return true;
}

}  // namespace

TEST_CASE("trim runs in a real guest and leaves it running", "[integration]") {
    if (!ready()) {
        return;
    }

    const TempDistro distro{"trim"};
    REQUIRE(distro.valid());

    const Win32Registry registry;
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());
    const Distro* registered = distros->find(distro.name());
    REQUIRE(registered != nullptr);

    const WslExeHost host;
    TrimOperation operation{host, *registered};
    QuietSink sink;

    const auto outcome = run(operation, sink, RunOptions{});
    if (!outcome.has_value()) {
        FAIL("trim failed: " << outcome.error().to_string());
    }

    // The distribution is still there and still runs. `trim` is the one reclaim
    // step that must not shut anything down -- that is what makes it safe to
    // put on a schedule.
    const auto guest = distro.run("/bin/true");
    CHECK(guest.exit_code == 0);
}

TEST_CASE("trim changes nothing on a dry run against a real guest", "[integration]") {
    if (!ready()) {
        return;
    }

    const TempDistro distro{"trimdry"};
    REQUIRE(distro.valid());

    const Win32Registry registry;
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());
    const Distro* registered = distros->find(distro.name());
    REQUIRE(registered != nullptr);

    const WslExeHost host;
    TrimOperation operation{host, *registered};
    QuietSink sink;

    const auto outcome = run(operation, sink, RunOptions{.dry_run = true});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->plan.steps.size() == 1);
    CHECK_FALSE(outcome->report.has_value());
    CHECK_FALSE(operation.trimmed_bytes().has_value());
}
