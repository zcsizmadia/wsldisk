// Contract tests for the config file against the real filesystem: real reads,
// real writes, and a real directory created two levels deep.
//
// The unit tests drive `load_config` and `render_config` through the in-memory
// fake, which is right, but it leaves the part that touches Win32 untested --
// and reading a whole file through `ReadFile` in a loop, and creating parent
// directories one at a time, are exactly the places an off-by-one lives.

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "errors.h"
#include "model/config.h"
#include "platform/filesystem.h"

using wsldisk::model::Config;
using wsldisk::model::load_config;
using wsldisk::model::parse_config;
using wsldisk::model::render_config;
using wsldisk::platform::Win32FileSystem;

namespace {

/// A directory under %TEMP% that removes itself and everything in it.
class TempTree {
public:
    TempTree()
        : path_(std::filesystem::temp_directory_path() /
                ("wsldisk-config-" + std::to_string(::GetCurrentProcessId()) + "-" +
                 std::to_string(++counter))) {
        std::filesystem::create_directories(path_);
    }

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    static int counter;
    std::filesystem::path path_;
};

int TempTree::counter = 0;

}  // namespace

TEST_CASE("a config written to disk reads back as the same settings", "[contract][config]") {
    const TempTree tree;
    const std::filesystem::path file = tree.path() / "wsldisk" / "config.toml";

    Config config;
    config.scan_dirs = {R"(D:\WSL)", R"(E:\a path with spaces)"};
    config.compact_trim = false;
    config.compact_restart = true;
    config.unlock_timeout_seconds = 30;

    Win32FileSystem filesystem;
    REQUIRE(filesystem.create_directories(file.parent_path()).has_value());
    REQUIRE(filesystem.write_text_file(file, render_config(config)).has_value());

    const auto loaded = load_config(filesystem, file);

    REQUIRE(loaded.has_value());
    CHECK(loaded->scan_dirs == config.scan_dirs);
    CHECK(loaded->compact_trim == config.compact_trim);
    CHECK(loaded->compact_restart == config.compact_restart);
    CHECK(loaded->unlock_timeout_seconds == config.unlock_timeout_seconds);
}

TEST_CASE("create_directories makes every level and is happy to repeat", "[contract][config]") {
    const TempTree tree;
    const std::filesystem::path deep = tree.path() / "one" / "two" / "three";

    Win32FileSystem filesystem;
    REQUIRE(filesystem.create_directories(deep).has_value());
    CHECK(std::filesystem::is_directory(deep));

    // `mkdir -p`: already there is the ordinary case, not a failure.
    CHECK(filesystem.create_directories(deep).has_value());
}

TEST_CASE("a file larger than one read buffer comes back whole", "[contract][config]") {
    // The read loop runs until ReadFile reports zero bytes; a file that fits in
    // one buffer would never test the second pass.
    const TempTree tree;
    const std::filesystem::path file = tree.path() / "big.toml";

    Config config;
    for (int index = 0; index < 500; ++index) {
        config.scan_dirs.push_back(R"(D:\a reasonably long directory name )" + std::to_string(index));
    }

    Win32FileSystem filesystem;
    const std::string rendered = render_config(config);
    REQUIRE(rendered.size() > 4096);
    REQUIRE(filesystem.write_text_file(file, rendered).has_value());

    const auto read = filesystem.read_text_file(file);
    REQUIRE(read.has_value());
    CHECK(*read == rendered);

    const auto reparsed = parse_config(*read);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->scan_dirs.size() == config.scan_dirs.size());
}

TEST_CASE("writing a config replaces whatever was there", "[contract][config]") {
    const TempTree tree;
    const std::filesystem::path file = tree.path() / "config.toml";

    Win32FileSystem filesystem;
    REQUIRE(filesystem.write_text_file(file, render_config(Config{})).has_value());

    Config shorter;
    shorter.unlock_timeout_seconds = 1;
    REQUIRE(filesystem.write_text_file(file, render_config(shorter)).has_value());

    // Not appended to, and no tail of the longer previous file left behind.
    const auto read = filesystem.read_text_file(file);
    REQUIRE(read.has_value());
    CHECK(*read == render_config(shorter));
}

TEST_CASE("reading a file that is not there is an error, not empty", "[contract][config]") {
    const TempTree tree;

    const Win32FileSystem filesystem;
    const auto read = filesystem.read_text_file(tree.path() / "missing.toml");

    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().message.find("open") != std::string::npos);
}

TEST_CASE("a missing config file is the defaults against the real filesystem", "[contract][config]") {
    const TempTree tree;

    const Win32FileSystem filesystem;
    const auto config = load_config(filesystem, tree.path() / "nothing-here.toml");

    REQUIRE(config.has_value());
    CHECK(config->compact_trim);
    CHECK(config->unlock_timeout_seconds == 90);
}

TEST_CASE("the config path resolves against the real environment", "[contract][config]") {
    const Win32FileSystem filesystem;

    const auto path = wsldisk::model::config_path(filesystem);

    REQUIRE(path.has_value());
    // %APPDATA% really expanded, and the file named under a wsldisk directory
    // of our own rather than loose in someone else's.
    CHECK(path->wstring().find(L'%') == std::wstring::npos);
    CHECK(path->is_absolute());
    CHECK(path->filename() == "config.toml");
    CHECK(path->parent_path().filename() == "wsldisk");
}

TEST_CASE("the wslconfig path resolves against the real environment", "[contract][config]") {
    const Win32FileSystem filesystem;

    const auto path = wsldisk::model::wslconfig_path(filesystem);

    REQUIRE(path.has_value());
    CHECK(path->wstring().find(L'%') == std::wstring::npos);
    CHECK(path->filename() == ".wslconfig");
}

TEST_CASE("writing into a directory that is not there fails rather than silently", "[contract][config]") {
    const TempTree tree;

    Win32FileSystem filesystem;
    const auto status = filesystem.write_text_file(tree.path() / "absent" / "config.toml", "x = 1\n");

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().message.find("create") != std::string::npos);
}
