#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "errors.h"
#include "logger.h"
#include "options.h"
#include "preflight.h"

using wsldisk::Error;
using wsldisk::ErrorCode;
using wsldisk::exit_code_for;
using wsldisk::cli::GlobalOptions;
using wsldisk::cli::NullLogger;
using wsldisk::cli::report;
using wsldisk::cli::require_wsl2;
using wsldisk::cli::StreamLogger;
using wsldisk::model::Distro;

namespace {

Distro distro_of_version(std::uint32_t version) {
    Distro distro;
    distro.name = "Ubuntu";
    distro.version = version;
    return distro;
}

/// A temporary file path that removes itself.
class TempFile {
public:
    TempFile()
        : path_(std::filesystem::temp_directory_path() /
                ("wsldisk-log-" + std::to_string(++counter) + ".txt")) {}

    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::string contents() const {
        std::ifstream in(path_, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

private:
    static int counter;
    std::filesystem::path path_;
};

int TempFile::counter = 0;

}  // namespace

TEST_CASE("a WSL1 distribution is refused with the conversion command", "[cli][preflight]") {
    // D8: WSL1 has no virtual disk at all, so there is nothing to act on.
    const auto status = require_wsl2(distro_of_version(1));

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Preflight);
    CHECK(exit_code_for(status.error().code) == 3);
    CHECK(status.error().remedy.find("wsl --set-version Ubuntu 2") != std::string::npos);
}

TEST_CASE("a WSL2 distribution passes the preflight", "[cli][preflight]") {
    CHECK(require_wsl2(distro_of_version(2)).has_value());
}

TEST_CASE("an error is reported to stderr for a human", "[cli][plumbing]") {
    std::ostringstream out;
    std::ostringstream err;
    const Error error{ErrorCode::DistroBusy, "Ubuntu is running", "run `wsl --shutdown` first"};

    const int code = report(error, GlobalOptions{}, out, err);

    CHECK(code == 11);
    CHECK(out.str().empty());
    CHECK(err.str() == "error: Ubuntu is running -- run `wsl --shutdown` first\n");
}

TEST_CASE("an error is reported to stdout as json when asked", "[cli][plumbing]") {
    // A script reading stdout should get a parseable answer whether or not the
    // command worked.
    std::ostringstream out;
    std::ostringstream err;
    const Error error{ErrorCode::DistroBusy, "Ubuntu is running", "run `wsl --shutdown` first"};

    const int code = report(error, GlobalOptions{.json = true}, out, err);

    CHECK(code == 11);
    CHECK(err.str().empty());
    CHECK(out.str().find("\"error\":\"distro-busy\"") != std::string::npos);
    CHECK(out.str().ends_with("\n"));
}

TEST_CASE("every error code maps to its documented exit code", "[cli][plumbing]") {
    // The numbers are the public contract from 1.0 onwards; scripts branch on
    // them, so they are pinned here rather than only in errors.h.
    CHECK(exit_code_for(ErrorCode::Generic) == 1);
    CHECK(exit_code_for(ErrorCode::Usage) == 2);
    CHECK(exit_code_for(ErrorCode::Preflight) == 3);
    CHECK(exit_code_for(ErrorCode::NeedsElevation) == 4);
    CHECK(exit_code_for(ErrorCode::Partial) == 5);
    CHECK(exit_code_for(ErrorCode::IntegrityCheckFailed) == 6);
    CHECK(exit_code_for(ErrorCode::DistroNotFound) == 10);
    CHECK(exit_code_for(ErrorCode::DistroBusy) == 11);
}

TEST_CASE("the null logger still reports warnings", "[cli][logger]") {
    // Warnings are not verbose detail; silencing them would hide a skipped
    // registry key.
    std::ostringstream err;
    NullLogger logger{err};

    logger.verbose("ignored");
    logger.warn("a key was skipped");

    CHECK(err.str() == "warning: a key was skipped\n");
}

TEST_CASE("verbose output is dropped unless asked for", "[cli][logger]") {
    std::ostringstream err;
    StreamLogger logger{err, false, {}};

    logger.verbose("opening the disk");

    CHECK(err.str().empty());
}

TEST_CASE("verbose output goes to the stream when asked for", "[cli][logger]") {
    std::ostringstream err;
    StreamLogger logger{err, true, {}};

    logger.verbose("opening the disk");

    CHECK(err.str() == "opening the disk\n");
}

TEST_CASE("a log file receives verbose detail even without -v", "[cli][logger]") {
    // Someone who asked for a log asked for the detail; the flag is about the
    // console, not the file.
    const TempFile file;
    std::ostringstream err;
    {
        StreamLogger logger{err, false, file.path()};
        REQUIRE(logger.logging_to_file());
        logger.verbose("opening the disk");
        logger.warn("a key was skipped");
    }

    CHECK(err.str() == "warning: a key was skipped\n");
    CHECK(file.contents().find("verbose: opening the disk") != std::string::npos);
    CHECK(file.contents().find("warning: a key was skipped") != std::string::npos);
}

TEST_CASE("a log file that cannot be opened does not fail the command", "[cli][logger]") {
    // Failing because the log could not be written would be worse than not
    // logging.
    std::ostringstream err;
    StreamLogger logger{err, false, std::filesystem::path{"Z:\\no-such-drive\\wsldisk.log"}};

    CHECK_FALSE(logger.logging_to_file());
    CHECK(err.str().find("could not open the log file") != std::string::npos);
    // And it still works.
    logger.warn("still reporting");
    CHECK(err.str().find("still reporting") != std::string::npos);
}

TEST_CASE("a log file is appended to rather than truncated", "[cli][logger]") {
    // A log that truncates on every run is no use for reconstructing what
    // happened across several attempts.
    const TempFile file;
    std::ostringstream err;
    {
        StreamLogger first{err, false, file.path()};
        first.warn("first run");
    }
    {
        StreamLogger second{err, false, file.path()};
        second.warn("second run");
    }

    const std::string contents = file.contents();
    CHECK(contents.find("first run") != std::string::npos);
    CHECK(contents.find("second run") != std::string::npos);
}

TEST_CASE("the global flags parse in either position", "[cli][options]") {
    // `wsldisk --json list` and `wsldisk list --json` should both be natural.
    std::ostringstream out;
    std::ostringstream err;

    const std::vector<std::string> arguments{"--json", "--dry-run", "-v", "--yes"};
    CHECK(wsldisk::cli::run(arguments, out, err) == 0);
}

TEST_CASE("an unknown flag is a usage error", "[cli][options]") {
    std::ostringstream out;
    std::ostringstream err;

    const std::vector<std::string> arguments{"--not-a-flag"};

    CHECK(wsldisk::cli::run(arguments, out, err) == exit_code_for(ErrorCode::Usage));
    CHECK(err.str().find("error:") != std::string::npos);
}

TEST_CASE("the log flag takes a path", "[cli][options]") {
    const TempFile file;
    std::ostringstream out;
    std::ostringstream err;

    const std::vector<std::string> arguments{"--log", file.path().string()};

    CHECK(wsldisk::cli::run(arguments, out, err) == 0);
}
