// Runs the real wsldisk.exe. This is the only test that exercises main(), and
// it is what the "wsldisk --version works" roadmap item is checked against.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <string>

#include "version.h"

namespace {

struct ProcessOutput {
    int exit_code = 0;
    std::string output;
};

/// Runs a command line and captures its stdout. stderr is folded in so a failure
/// message shows up in the assertion instead of vanishing.
ProcessOutput run_process(const std::string& command_line) {
    const std::string redirected = "\"" + command_line + "\" 2>&1";
    std::FILE* pipe = ::_popen(redirected.c_str(), "r");
    REQUIRE(pipe != nullptr);

    std::string output;
    std::array<char, 512> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    return {.exit_code = ::_pclose(pipe), .output = output};
}

std::string quoted_exe() {
    return std::string{"\""} + WSLDISK_EXE_PATH + "\"";
}

}  // namespace

TEST_CASE("the executable prints its version banner and exits 0", "[contract][cli]") {
    const auto result = run_process(quoted_exe() + " --version");
    CHECK(result.exit_code == 0);
    CHECK(result.output.find(wsldisk::version_banner()) != std::string::npos);
}

TEST_CASE("the executable prints usage and exits 0", "[contract][cli]") {
    const auto result = run_process(quoted_exe() + " --help");
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("wsldisk [OPTIONS]") != std::string::npos);
}

TEST_CASE("the executable rejects an unknown flag with exit code 2", "[contract][cli]") {
    const auto result = run_process(quoted_exe() + " --not-a-flag");
    CHECK(result.exit_code == 2);
    CHECK(result.output.find("error:") != std::string::npos);
}
