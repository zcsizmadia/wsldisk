// Integration tests drive real WSL. They are compiled and registered
// everywhere, but skip themselves unless WSLDISK_INTEGRATION=1 is set, so a
// developer box or a runner without WSL still reports green instead of failing
// for the wrong reason.
//
// M0 ships only this presence check; the scenarios listed in docs/TESTING.md
// land alongside the commands they cover, starting in M1.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "integration_fixture.h"

TEST_CASE("WSL2 is available to the integration suite", "[integration]") {
    if (!wsldisk::testing::integration_enabled()) {
        SKIP("set WSLDISK_INTEGRATION=1 to run integration tests");
    }

    const std::vector<std::string> arguments{"--version"};
    const auto version = wsldisk::testing::run_wsl(arguments);
    INFO("wsl.exe --version said: " << version.output);
    REQUIRE(version.exit_code == 0);
    CHECK(version.output.find("WSL") != std::string::npos);
}

TEST_CASE("wsl --status answers", "[integration]") {
    // Named for what it checks. It used to be called "the default WSL version is
    // 2", which it never verified and could not: `wsl --status` is localized, and
    // the rule everywhere else in this codebase is that nothing parses localized
    // wsl.exe text (PLAN.md, risks table). A test that cannot fail on the
    // condition in its own name is worse than no test, because the green check
    // reads as an assurance.
    //
    // Nothing needs the machine default anyway: `ScratchDistro` passes
    // `--version 2` to every import explicitly, so the suite pins what it uses
    // rather than depending on how the runner is configured.
    if (!wsldisk::testing::integration_enabled()) {
        SKIP("set WSLDISK_INTEGRATION=1 to run integration tests");
    }

    const std::vector<std::string> arguments{"--status"};
    const auto status = wsldisk::testing::run_wsl(arguments);
    REQUIRE(status.exit_code == 0);
    CHECK_FALSE(status.output.empty());
}
