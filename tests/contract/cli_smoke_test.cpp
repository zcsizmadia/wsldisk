// Runs the real wsldisk.exe. This is the only test that exercises main(), and
// it is what the "wsldisk --version works" roadmap item is checked against.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

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

TEST_CASE("the executable describes one distribution", "[contract][cli]") {
    // The `info` half of the subcommand dispatch, which the unit tests reach
    // through run_info rather than through main.
    const ProcessOutput result = run_process(quoted_exe() + " info wsldisk-no-such-distro");

    INFO(result.output);
    // 10 when the registry could be read and the name is not there, 3 when
    // there is no WSL on this machine at all. Both are answers; a crash is not.
    CHECK((result.exit_code == 10 || result.exit_code == 3));
    CHECK(result.output.find("error:") != std::string::npos);
}

TEST_CASE("info requires a distribution name", "[contract][cli]") {
    const ProcessOutput result = run_process(quoted_exe() + " info");

    CHECK(result.exit_code == 2);
}

TEST_CASE("the executable reaches the orphans command", "[contract][cli]") {
    // The `orphans` third of the subcommand dispatch. Deliberately a --relink
    // of a name that cannot exist rather than a scan: a scan prints the real
    // paths on this machine, and a test has no business putting a developer's
    // home directory into CI output.
    const ProcessOutput result =
        run_process(quoted_exe() + " orphans --relink wsldisk-no-such-distro --to " + quoted_exe());

    INFO(result.output);
    // 10 when the registry could be read and the name is not there, 3 when
    // there is no WSL on this machine at all.
    CHECK((result.exit_code == 10 || result.exit_code == 3));
    CHECK(result.output.find("error:") != std::string::npos);
}

TEST_CASE("orphans --relink needs somewhere to point", "[contract][cli]") {
    // `--to` is declared as a requirement of `--relink`, so half an instruction
    // is a usage error rather than a partial rewrite of the registry.
    const ProcessOutput result = run_process(quoted_exe() + " orphans --relink Ubuntu");

    CHECK(result.exit_code == 2);
}

TEST_CASE("the executable reaches the relink command", "[contract][cli]") {
    // The `relink` branch of the subcommand dispatch. A name that cannot exist,
    // so the lookup fails before anything is written -- running the suite never
    // repoints a distribution on this machine.
    const ProcessOutput result = run_process(quoted_exe() + " relink wsldisk-no-such-distro " + quoted_exe());

    INFO(result.output);
    // 10 when the registry could be read and the name is not there, 3 when
    // there is no WSL on this machine at all.
    CHECK((result.exit_code == 10 || result.exit_code == 3));
    CHECK(result.output.find("error:") != std::string::npos);
}

TEST_CASE("relink needs both a distribution and a path", "[contract][cli]") {
    // Half an instruction is a usage error rather than a partial rewrite of the
    // registry.
    CHECK(run_process(quoted_exe() + " relink").exit_code == 2);
    CHECK(run_process(quoted_exe() + " relink wsldisk-no-such-distro").exit_code == 2);
}

TEST_CASE("the executable reaches the trim command", "[contract][cli]") {
    // The `trim` quarter of the subcommand dispatch. A name that cannot exist,
    // so nothing on this machine is trimmed by running the suite.
    const ProcessOutput result = run_process(quoted_exe() + " trim wsldisk-no-such-distro");

    INFO(result.output);
    // 10 when the registry could be read and the name is not there, 3 when
    // there is no WSL on this machine at all.
    CHECK((result.exit_code == 10 || result.exit_code == 3));
    CHECK(result.output.find("error:") != std::string::npos);
}

TEST_CASE("trim requires a distribution name", "[contract][cli]") {
    const ProcessOutput result = run_process(quoted_exe() + " trim");

    CHECK(result.exit_code == 2);
}

TEST_CASE("the executable reaches the compact command", "[contract][cli]") {
    // The `compact` quarter of the subcommand dispatch. A name that cannot
    // exist, so running the suite never compacts anything on this machine.
    const ProcessOutput result = run_process(quoted_exe() + " compact wsldisk-no-such-distro");

    INFO(result.output);
    // 10 when the registry could be read and the name is not there, 3 when
    // there is no WSL on this machine at all.
    CHECK((result.exit_code == 10 || result.exit_code == 3));
    CHECK(result.output.find("error:") != std::string::npos);
}

TEST_CASE("compact needs to be told what to compact", "[contract][cli]") {
    // Not a default of "everything": a command that reaches for every disk when
    // given no target would be the worst possible guess.
    const ProcessOutput result = run_process(quoted_exe() + " compact");

    CHECK(result.exit_code == 2);
    CHECK(result.output.find("name one distribution") != std::string::npos);
}

TEST_CASE("compact refuses two targets at once", "[contract][cli]") {
    const ProcessOutput result = run_process(quoted_exe() + " compact wsldisk-no-such-distro --all");

    CHECK(result.exit_code == 2);
}

TEST_CASE("the executable reaches the config command", "[contract][cli]") {
    // `config path` reads nothing and changes nothing, so the suite can prove
    // the dispatch works without touching the developer's own config file.
    const ProcessOutput result = run_process(quoted_exe() + " config path");

    INFO(result.output);
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("config.toml") != std::string::npos);
}

TEST_CASE("config refuses a setting that does not exist", "[contract][cli]") {
    const ProcessOutput result = run_process(quoted_exe() + " config get wsldisk.no.such.setting");

    CHECK(result.exit_code == 2);
    CHECK(result.output.find("error:") != std::string::npos);
}

TEST_CASE("the executable emits a completion script", "[contract][cli]") {
    // `completion` reads nothing and changes nothing, so it can be run for real
    // here. It also has to work on a machine with no WSL at all, which is often
    // exactly when someone is setting their shell up.
    const ProcessOutput result = run_process(quoted_exe() + " completion bash");

    INFO(result.output);
    CHECK(result.exit_code == 0);
    CHECK(result.output.find("complete -F _wsldisk wsldisk") != std::string::npos);
}

TEST_CASE("completion refuses a shell it does not know", "[contract][cli]") {
    const ProcessOutput result = run_process(quoted_exe() + " completion fish");

    CHECK(result.exit_code == 2);
    CHECK(result.output.find("error:") != std::string::npos);
}

TEST_CASE("every command's --json stdout is empty or parses, however it ends", "[contract][cli]") {
    // The check that would have caught all six of them.
    //
    // `--json` was honoured by the happy path of each command and quietly
    // ignored somewhere else: `orphans --delete` printed a table, a prompt and
    // a Docker warning; four commands printed their `--dry-run` plan as prose;
    // three `config` verbs printed bare values; `compact --all` printed a
    // sentence when there was nothing to do; and a usage error printed nothing
    // at all. Every one of those was a command whose *other* paths were tested.
    //
    // So this asserts the property rather than the paths: whatever a command
    // does, its stdout under `--json` is either empty or JSON, line by line.
    //
    // Every distribution named here is one that cannot exist, so a command that
    // gets further than the lookup still changes nothing on the machine running
    // the suite.
    const std::vector<std::string> commands{
        "list --json",
        "list --json --probe",
        "info wsldisk-no-such-distro --json",
        "info --json",  // missing positional: a usage error
        "orphans --json",
        "orphans --json --scan wsldisk-no-such-directory",
        "orphans --relink wsldisk-no-such-distro --to wsldisk-no-such-file --json",
        "orphans --delete --yes --json --scan wsldisk-no-such-directory",
        "orphans --delete --yes --json --dry-run --scan wsldisk-no-such-directory",
        "trim wsldisk-no-such-distro --json",
        "trim wsldisk-no-such-distro --json --dry-run",
        "trim --json",  // missing positional
        "relink wsldisk-no-such-distro wsldisk-no-such-file --json",
        "relink wsldisk-no-such-distro wsldisk-no-such-file --json --dry-run",
        "relink --json",  // missing positionals
        "compact wsldisk-no-such-distro --json",
        "compact wsldisk-no-such-distro --json --dry-run",
        "compact --file wsldisk-no-such-file --json",
        "compact --json",  // no target named
        "config --json",
        "config path --json",
        "config get --json",
        "config get compact.trim --json",
        "config get wsldisk.no.such.key --json",
        "config set compact.trim true --json --dry-run",
        "config set wsldisk.no.such.key x --json",
        "completion bash --json",  // refused: `completion` prints a script, not data
        "--not-a-flag --json",     // a usage error before any command
    };

    for (const std::string& command : commands) {
        const ProcessOutput result = run_process(quoted_exe() + " " + command);

        INFO("wsldisk " << command);
        INFO("exit " << result.exit_code);
        INFO("stdout: " << result.output);

        std::istringstream lines{result.output};
        std::string line;
        while (std::getline(lines, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            INFO("line: " << line);
            // Parsed rather than pattern-matched: `{` at each end would accept
            // plenty of things no JSON reader will.
            CHECK_NOTHROW(nlohmann::json::parse(line));
        }
    }
}
