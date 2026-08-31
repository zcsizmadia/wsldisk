// Contract tests for the completion scripts: each one is handed to the shell it
// claims to be for, and the shell is asked whether it parses.
//
// The unit tests assert the scripts *mention* every command and flag, which a
// generator can satisfy while emitting something no shell will load. Only the
// shell can answer the other half. An apostrophe in one command description was
// enough to break the zsh script while every unit assertion still passed.
//
// A shell that is not on this machine is a skip, not a failure: the runners have
// pwsh and bash, and zsh only sometimes.

#include <windows.h>

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "commands.h"
#include "completion_command.h"

namespace {

/// A file under %TEMP% that removes itself.
class TempScript {
public:
    TempScript(std::string_view name, const std::string& contents)
        : path_(std::filesystem::temp_directory_path() /
                ("wsldisk-completion-" + std::to_string(::GetCurrentProcessId()) + "-" +
                 std::to_string(++counter) + "-" + std::string{name})) {
        std::ofstream out(path_, std::ios::binary);
        out << contents;
    }

    ~TempScript() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempScript(const TempScript&) = delete;
    TempScript& operator=(const TempScript&) = delete;
    TempScript(TempScript&&) = delete;
    TempScript& operator=(TempScript&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    static int counter;
    std::filesystem::path path_;
};

int TempScript::counter = 0;

/// Runs a command line and returns its exit code, or -1 if it could not start.
[[nodiscard]] int run(const std::string& command_line) {
    std::FILE* pipe = ::_popen((command_line + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return -1;
    }
    std::string output;
    std::array<char, 512> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const int status = ::_pclose(pipe);
    INFO(output);
    return status;
}

/// Whether a program answers at all, so a missing shell skips rather than fails.
[[nodiscard]] bool available(const std::string& probe) {
    return run(probe) == 0;
}

/// The script the real command tree generates for one shell.
[[nodiscard]] std::string script_for(std::string_view shell) {
    CLI::App app{"Compact, shrink, move, inspect and snapshot WSL2 virtual disks", "wsldisk"};
    wsldisk::cli::CommandOptions options;
    wsldisk::cli::add_all_commands(app, options);
    return wsldisk::cli::generate_completion(app, shell);
}

}  // namespace

TEST_CASE("the bash script parses in bash", "[contract][completion]") {
    if (!available("bash -c \"exit 0\"")) {
        SKIP("bash is not on this machine");
    }

    const TempScript script{"completion.bash", script_for("bash")};

    // `-n` reads and parses without running: loading it for real would register
    // a completion in the shell this test spawned, which is not its business.
    //
    // Fed on stdin rather than named as an argument. On Windows `bash` may be
    // `C:\Windows\System32ash.exe`, the WSL launcher, which answers a probe
    // happily and then cannot open a Windows path. cmd does the redirection, so
    // the shell only ever sees bytes.
    CHECK(run("bash -n < \"" + script.path().string() + "\"") == 0);
}

TEST_CASE("the zsh script parses in zsh", "[contract][completion]") {
    if (!available("zsh -c \"exit 0\"")) {
        SKIP("zsh is not on this machine");
    }

    const TempScript script{"completion.zsh", script_for("zsh")};

    // On stdin, for the same reason as bash.
    CHECK(run("zsh -n < \"" + script.path().string() + "\"") == 0);
}

TEST_CASE("the powershell script parses in powershell", "[contract][completion]") {
    if (!available("pwsh -NoProfile -Command \"exit 0\"")) {
        SKIP("pwsh is not on this machine");
    }

    const TempScript script{"completion.ps1", script_for("powershell")};

    // The parser rather than the interpreter: running it would register a
    // completer in the spawned session for no gain.
    const std::string command =
        "pwsh -NoProfile -Command \"$e = $null; "
        "[void][System.Management.Automation.Language.Parser]::ParseFile('" +
        script.path().string() + "', [ref]$null, [ref]$e); if ($e.Count) { $e; exit 1 }\"";
    CHECK(run(command) == 0);
}
