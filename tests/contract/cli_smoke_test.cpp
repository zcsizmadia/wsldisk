// Runs the real wsldisk.exe. This is the only test that exercises main(), and
// it is what the "wsldisk --version works" roadmap item is checked against.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <sstream>
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

TEST_CASE("the executable lists distributions", "[contract][cli]") {
    // The only place the `list` wiring runs end to end: the unit tests drive
    // gather() and the renderers directly with fakes, which is deliberate, but
    // leaves the plumbing between them and the real services untested.
    const ProcessOutput result = run_process(quoted_exe() + " list");

    INFO(result.output);
    if (result.exit_code != 0) {
        // A machine with no WSL has no Lxss key, which is a preflight failure
        // with a remedy -- not a crash and not an empty screen.
        CHECK(result.exit_code == 3);
        CHECK(result.output.find("error:") != std::string::npos);
        return;
    }
    // The header is printed whether or not any distribution is registered.
    CHECK(result.output.find("NAME") != std::string::npos);
    CHECK(result.output.find("SIZE ON DISK") != std::string::npos);
}

TEST_CASE("the executable lists distributions as json", "[contract][cli]") {
    const ProcessOutput result = run_process(quoted_exe() + " list --json");

    INFO(result.output);
    // Whatever happened, stdout is parseable: every line is an object, and an
    // error is an object too. That is the promise `--json` makes.
    std::istringstream lines{result.output};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        INFO("line: " << line);
        CHECK(line.front() == '{');
        CHECK(line.back() == '}');
    }
}

TEST_CASE("an unknown subcommand is a usage error", "[contract][cli]") {
    const ProcessOutput result = run_process(quoted_exe() + " nonsense");

    CHECK(result.exit_code == 2);
    CHECK(result.output.find("error:") != std::string::npos);
}
