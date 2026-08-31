#include "config_command.h"

#include <windows.h>

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "errors.h"
#include "fake_clock.h"
#include "fake_filesystem.h"
#include "fake_registry.h"
#include "fake_virtual_disk.h"
#include "fake_wsl_host.h"
#include "logger.h"
#include "model/config.h"
#include "platform/win32_api.h"

using wsldisk::ErrorCode;
using wsldisk::exit_code_for;
using wsldisk::exit_code_success;
using wsldisk::cli::ConfigOptions;
using wsldisk::cli::editor_command;
using wsldisk::cli::GlobalOptions;
using wsldisk::cli::LaunchEditor;
using wsldisk::cli::load_configuration;
using wsldisk::cli::NullLogger;
using wsldisk::cli::run_config;
using wsldisk::cli::Services;
using wsldisk::cli::StreamLogger;
using wsldisk::testing::FakeClock;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;

namespace {

const std::filesystem::path config_file = LR"(C:\Users\example\AppData\Roaming\wsldisk\config.toml)";
const std::filesystem::path wslconfig_file = LR"(C:\Users\example\.wslconfig)";

struct Machine {
    FakeRegistry registry;
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    FakeClock clock;
    std::ostringstream errors;
    /// Every file the editor was asked to open.
    std::vector<std::string> opened;

    Machine() {
        filesystem.set_variable(L"APPDATA", LR"(C:\Users\example\AppData\Roaming)");
        filesystem.set_variable(L"USERPROFILE", LR"(C:\Users\example)");
    }

    [[nodiscard]] Services services() {
        return Services{.registry = &registry,
                        .filesystem = &filesystem,
                        .disks = &disks,
                        .host = &host,
                        .clock = &clock};
    }

    [[nodiscard]] LaunchEditor editor() {
        return [this](const std::filesystem::path& file) -> wsldisk::Status {
            opened.push_back(file.string());
            return {};
        };
    }

    [[nodiscard]] int run(const ConfigOptions& options, const GlobalOptions& global, std::ostream& out) {
        NullLogger logger{errors};
        return run_config(services(), options, global, logger, editor(), out, errors);
    }
};

}  // namespace

TEST_CASE("config path prints where the file lives", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "path"}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str() == config_file.string() + "\n");
}

TEST_CASE("config path works even when the file is unreadable", "[cli][config]") {
    // `config path` is what someone runs *because* the file is broken. It must
    // not need to read it.
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact\n");
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{.action = "path"}, GlobalOptions{}, out) == exit_code_success);
}

TEST_CASE("config path reports an environment it cannot expand", "[cli][config]") {
    Machine machine;
    machine.filesystem.fail_queries(
        wsldisk::Error{ErrorCode::Preflight, "no environment", "check the shell"});
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{.action = "path"}, GlobalOptions{}, out) ==
          exit_code_for(ErrorCode::Preflight));
}

TEST_CASE("config with no file shows the defaults", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str().find("compact.trim:") != std::string::npos);
    CHECK(out.str().find("true") != std::string::npos);
    // Nothing about .wslconfig, because there is none.
    CHECK(out.str().find("read-only") == std::string::npos);
}

TEST_CASE("config shows the read-only wslconfig keys under its own", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(wslconfig_file,
                                     "[wsl2]\ndefaultVhdSize = 256GB\nswapFile = D:\\swap.vhdx\n");
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str().find("from .wslconfig (read-only)") != std::string::npos);
    CHECK(out.str().find("wsl2.defaultVhdSize") != std::string::npos);
    CHECK(out.str().find("256GB") != std::string::npos);
    // Never written: it belongs to WSL.
    CHECK_FALSE(machine.filesystem.text_of(wslconfig_file)->empty());
    CHECK(machine.filesystem.text_of(wslconfig_file)->find("wsldisk") == std::string::npos);
}

TEST_CASE("config ignores a wslconfig it cannot read", "[cli][config]") {
    // Most machines have none, and it is not this command's subject.
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact]\ntrim = false\n");
    machine.filesystem.add_directory(wslconfig_file);
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{}, GlobalOptions{}, out) == exit_code_success);
}

TEST_CASE("config renders itself as one json object", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact]\ntrim = false\n");
    machine.filesystem.add_text_file(wslconfig_file, "[wsl2]\nvhdSize = 1TB\n");
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{}, GlobalOptions{.json = true}, out);

    CHECK(code == exit_code_success);
    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object["path"] == config_file.string());
    CHECK(object["settings"]["compact.trim"] == "false");
    CHECK(object["wslconfig"]["vhdSize"] == "1TB");
}

TEST_CASE("config json leaves out wslconfig keys that are not set", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{}, GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK_FALSE(object.contains("wslconfig"));
}

TEST_CASE("config reports a file that will not parse", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact\n");
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Usage));
    CHECK(machine.errors.str().find("line 1") != std::string::npos);
}

TEST_CASE("config get with no key prints every setting", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "get"}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    for (const std::string& key : wsldisk::model::config_keys()) {
        CHECK(out.str().find(key + " = ") != std::string::npos);
    }
}

TEST_CASE("config get prints one setting on its own", "[cli][config]") {
    // On its own so `for /f` and `$(...)` get the value and nothing else.
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[wsl]\nunlock_timeout_seconds = 30\n");
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "get", .key = "wsl.unlock_timeout_seconds"},
                                 GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str() == "30\n");
}

TEST_CASE("config get refuses a key that does not exist", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    const int code =
        machine.run(ConfigOptions{.action = "get", .key = "compact.trimm"}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Usage));
    CHECK(machine.errors.str().find("compact.trimm") != std::string::npos);
    CHECK(machine.errors.str().find("compact.trim,") != std::string::npos);
}

TEST_CASE("config set writes the value and prints it back", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "set", .key = "compact.restart", .value = "true"},
                                 GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(out.str() == "compact.restart = true\n");

    const auto written = machine.filesystem.text_of(config_file);
    REQUIRE(written.has_value());
    // Written through the serialiser, so it parses back to what was asked for.
    const auto reparsed = wsldisk::model::parse_config(*written);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->compact_restart);
}

TEST_CASE("config set creates the directory the first time", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{.action = "set", .key = "compact.trim", .value = "false"},
                      GlobalOptions{}, out) == exit_code_success);

    CHECK(machine.filesystem.created_directories() ==
          std::vector<std::wstring>{config_file.parent_path().wstring()});
}

TEST_CASE("config set keeps the settings it was not asked to change", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[scan]\ndirs = [\"D:\\\\WSL\"]\n");
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{.action = "set", .key = "compact.trim", .value = "false"},
                      GlobalOptions{}, out) == exit_code_success);

    const auto reparsed = wsldisk::model::parse_config(*machine.filesystem.text_of(config_file));
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->scan_dirs == std::vector<std::string>{R"(D:\WSL)"});
    CHECK_FALSE(reparsed->compact_trim);
}

TEST_CASE("config set refuses a value the setting cannot take", "[cli][config]") {
    // Refused before anything is written, rather than written and rejected on
    // the next run.
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "set", .key = "compact.trim", .value = "maybe"},
                                 GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Usage));
    CHECK_FALSE(machine.filesystem.text_of(config_file).has_value());
}

TEST_CASE("config set refuses a key that does not exist", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "set", .key = "compact.trimm", .value = "true"},
                                 GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::Usage));
    CHECK_FALSE(machine.filesystem.text_of(config_file).has_value());
}

TEST_CASE("config set reports a file it cannot write", "[cli][config]") {
    Machine machine;
    machine.filesystem.fail_write(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot write", "run as the owning user"});
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "set", .key = "compact.trim", .value = "false"},
                                 GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::NeedsElevation));
}

TEST_CASE("config set --dry-run writes nothing", "[cli][config]") {
    Machine machine;
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "set", .key = "compact.trim", .value = "false"},
                                 GlobalOptions{.dry_run = true}, out);

    CHECK(code == exit_code_success);
    CHECK_FALSE(machine.filesystem.text_of(config_file).has_value());
    CHECK(out.str().find("--dry-run") != std::string::npos);
    CHECK(out.str().find("compact.trim = false") != std::string::npos);
}

TEST_CASE("config edit opens the file", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact]\n");
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "edit"}, GlobalOptions{}, out);

    CHECK(code == exit_code_success);
    CHECK(machine.opened == std::vector<std::string>{config_file.string()});
}

TEST_CASE("config edit writes the file first when there is none", "[cli][config]") {
    // An editor opened on a file that does not exist leaves the user to write
    // the schema from memory.
    Machine machine;
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{.action = "edit"}, GlobalOptions{}, out) == exit_code_success);

    const auto written = machine.filesystem.text_of(config_file);
    REQUIRE(written.has_value());
    CHECK(written->find("[compact]") != std::string::npos);
    CHECK(machine.opened.size() == 1);
}

TEST_CASE("config edit reports a file it cannot create", "[cli][config]") {
    Machine machine;
    machine.filesystem.fail_write(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot write", "run as the owning user"});
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "edit"}, GlobalOptions{}, out);

    CHECK(code == exit_code_for(ErrorCode::NeedsElevation));
    CHECK(machine.opened.empty());
}

TEST_CASE("config edit reports an editor that will not start", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact]\n");
    std::ostringstream out;
    NullLogger logger{machine.errors};

    const LaunchEditor failing = [](const std::filesystem::path&) -> wsldisk::Status {
        return wsldisk::fail(ErrorCode::Generic, "notepad did not start", "set %EDITOR%");
    };
    const int code = run_config(machine.services(), ConfigOptions{.action = "edit"}, GlobalOptions{}, logger,
                                failing, out, machine.errors);

    CHECK(code == exit_code_for(ErrorCode::Generic));
    CHECK(machine.errors.str().find("notepad did not start") != std::string::npos);
}

TEST_CASE("config edit --dry-run opens nothing", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact]\n");
    std::ostringstream out;

    const int code = machine.run(ConfigOptions{.action = "edit"}, GlobalOptions{.dry_run = true}, out);

    CHECK(code == exit_code_success);
    CHECK(machine.opened.empty());
    CHECK(out.str().find("would open") != std::string::npos);
}

TEST_CASE("the editor is EDITOR when it is set", "[cli][config]") {
    FakeFileSystem filesystem;
    filesystem.set_variable(L"EDITOR", L"code --wait");

    CHECK(editor_command(filesystem) == "code --wait");
}

TEST_CASE("the editor falls back to notepad", "[cli][config]") {
    // An unset variable expands to itself, which is how "there is none" looks.
    const FakeFileSystem unset;
    CHECK(editor_command(unset) == "notepad");

    FakeFileSystem broken;
    broken.fail_queries(wsldisk::Error{ErrorCode::Preflight, "no environment", "check the shell"});
    CHECK(editor_command(broken) == "notepad");

    FakeFileSystem empty;
    empty.set_variable(L"EDITOR", L"");
    CHECK(editor_command(empty) == "notepad");
}

TEST_CASE("config shows only the wslconfig keys that are set", "[cli][config]") {
    // One key present and the others absent, each way round: a renderer that
    // printed a blank line for an unset key would be saying it is set to
    // nothing, which is not the same as not being there.
    Machine only_vhd_size;
    only_vhd_size.filesystem.add_text_file(wslconfig_file, "[wsl2]\nvhdSize = 1TB\n");
    std::ostringstream vhd_out;
    CHECK(only_vhd_size.run(ConfigOptions{}, GlobalOptions{}, vhd_out) == exit_code_success);
    CHECK(vhd_out.str().find("wsl2.vhdSize") != std::string::npos);
    CHECK(vhd_out.str().find("defaultVhdSize") == std::string::npos);
    CHECK(vhd_out.str().find("swapFile") == std::string::npos);

    Machine only_swap;
    only_swap.filesystem.add_text_file(wslconfig_file, "[wsl2]\nswapFile = D:\\swap.vhdx\n");
    std::ostringstream swap_out;
    CHECK(only_swap.run(ConfigOptions{}, GlobalOptions{}, swap_out) == exit_code_success);
    CHECK(swap_out.str().find("wsl2.swapFile") != std::string::npos);
    CHECK(swap_out.str().find("vhdSize") == std::string::npos);
}

TEST_CASE("config json shows only the wslconfig keys that are set", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(wslconfig_file, "[wsl2]\nswapFile = D:\\swap.vhdx\n");
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{}, GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object["wslconfig"]["swapFile"] == R"(D:\swap.vhdx)");
    CHECK_FALSE(object["wslconfig"].contains("vhdSize"));
    CHECK_FALSE(object["wslconfig"].contains("defaultVhdSize"));
}

TEST_CASE("config json shows a defaultVhdSize on its own", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(wslconfig_file, "[wsl2]\ndefaultVhdSize = 256GB\n");
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{}, GlobalOptions{.json = true}, out) == exit_code_success);

    const nlohmann::json object = nlohmann::json::parse(out.str());
    CHECK(object["wslconfig"]["defaultVhdSize"] == "256GB");
}

TEST_CASE("config ignores a wslconfig whose path cannot be built", "[cli][config]") {
    // Only the %USERPROFILE% lookup is broken: the config file itself is still
    // reachable, and a machine that cannot say where .wslconfig would be simply
    // has nothing to show from it.
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact]\ntrim = false\n");
    machine.filesystem.fail_variable(L"USERPROFILE",
                                     wsldisk::Error{ErrorCode::Preflight, "no profile", "check the shell"});
    std::ostringstream out;

    CHECK(machine.run(ConfigOptions{}, GlobalOptions{}, out) == exit_code_success);
    CHECK(out.str().find("read-only") == std::string::npos);
}

TEST_CASE("every config verb sets its action when parsed", "[cli][config]") {
    // The verbs are wired with CLI11 callbacks, which nothing else in these
    // tests goes through: `run_config` is handed an action rather than a
    // command line. A verb whose callback never fired would silently fall
    // through to `show`.
    struct Parsed {
        GlobalOptions global;
        ConfigOptions options;
    };

    const auto parse = [](const std::vector<std::string>& arguments) {
        auto parsed = std::make_unique<Parsed>();
        CLI::App app{"wsldisk", "wsldisk"};
        wsldisk::cli::add_config_command(app, parsed->global, parsed->options);
        // CLI11 parses in reverse order, the way `cli::run` hands it argv.
        std::vector<std::string> reversed(arguments.rbegin(), arguments.rend());
        app.parse(std::move(reversed));
        return parsed;
    };

    CHECK(parse({"config", "path"})->options.action == "path");
    CHECK(parse({"config", "edit"})->options.action == "edit");

    const auto got = parse({"config", "get", "compact.trim"});
    CHECK(got->options.action == "get");
    CHECK(got->options.key == "compact.trim");

    const auto set = parse({"config", "set", "compact.trim", "false"});
    CHECK(set->options.action == "set");
    CHECK(set->options.key == "compact.trim");
    CHECK(set->options.value == "false");

    // Bare `config` is the show verb, which is why the action is empty.
    CHECK(parse({"config"})->options.action.empty());
}

TEST_CASE("config set needs both a key and a value", "[cli][config]") {
    GlobalOptions global;
    ConfigOptions options;
    CLI::App app{"wsldisk", "wsldisk"};
    wsldisk::cli::add_config_command(app, global, options);

    std::vector<std::string> arguments{"compact.trim", "set", "config"};
    CHECK_THROWS_AS(app.parse(std::move(arguments)), CLI::RequiredError);
}

TEST_CASE("open_in_editor runs the editor the environment names", "[cli][config]") {
    // The production `LaunchEditor`, driven through the Win32 injection table
    // so nothing is actually started.
    std::wstring command_line;
    wsldisk::platform::Win32Api api;
    api.create_process = [&command_line](LPCWSTR, LPWSTR line, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                         BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                         LPPROCESS_INFORMATION information) -> BOOL {
        command_line = line;
        information->hProcess = reinterpret_cast<HANDLE>(0x70);
        information->hThread = reinterpret_cast<HANDLE>(0x71);
        return TRUE;
    };
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_OBJECT_0; };
    api.close_handle = [](HANDLE) -> BOOL { return TRUE; };
    const wsldisk::platform::ScopedWin32Api scoped{api};

    FakeFileSystem filesystem;
    filesystem.set_variable(L"EDITOR", L"myeditor");

    REQUIRE(wsldisk::cli::open_in_editor(filesystem, R"(C:\config.toml)").has_value());
    CHECK(command_line == LR"(myeditor C:\config.toml)");
}

// `load_configuration` is what every command runs with. It used to sit inline in
// `app.cpp`, beside the real Win32 services, where neither failure path could be
// reached from a test -- and both of them decide what `compact` does.

TEST_CASE("load_configuration reads the file when there is one", "[cli][config]") {
    Machine machine;
    machine.filesystem.add_text_file(config_file, "[compact]\nrestart = true\n");
    NullLogger logger{machine.errors};

    const auto config = load_configuration(machine.filesystem, logger);

    CHECK(config.compact_restart);
    CHECK(machine.errors.str().empty());
}

TEST_CASE("load_configuration falls back to defaults when there is no file", "[cli][config]") {
    // The ordinary case on a machine nobody has configured. Not a warning: there
    // is nothing wrong.
    Machine machine;
    NullLogger logger{machine.errors};

    const auto config = load_configuration(machine.filesystem, logger);

    CHECK(config.compact_trim);
    CHECK_FALSE(config.compact_restart);
    CHECK(machine.errors.str().empty());
}

TEST_CASE("load_configuration warns and carries on when the file will not parse", "[cli][config]") {
    // Still the defaults, and still exit zero from whatever command asked. The
    // user wanted to compact a disk, not to have their settings audited.
    Machine machine;
    machine.filesystem.add_text_file(config_file, "this is not toml [[[\n");
    StreamLogger logger{machine.errors, false, {}};

    const auto config = load_configuration(machine.filesystem, logger);

    CHECK(config.compact_trim);
    CHECK(machine.errors.str().find("ignoring") != std::string::npos);
}

TEST_CASE("load_configuration warns when it cannot even find the file", "[cli][config]") {
    // `%APPDATA%` cannot be expanded, so `config_path` fails before there is a
    // file to read at all. Still the defaults, still no refusal.
    Machine machine;
    machine.filesystem.fail_variable(
        L"APPDATA", wsldisk::Error{ErrorCode::Generic, "no such variable", "check the environment"});
    StreamLogger logger{machine.errors, false, {}};

    const auto config = load_configuration(machine.filesystem, logger);

    CHECK(config.compact_trim);
    CHECK(machine.errors.str().find("built-in defaults") != std::string::npos);
}

TEST_CASE("editor_command quotes a program path that has a space", "[cli][config]") {
    // It exists, so the whole value is a path rather than a command with
    // arguments, and CreateProcessW needs it quoted to find it.
    Machine machine;
    const std::filesystem::path editor = LR"(C:\Program Files\editor\ed.exe)";
    machine.filesystem.set_variable(L"EDITOR", editor.wstring());
    machine.filesystem.add_file(editor, FakeFileSystem::File{});

    CHECK(editor_command(machine.filesystem) == R"("C:\Program Files\editor\ed.exe")");
}

TEST_CASE("editor_command leaves an editor with arguments alone", "[cli][config]") {
    // `code --wait` names no file, so it is a command line and must be passed
    // through as written. Quoting it made `wsldisk config edit` fail for every
    // VS Code user.
    Machine machine;
    machine.filesystem.set_variable(L"EDITOR", L"code --wait");

    CHECK(editor_command(machine.filesystem) == "code --wait");
}

TEST_CASE("editor_command leaves a bare program name alone", "[cli][config]") {
    Machine machine;
    machine.filesystem.set_variable(L"EDITOR", L"vim");

    CHECK(editor_command(machine.filesystem) == "vim");
}
