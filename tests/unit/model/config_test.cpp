#include "model/config.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "fake_filesystem.h"

using wsldisk::ErrorCode;
using wsldisk::model::Config;
using wsldisk::model::config_keys;
using wsldisk::model::config_path;
using wsldisk::model::get_config_value;
using wsldisk::model::load_config;
using wsldisk::model::parse_config;
using wsldisk::model::parse_wslconfig;
using wsldisk::model::render_config;
using wsldisk::model::set_config_value;
using wsldisk::model::wslconfig_path;
using wsldisk::testing::FakeFileSystem;

namespace {

/// A filesystem with the two variables the config paths need.
FakeFileSystem machine() {
    FakeFileSystem filesystem;
    filesystem.set_variable(L"APPDATA", LR"(C:\Users\example\AppData\Roaming)");
    filesystem.set_variable(L"USERPROFILE", LR"(C:\Users\example)");
    return filesystem;
}

}  // namespace

TEST_CASE("an empty config is the defaults", "[model][config]") {
    const auto config = parse_config("");

    REQUIRE(config.has_value());
    CHECK(config->scan_dirs.empty());
    CHECK(config->compact_trim);
    CHECK_FALSE(config->compact_restart);
    CHECK(config->unlock_timeout_seconds == 5);
}

TEST_CASE("every key is read from the file", "[model][config]") {
    const auto config = parse_config(R"(
[scan]
dirs = ["D:\\WSL", "E:\\disks"]

[compact]
trim = false
restart = true

[wsl]
unlock_timeout_seconds = 30
)");

    REQUIRE(config.has_value());
    CHECK(config->scan_dirs == std::vector<std::string>{R"(D:\WSL)", R"(E:\disks)"});
    CHECK_FALSE(config->compact_trim);
    CHECK(config->compact_restart);
    CHECK(config->unlock_timeout_seconds == 30);
    CHECK(config->unlock_timeout() == std::chrono::seconds{30});
}

TEST_CASE("a partial config keeps the defaults for the rest", "[model][config]") {
    // The common case: a user sets one thing. Every other setting has to keep
    // behaving as it did.
    const auto config = parse_config("[compact]\nrestart = true\n");

    REQUIRE(config.has_value());
    CHECK(config->compact_restart);
    CHECK(config->compact_trim);
    CHECK(config->unlock_timeout_seconds == 5);
}

TEST_CASE("a malformed config says where", "[model][config]") {
    const auto config = parse_config("[compact]\ntrim = \n");

    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == ErrorCode::Usage);
    // The position is the whole value of the message: without it the user has
    // to read the file top to bottom.
    CHECK(config.error().message.find("line 2") != std::string::npos);
    CHECK(config.error().remedy.find("delete the file") != std::string::npos);
}

TEST_CASE("a key this version does not know is ignored", "[model][config]") {
    // A config written by a later version has to stay readable by an earlier
    // one, or every upgrade is a breaking change.
    const auto config = parse_config("[compact]\ntrim = false\nsomething_new = 7\n");

    REQUIRE(config.has_value());
    CHECK_FALSE(config->compact_trim);
}

TEST_CASE("a scan entry that is not a string is skipped", "[model][config]") {
    const auto config = parse_config("[scan]\ndirs = [\"D:\\\\WSL\", 7, true]\n");

    REQUIRE(config.has_value());
    CHECK(config->scan_dirs == std::vector<std::string>{R"(D:\WSL)"});
}

TEST_CASE("scan.dirs of the wrong type is ignored rather than fatal", "[model][config]") {
    const auto config = parse_config("[scan]\ndirs = \"D:\\\\WSL\"\n");

    REQUIRE(config.has_value());
    CHECK(config->scan_dirs.empty());
}

TEST_CASE("an unlock timeout past the maximum is refused", "[model][config]") {
    // The disk is never released on a timer, so a long wait only delays a
    // refusal the user has to act on anyway.
    const auto config = parse_config("[wsl]\nunlock_timeout_seconds = 86400\n");

    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == ErrorCode::Usage);
    CHECK(config.error().message.find("3600") != std::string::npos);
}

TEST_CASE("a rendered config parses back to the same thing", "[model][config]") {
    Config config;
    config.scan_dirs = {R"(D:\WSL)", R"(E:\a "quoted" path)"};
    config.compact_trim = false;
    config.compact_restart = true;
    config.unlock_timeout_seconds = 42;

    const auto reparsed = parse_config(render_config(config));

    REQUIRE(reparsed.has_value());
    CHECK(reparsed->scan_dirs == config.scan_dirs);
    CHECK(reparsed->compact_trim == config.compact_trim);
    CHECK(reparsed->compact_restart == config.compact_restart);
    CHECK(reparsed->unlock_timeout_seconds == config.unlock_timeout_seconds);
}

TEST_CASE("the defaults round-trip too", "[model][config]") {
    const auto reparsed = parse_config(render_config(Config{}));

    REQUIRE(reparsed.has_value());
    CHECK(reparsed->compact_trim);
    CHECK(reparsed->unlock_timeout_seconds == 5);
    CHECK(reparsed->scan_dirs.empty());
}

TEST_CASE("every key can be read back", "[model][config]") {
    Config config;
    config.scan_dirs = {R"(D:\WSL)", R"(E:\disks)"};
    config.compact_trim = false;
    config.compact_restart = true;
    config.unlock_timeout_seconds = 30;

    CHECK(get_config_value(config, "scan.dirs") == R"(D:\WSL, E:\disks)");
    CHECK(get_config_value(config, "compact.trim") == "false");
    CHECK(get_config_value(config, "compact.restart") == "true");
    CHECK(get_config_value(config, "wsl.unlock_timeout_seconds") == "30");
    CHECK_FALSE(get_config_value(config, "nope").has_value());

    // Nothing in the list is unreadable, which is what makes `config get` with
    // no argument able to print them all.
    for (const std::string& key : config_keys()) {
        CHECK(get_config_value(config, key).has_value());
    }
}

TEST_CASE("every key can be set from text", "[model][config]") {
    Config config;

    CHECK(set_config_value(config, "compact.trim", "false").has_value());
    CHECK_FALSE(config.compact_trim);
    CHECK(set_config_value(config, "compact.restart", "true").has_value());
    CHECK(config.compact_restart);
    CHECK(set_config_value(config, "wsl.unlock_timeout_seconds", "30").has_value());
    CHECK(config.unlock_timeout_seconds == 30);
    CHECK(set_config_value(config, "scan.dirs", R"(D:\WSL; E:\disks)").has_value());
    CHECK(config.scan_dirs == std::vector<std::string>{R"(D:\WSL)", R"(E:\disks)"});
}

TEST_CASE("scan.dirs is separated by semicolons, not commas", "[model][config]") {
    // A Windows path can contain a comma and never a semicolon, which is why
    // PATH has used the same separator for thirty years.
    Config config;

    REQUIRE(set_config_value(config, "scan.dirs", R"(D:\Disks, Backups)").has_value());
    CHECK(config.scan_dirs == std::vector<std::string>{R"(D:\Disks, Backups)"});
}

TEST_CASE("setting scan.dirs to nothing clears it", "[model][config]") {
    Config config;
    config.scan_dirs = {R"(D:\WSL)"};

    REQUIRE(set_config_value(config, "scan.dirs", "").has_value());
    CHECK(config.scan_dirs.empty());
}

TEST_CASE("empty entries in scan.dirs are dropped", "[model][config]") {
    Config config;

    REQUIRE(set_config_value(config, "scan.dirs", R"(D:\WSL;;  ;E:\disks;)").has_value());
    CHECK(config.scan_dirs == std::vector<std::string>{R"(D:\WSL)", R"(E:\disks)"});
}

TEST_CASE("a boolean setting refuses anything but true or false", "[model][config]") {
    // Not 1/yes/on: TOML has one spelling, and accepting more would mean `set`
    // writes a file `get` reads back differently.
    Config config;

    for (const std::string value : {"1", "yes", "on", "True", ""}) {
        const auto status = set_config_value(config, "compact.trim", value);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::Usage);
        CHECK(status.error().remedy.find("true") != std::string::npos);
    }
}

TEST_CASE("the timeout setting refuses anything but a number in range", "[model][config]") {
    Config config;

    for (const std::string value : {"", "30s", "-1", "1e3", "86400", "99999999999999999999"}) {
        const auto status = set_config_value(config, "wsl.unlock_timeout_seconds", value);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::Usage);
    }
    CHECK(set_config_value(config, "wsl.unlock_timeout_seconds", "3600").has_value());
    CHECK(set_config_value(config, "wsl.unlock_timeout_seconds", "0").has_value());
}

TEST_CASE("an unknown key on set lists the ones that exist", "[model][config]") {
    Config config;

    const auto status = set_config_value(config, "compact.trimm", "true");

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Usage);
    CHECK(status.error().message.find("compact.trimm") != std::string::npos);
    for (const std::string& key : config_keys()) {
        CHECK(status.error().remedy.find(key) != std::string::npos);
    }
}

TEST_CASE("the config lives under APPDATA", "[model][config]") {
    const FakeFileSystem filesystem = machine();

    const auto path = config_path(filesystem);

    REQUIRE(path.has_value());
    CHECK(*path == std::filesystem::path{LR"(C:\Users\example\AppData\Roaming\wsldisk\config.toml)"});
}

TEST_CASE("a config path that cannot be expanded is reported", "[model][config]") {
    FakeFileSystem filesystem;
    filesystem.fail_queries(wsldisk::Error{ErrorCode::Preflight, "no environment", "check the shell"});

    CHECK_FALSE(config_path(filesystem).has_value());
    CHECK_FALSE(wslconfig_path(filesystem).has_value());
}

TEST_CASE("a missing config file is the defaults, not an error", "[model][config]") {
    // There is nothing a file could say that the defaults do not already say,
    // so asking the user to create one would be ceremony.
    const FakeFileSystem filesystem = machine();

    const auto config = load_config(filesystem, LR"(C:\nowhere\config.toml)");

    REQUIRE(config.has_value());
    CHECK(config->compact_trim);
}

TEST_CASE("a config file that is there is read", "[model][config]") {
    FakeFileSystem filesystem = machine();
    filesystem.add_text_file(LR"(C:\config.toml)", "[compact]\ntrim = false\n");

    const auto config = load_config(filesystem, LR"(C:\config.toml)");

    REQUIRE(config.has_value());
    CHECK_FALSE(config->compact_trim);
}

TEST_CASE("a config file that cannot be read is reported", "[model][config]") {
    FakeFileSystem filesystem = machine();
    filesystem.add_text_file(LR"(C:\config.toml)", "[compact]\n");
    filesystem.fail_queries(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});

    const auto config = load_config(filesystem, LR"(C:\config.toml)");

    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("a config file that will not parse is an error", "[model][config]") {
    FakeFileSystem filesystem = machine();
    filesystem.add_text_file(LR"(C:\config.toml)", "[compact\n");

    const auto config = load_config(filesystem, LR"(C:\config.toml)");

    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == ErrorCode::Usage);
}

TEST_CASE("the wslconfig keys are read out of the wsl2 section", "[model][config]") {
    const auto wsl = parse_wslconfig(R"(# a comment
; another
[wsl2]
memory = 8GB
defaultVhdSize = 256GB
vhdSize = 1TB
swapFile = D:\\swap.vhdx
)");

    REQUIRE(wsl.default_vhd_size == "256GB");
    REQUIRE(wsl.vhd_size == "1TB");
    REQUIRE(wsl.swap_file == R"(D:\\swap.vhdx)");
    CHECK_FALSE(wsl.empty());
}

TEST_CASE("wslconfig keys outside the wsl2 section are not read", "[model][config]") {
    // `.wslconfig` has other sections, and a key of the same name in one of
    // them is someone else's setting.
    const auto wsl = parse_wslconfig(R"([experimental]
defaultVhdSize = 8GB
)");

    CHECK(wsl.empty());
}

TEST_CASE("an empty or shapeless wslconfig reads as nothing", "[model][config]") {
    CHECK(parse_wslconfig("").empty());
    CHECK(parse_wslconfig("no sections here\n").empty());
    CHECK(parse_wslconfig("[wsl2]\nnot a pair\n").empty());
    CHECK(parse_wslconfig("[wsl2]\nmemory = 8GB\n").empty());
}

TEST_CASE("wslconfig tolerates whitespace and a missing trailing newline", "[model][config]") {
    const auto wsl = parse_wslconfig("  [wsl2]  \n\t swapFile   =   D:\\swap.vhdx");

    REQUIRE(wsl.swap_file == R"(D:\swap.vhdx)");
}

TEST_CASE("the wslconfig lives under USERPROFILE", "[model][config]") {
    const FakeFileSystem filesystem = machine();

    const auto path = wslconfig_path(filesystem);

    REQUIRE(path.has_value());
    CHECK(*path == std::filesystem::path{LR"(C:\Users\example\.wslconfig)"});
}
