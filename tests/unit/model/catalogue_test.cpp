#include "model/catalogue.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

#include "errors.h"

using wsldisk::ErrorCode;
using wsldisk::model::cache_catalogue;
using wsldisk::model::CacheEntry;
using wsldisk::model::parse_catalogue;
using wsldisk::model::path_contains;

TEST_CASE("a catalogue entry parses into its four fields", "[model][catalogue]") {
    const auto parsed = parse_catalogue(R"(
[[cache]]
path = "/var/cache/apt/archives"
label = "apt package cache"
safe = true
note = "`apt clean` empties it"
)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
    CHECK((*parsed)[0].path == "/var/cache/apt/archives");
    CHECK((*parsed)[0].label == "apt package cache");
    CHECK((*parsed)[0].safe);
    CHECK((*parsed)[0].note == "`apt clean` empties it");
    CHECK_FALSE((*parsed)[0].is_per_user());
}

TEST_CASE("a note is optional", "[model][catalogue]") {
    const auto parsed = parse_catalogue(R"(
[[cache]]
path = "/tmp"
label = "temporary files"
safe = true
)");

    REQUIRE(parsed.has_value());
    CHECK((*parsed)[0].note.empty());
}

TEST_CASE("a tilde path is per-user", "[model][catalogue]") {
    // One entry covers every account on the machine, which is why the expansion
    // happens at measurement time rather than here.
    const auto parsed = parse_catalogue(R"(
[[cache]]
path = "~/.cache/pip"
label = "pip cache"
safe = true
)");

    REQUIRE(parsed.has_value());
    CHECK((*parsed)[0].is_per_user());
}

TEST_CASE("a catalogue entry missing a required field says which", "[model][catalogue]") {
    // The file is hand-edited, so "it did not work" is not a useful thing to say
    // about a typo.
    const auto missing_path = parse_catalogue(R"(
[[cache]]
label = "something"
safe = true
)");
    REQUIRE_FALSE(missing_path.has_value());
    CHECK(missing_path.error().message.find("path") != std::string::npos);

    const auto missing_label = parse_catalogue(R"(
[[cache]]
path = "/tmp"
safe = true
)");
    REQUIRE_FALSE(missing_label.has_value());
    CHECK(missing_label.error().message.find("label") != std::string::npos);

    const auto missing_safe = parse_catalogue(R"(
[[cache]]
path = "/tmp"
label = "temporary files"
)");
    REQUIRE_FALSE(missing_safe.has_value());
    CHECK(missing_safe.error().message.find("safe") != std::string::npos);
}

TEST_CASE("a catalogue entry names which entry was wrong", "[model][catalogue]") {
    // Counting from one, because the file is read by a person.
    const auto parsed = parse_catalogue(R"(
[[cache]]
path = "/tmp"
label = "temporary files"
safe = true

[[cache]]
path = "/var/tmp"
safe = true
)");

    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().message.find("entry 2") != std::string::npos);
}

TEST_CASE("a relative catalogue path is refused", "[model][catalogue]") {
    // It would be measured against whatever the guest's working directory
    // happened to be, which is not a thing anyone meant to write.
    const auto parsed = parse_catalogue(R"(
[[cache]]
path = "var/cache"
label = "somewhere"
safe = true
)");

    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().message.find("relative") != std::string::npos);
}

TEST_CASE("a trailing slash is refused", "[model][catalogue]") {
    // It would break the containment check, which looks for the separator after
    // a prefix.
    const auto parsed = parse_catalogue(R"(
[[cache]]
path = "/var/log/"
label = "logs"
safe = true
)");

    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().message.find("slash") != std::string::npos);
}

TEST_CASE("malformed catalogue TOML is reported rather than crashing", "[model][catalogue]") {
    const auto parsed = parse_catalogue("[[cache]\npath =");

    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ErrorCode::Usage);
}

TEST_CASE("a catalogue with no entries is refused", "[model][catalogue]") {
    const auto parsed = parse_catalogue("# nothing here\n");

    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().message.find("no [[cache]] entries") != std::string::npos);
}

TEST_CASE("a cache that is not a table is refused", "[model][catalogue]") {
    const auto parsed = parse_catalogue("cache = [1, 2]\n");

    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().message.find("not a table") != std::string::npos);
}

TEST_CASE("path_contains knows a directory from a prefix", "[model][catalogue]") {
    CHECK(path_contains("/var/log", "/var/log/journal"));
    // Not a prefix match: `/var/log` does not contain `/var/logbook`, and
    // treating it as if it did would silently stop counting a real directory.
    CHECK_FALSE(path_contains("/var/log", "/var/logbook"));
    CHECK_FALSE(path_contains("/var/log", "/var/log"));
    CHECK_FALSE(path_contains("/var/log/journal", "/var/log"));
    CHECK_FALSE(path_contains("/tmp", "/var/tmp"));
}

TEST_CASE("the embedded catalogue parses and is not empty", "[model][catalogue]") {
    // The file is compiled in, so a bad edit is a broken build rather than a
    // broken machine -- but only if something reads it.
    const std::vector<CacheEntry>& catalogue = cache_catalogue();

    REQUIRE_FALSE(catalogue.empty());
    for (const CacheEntry& entry : catalogue) {
        INFO(entry.path);
        CHECK_FALSE(entry.label.empty());
        CHECK((entry.path.starts_with('/') || entry.path.starts_with("~/")));
        CHECK_FALSE(entry.path.ends_with('/'));
    }
}

TEST_CASE("no two catalogue entries name the same path", "[model][catalogue]") {
    // A duplicate would be measured twice and reported twice, and the totals
    // would quietly double-count it.
    std::set<std::string> seen;
    for (const CacheEntry& entry : cache_catalogue()) {
        INFO(entry.path);
        CHECK(seen.insert(entry.path).second);
    }
}

TEST_CASE("the catalogue knows the package managers people actually use", "[model][catalogue]") {
    // Not an exhaustive list -- a guard against someone emptying the file and
    // every usage test still passing against nothing.
    const auto has = [](std::string_view path) {
        return std::ranges::any_of(cache_catalogue(),
                                   [path](const CacheEntry& entry) { return entry.path == path; });
    };
    CHECK(has("/var/cache/apt/archives"));
    CHECK(has("/var/lib/docker"));
    CHECK(has("~/.cache"));
}

TEST_CASE("docker storage is not marked clearable", "[model][catalogue]") {
    // It holds images and volumes the user built. wsldisk cannot tell whether
    // they matter, so it declines to decide -- `docker system prune` is theirs
    // to run.
    for (const CacheEntry& entry : cache_catalogue()) {
        if (entry.path == "/var/lib/docker") {
            CHECK_FALSE(entry.safe);
            CHECK_FALSE(entry.note.empty());
        }
    }
}
