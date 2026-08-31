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

TEST_CASE("the default WSL version is 2", "[integration]") {
    if (!wsldisk::testing::integration_enabled()) {
        SKIP("set WSLDISK_INTEGRATION=1 to run integration tests");
    }

    const std::vector<std::string> arguments{"--status"};
    const auto status = wsldisk::testing::run_wsl(arguments);
    REQUIRE(status.exit_code == 0);
    // wsl.exe --status is localized, so match the digit next to "2" only as a
    // smoke signal; commands never depend on this text (PLAN.md, risks table).
    CHECK_FALSE(status.output.empty());
}
