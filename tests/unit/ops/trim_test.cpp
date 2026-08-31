#include "ops/trim.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

#include "fake_registry.h"
#include "fake_wsl_host.h"
#include "lxss_hives.h"
#include "model/distro.h"
#include "ops/runner.h"
#include "recording_sink.h"

using wsldisk::ErrorCode;
using wsldisk::WslCommandResult;
using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::ops::parse_trimmed_bytes;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::ops::TrimOperation;
using wsldisk::testing::FakeWslHost;
using wsldisk::testing::RecordingSink;
namespace hives = wsldisk::testing::hives;

namespace {

/// One distribution out of the canned hive, by name.
[[nodiscard]] Distro distro_named(std::string_view name) {
    auto registry = hives::everything();
    const auto distros = enumerate(registry);
    REQUIRE(distros.has_value());
    const Distro* found = distros->find(name);
    REQUIRE(found != nullptr);
    return *found;
}

/// What busybox says when it will not take an option.
[[nodiscard]] WslCommandResult refuses_the_option() {
    return WslCommandResult{.exit_code = 1, .standard_error = "fstrim: unrecognized option: v\n"};
}

}  // namespace

TEST_CASE("the trimmed byte count is read from util-linux output", "[ops][trim]") {
    CHECK(parse_trimmed_bytes("/: 1005 GiB (1078939029504 bytes) trimmed on /dev/sdc\n") == 1078939029504ULL);
}

TEST_CASE("the trimmed byte count is read from busybox output", "[ops][trim]") {
    CHECK(parse_trimmed_bytes("/: 1078939029504 bytes trimmed\n") == 1078939029504ULL);
}

TEST_CASE("the trimmed byte count is read from the older util-linux wording", "[ops][trim]") {
    CHECK(parse_trimmed_bytes("/: 1073741824 bytes were trimmed\n") == 1073741824ULL);
}

TEST_CASE("output with no byte count reports none", "[ops][trim]") {
    CHECK_FALSE(parse_trimmed_bytes("").has_value());
    CHECK_FALSE(parse_trimmed_bytes("/: trimmed\n").has_value());
    // The word with nothing countable in front of it.
    CHECK_FALSE(parse_trimmed_bytes("some bytes\n").has_value());
    CHECK_FALSE(parse_trimmed_bytes("bytes").has_value());
}

TEST_CASE("a byte count too large to hold is reported as none", "[ops][trim]") {
    // Rather than a wrapped number that would read as a real measurement.
    CHECK_FALSE(parse_trimmed_bytes("/: 99999999999999999999999 bytes trimmed\n").has_value());
}

TEST_CASE("the last byte count wins when a run trims more than one mount", "[ops][trim]") {
    // fstrim prints one line per filesystem. Only `/` is ever asked for here,
    // but a guest that prints a preamble must not shift the answer.
    CHECK(parse_trimmed_bytes("/boot: 512 bytes trimmed\n/: 4096 bytes trimmed\n") == 4096ULL);
}

TEST_CASE("trim refuses a WSL1 distribution", "[ops][trim]") {
    FakeWslHost host;
    TrimOperation operation{host, distro_named("Legacy-WSL1")};

    const auto plan = operation.plan();

    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::Preflight);
    CHECK(plan.error().remedy.find("--set-version") != std::string::npos);
}

TEST_CASE("trim plans one step and warns that it leaves the distribution running", "[ops][trim]") {
    FakeWslHost host;
    TrimOperation operation{host, distro_named("Ubuntu")};

    const auto plan = operation.plan();

    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 1);
    CHECK(plan->steps[0].mutates);
    // There is nothing to put back: what changed is which blocks the guest has
    // told the disk it no longer needs.
    CHECK(plan->steps[0].is_irreversible());
    REQUIRE(plan->warnings.size() == 1);
    CHECK(plan->warnings[0].remedy.find("wsl --terminate Ubuntu") != std::string::npos);
}

TEST_CASE("trim runs fstrim -v on the root of the distribution", "[ops][trim]") {
    FakeWslHost host;
    host.on_command("/sbin/fstrim", WslCommandResult{.standard_output = "/: 1078939029504 bytes trimmed\n"});
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = run(operation, sink, RunOptions{});

    REQUIRE(outcome.has_value());
    REQUIRE(host.commands().size() == 1);
    CHECK(host.commands()[0].distribution == "Ubuntu");
    // `-av` is never used: busybox rejects `-a` outright (spike #1).
    CHECK(host.commands()[0].argv == std::vector<std::string>{"/sbin/fstrim", "-v", "/"});
    CHECK(operation.trimmed_bytes() == 1078939029504ULL);
    CHECK_FALSE(operation.used_fallback());
}

TEST_CASE("trim retries without -v when the guest will not take it", "[ops][trim]") {
    // What busybox does: refuse the option, then trim when asked plainly.
    const std::string wording = GENERATE(std::string{"fstrim: unrecognized option: v\n"},
                                         std::string{"fstrim: invalid option -- 'v'\n"});

    FakeWslHost host;
    host.on_commands("/sbin/fstrim", {WslCommandResult{.exit_code = 1, .standard_error = wording},
                                      WslCommandResult{.standard_output = "/: 4096 bytes trimmed\n"}});
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = operation.execute(sink);

    REQUIRE(outcome.has_value());
    REQUIRE(host.commands().size() == 2);
    CHECK(host.commands()[1].argv == std::vector<std::string>{"/sbin/fstrim", "/"});
    CHECK(operation.used_fallback());
    CHECK(sink.said("retrying without it"));
    CHECK(operation.trimmed_bytes() == 4096ULL);
}

TEST_CASE("trim reports nothing when the fallback spelling says nothing", "[ops][trim]") {
    // Plain `fstrim /` prints nothing at all. No figure is better than a zero,
    // which would read as "nothing was freed".
    FakeWslHost host;
    host.on_commands("/sbin/fstrim", {refuses_the_option(), WslCommandResult{}});
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = operation.execute(sink);

    REQUIRE(outcome.has_value());
    CHECK(operation.used_fallback());
    CHECK_FALSE(operation.trimmed_bytes().has_value());
}

TEST_CASE("trim does not retry a failure that is not about an option", "[ops][trim]") {
    // Retrying would only hide the real reason.
    FakeWslHost host;
    host.on_command("/sbin/fstrim",
                    WslCommandResult{.exit_code = 1, .standard_error = "fstrim: /: no such file\n"});
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = operation.execute(sink);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(host.commands().size() == 1);
    CHECK_FALSE(operation.used_fallback());
}

TEST_CASE("trim reports what the guest said when fstrim fails", "[ops][trim]") {
    FakeWslHost host;
    host.on_command("/sbin/fstrim", WslCommandResult{.exit_code = 1,
                                                     .standard_error = "fstrim: /: FITRIM ioctl failed: "
                                                                       "Operation not supported\n"});
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = operation.execute(sink);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("FITRIM ioctl failed") != std::string::npos);
    // One line: the message is printed as `error: <message> -- <remedy>`.
    CHECK(outcome.error().message.find('\n') == std::string::npos);
    CHECK(outcome.error().remedy.find("/sbin/fstrim") != std::string::npos);
}

TEST_CASE("trim falls back to stdout when the guest said nothing on stderr", "[ops][trim]") {
    FakeWslHost host;
    host.on_command("/sbin/fstrim", WslCommandResult{.exit_code = 32, .standard_output = "cannot open /\n"});
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = operation.execute(sink);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("cannot open /") != std::string::npos);
}

TEST_CASE("trim reports wsl.exe failing to answer", "[ops][trim]") {
    // Not the same as the command inside returning non-zero, and reported
    // differently: nothing ran in the guest at all.
    FakeWslHost host;
    host.fail_command(wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = operation.execute(sink);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message == "wsl.exe did not answer");
}

TEST_CASE("trim reports wsl.exe failing to answer the retry", "[ops][trim]") {
    // The first call refuses the option and the second cannot reach wsl.exe at
    // all -- a different path from the first call failing.
    FakeWslHost host;
    host.on_command("/sbin/fstrim", refuses_the_option());
    host.fail_command_from(
        2, wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = operation.execute(sink);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message == "wsl.exe did not answer");
    // Two calls, so the retry is what failed rather than the first attempt.
    CHECK(host.commands().size() == 1);
}

TEST_CASE("trim runs nothing on a dry run", "[ops][trim]") {
    FakeWslHost host;
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    const auto outcome = run(operation, sink, RunOptions{.dry_run = true});

    REQUIRE(outcome.has_value());
    CHECK(host.commands().empty());
}

TEST_CASE("trim has nothing to verify and nothing to undo", "[ops][trim]") {
    // fstrim exiting zero is the whole claim, and no previous state was
    // replaced. Both are assertions about the contract, not about behaviour
    // that could change quietly.
    FakeWslHost host;
    TrimOperation operation{host, distro_named("Ubuntu")};
    RecordingSink sink;

    CHECK(operation.verify().has_value());
    operation.rollback(sink);
    CHECK(sink.messages.empty());
}
