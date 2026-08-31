#include "render.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

#include "golden.h"

using wsldisk::Error;
using wsldisk::ErrorCode;
using wsldisk::cli::Cell;
using wsldisk::cli::Table;
using wsldisk::cli::to_human_line;
using wsldisk::cli::to_json_line;
using wsldisk::model::DiskInfo;
using wsldisk::model::Distro;
using wsldisk::testing::Golden;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

Distro ubuntu() {
    Distro distro;
    distro.name = "Ubuntu";
    distro.guid = "{4d1297e9-bac4-4da1-9867-a2ab591e9581}";
    distro.version = 2;
    distro.is_default = true;
    distro.flavor = "ubuntu";
    distro.os_version = "24.04";
    distro.vhdx_path = LR"(C:\wsl\Ubuntu\ext4.vhdx)";
    return distro;
}

DiskInfo measured() {
    DiskInfo info;
    info.virtual_size = 1024 * gigabyte;
    info.file_size = 14 * gigabyte;
    info.size_on_disk = 14 * gigabyte;
    info.allocated_bytes = 14 * gigabyte;
    info.is_sparse = true;
    info.guest_used = 8 * gigabyte;
    info.guest_free = 900 * gigabyte;
    return info;
}

/// The table `list` will print, so the golden file is the real output shape
/// rather than a shape invented for the test.
std::string render_list(const std::vector<std::pair<Distro, DiskInfo>>& rows) {
    Table table{{"NAME", "VERSION", "SIZE ON DISK", "GUEST USED", "RECLAIMABLE", "PATH"}};
    for (const auto& [distro, info] : rows) {
        table.add_row({Cell{.text = distro.name}, Cell{.text = std::to_string(distro.version)},
                       Cell{.bytes = info.size_on_disk}, Cell{.bytes = info.guest_used},
                       Cell{.bytes = info.reclaimable()}, Cell{.text = distro.vhdx_path.string()}});
    }
    std::ostringstream out;
    table.render(out);
    return out.str();
}

}  // namespace

TEST_CASE("the table renders a fully measured distribution", "[cli][render]") {
    Golden{"list-table.txt"}.check(render_list({{ubuntu(), measured()}}));
}

TEST_CASE("the table shows unknown fields as a dash", "[cli][render]") {
    // A running distribution holds its disk open, so this is the ordinary case
    // rather than an edge one.
    DiskInfo partial;
    partial.size_on_disk = 14 * gigabyte;

    Golden{"list-table-unknown.txt"}.check(render_list({{ubuntu(), partial}}));
}

TEST_CASE("the table widens columns to fit the content", "[cli][render]") {
    Distro long_name = ubuntu();
    long_name.name = "Ubuntu-24.04-LTS-with-a-long-name";
    long_name.vhdx_path = LR"(D:\some\deeply\nested\location\for\disks\ext4.vhdx)";

    Golden{"list-table-wide.txt"}.check(render_list({{ubuntu(), measured()}, {long_name, measured()}}));
}

TEST_CASE("an empty table still prints its headers", "[cli][render]") {
    // `list` on a machine with no distributions has to say *something*, and a
    // header row with nothing under it says it.
    Golden{"list-table-empty.txt"}.check(render_list({}));
}

TEST_CASE("a row with too few cells is padded rather than throwing", "[cli][render]") {
    // A half-drawn table is more useful than a crash while reporting something
    // else, which is when this would happen.
    Table table{{"A", "B", "C"}};
    table.add_row({Cell{.text = "one"}});

    std::ostringstream out;
    table.render(out);

    CHECK(out.str() == "A    B  C\none  -  -\n");
}

TEST_CASE("a boolean cell reads as yes or no", "[cli][render]") {
    Table table{{"SPARSE"}};
    table.add_row({Cell{.flag = true}});
    table.add_row({Cell{.flag = false}});

    std::ostringstream out;
    table.render(out);

    CHECK(out.str() == "SPARSE\nyes\nno\n");
}

TEST_CASE("the json line carries every measured field", "[cli][render]") {
    Golden{"list-json.txt"}.check(to_json_line(ubuntu(), measured()) + "\n");
}

TEST_CASE("the json line omits fields that are unknown", "[cli][render]") {
    // Left out rather than written as null: a consumer checking for the key
    // gets the same answer either way, and the line stays shorter.
    DiskInfo partial;
    partial.size_on_disk = 14 * gigabyte;

    const auto object = nlohmann::json::parse(to_json_line(ubuntu(), partial));

    CHECK(object.contains("size_on_disk"));
    CHECK_FALSE(object.contains("guest_used"));
    CHECK_FALSE(object.contains("reclaimable"));
    CHECK_FALSE(object.contains("virtual_size"));
}

TEST_CASE("json sizes are integers, not formatted strings", "[cli][render]") {
    // A consumer that wants "14.0 GiB" can format it; one that wants to compare
    // or sum cannot un-format it.
    const auto object = nlohmann::json::parse(to_json_line(ubuntu(), measured()));

    CHECK(object["size_on_disk"].is_number_unsigned());
    CHECK(object["size_on_disk"].get<std::uint64_t>() == 14 * gigabyte);
    CHECK(object["virtual_size"].is_number_unsigned());
}

TEST_CASE("the json line is a single line", "[cli][render]") {
    // `--all` prints one object per line, so an embedded newline would break
    // every consumer reading it line by line.
    const std::string line = to_json_line(ubuntu(), measured());

    CHECK(line.find('\n') == std::string::npos);
}

TEST_CASE("notes appear in the json line when there are any", "[cli][render]") {
    DiskInfo info = measured();
    info.notes.push_back("Ubuntu is not running; pass --probe to read guest usage");

    const auto object = nlohmann::json::parse(to_json_line(ubuntu(), info));

    REQUIRE(object.contains("notes"));
    CHECK(object["notes"].size() == 1);
}

TEST_CASE("a distribution with no flavor omits it", "[cli][render]") {
    Distro legacy = ubuntu();
    legacy.flavor.clear();
    legacy.os_version.clear();

    const auto object = nlohmann::json::parse(to_json_line(legacy, measured()));

    CHECK_FALSE(object.contains("flavor"));
    CHECK_FALSE(object.contains("os_version"));
}

TEST_CASE("an error renders as json with its stable token", "[cli][render]") {
    const Error error{ErrorCode::DistroBusy, "Ubuntu is running", "run `wsl --shutdown` first"};

    const auto object = nlohmann::json::parse(to_json_line(error));

    CHECK(object["error"] == "distro-busy");
    CHECK(object["exit_code"] == 11);
    CHECK(object["message"] == "Ubuntu is running");
    CHECK(object["remedy"] == "run `wsl --shutdown` first");
}

TEST_CASE("an error with no remedy omits it", "[cli][render]") {
    const Error error{ErrorCode::Generic, "something went wrong"};

    const auto object = nlohmann::json::parse(to_json_line(error));

    CHECK_FALSE(object.contains("remedy"));
}

TEST_CASE("an error renders for a human as message and remedy", "[cli][render]") {
    const Error error{ErrorCode::DistroBusy, "Ubuntu is running", "run `wsl --shutdown` first"};

    CHECK(to_human_line(error) == "Ubuntu is running -- run `wsl --shutdown` first");
}

TEST_CASE("the json line emits a non-ASCII path as UTF-8", "[cli][render]") {
    // `vhdx_path` used to go through `path::string()` while `base_path`, right
    // beside it, went through `to_utf8`. On a 932 machine that put Shift-JIS
    // bytes into the object and `dump()` threw for invalid UTF-8, so `list
    // --json` failed with a JSON-library message instead of listing anything.
    Distro distro = ubuntu();
    distro.name = "Übuntu";
    distro.vhdx_path = LR"(D:\wsl\Übuntu\ext4.vhdx)";

    const std::string line = to_json_line(distro, DiskInfo{});

    // Parses at all, which is the half that used to throw.
    const nlohmann::json object = nlohmann::json::parse(line);
    CHECK(object.at("name") == "Übuntu");
    CHECK(object.at("vhdx_path") == R"(D:\wsl\Übuntu\ext4.vhdx)");
}
