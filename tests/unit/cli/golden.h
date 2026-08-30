#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace wsldisk::testing {

/// Compares rendered output against a checked-in file.
///
/// Golden files exist so a change to the output is a change someone had to look
/// at: the diff in the pull request *is* the review of the user-visible text.
///
/// Regenerating is deliberate and out-of-band. `scripts/update-golden.ps1` sets
/// `WSLDISK_UPDATE_GOLDEN=1` and runs the suite; nothing rewrites a golden file
/// by accident. An environment variable rather than the `--update-golden` flag
/// the ticket suggested, because the unit suite uses Catch2's own `main` and a
/// custom option would mean replacing it -- and Catch2 rejects flags it does not
/// know before any test runs.
class Golden {
public:
    explicit Golden(std::string name) : path_(directory() / std::move(name)) {}

    /// Asserts `actual` matches the file, or rewrites it when updating.
    void check(const std::string& actual) const {
        if (updating()) {
            std::filesystem::create_directories(path_.parent_path());
            std::ofstream out(path_, std::ios::binary);
            out << actual;
            SUCCEED("rewrote " << path_.string());
            return;
        }

        std::ifstream in(path_, std::ios::binary);
        INFO("golden file: " << path_.string()
                             << "\nrun scripts/update-golden.ps1 if the change is intended");
        REQUIRE(in.is_open());

        std::ostringstream expected;
        expected << in.rdbuf();
        CHECK(actual == expected.str());
    }

private:
    [[nodiscard]] static bool updating() {
        std::size_t length = 0;
        std::array<char, 8> value{};
        if (::getenv_s(&length, value.data(), value.size(), "WSLDISK_UPDATE_GOLDEN") != 0 || length == 0) {
            return false;
        }
        return std::string{value.data()} == "1";
    }

    /// Located from the source tree, not the build directory: the files are
    /// checked in next to the tests that use them.
    [[nodiscard]] static std::filesystem::path directory() {
        return std::filesystem::path{WSLDISK_GOLDEN_DIR};
    }

    std::filesystem::path path_;
};

}  // namespace wsldisk::testing
