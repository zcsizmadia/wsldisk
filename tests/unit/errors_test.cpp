#include "errors.h"

#include <catch2/catch_test_macros.hpp>

using wsldisk::Error;
using wsldisk::ErrorCode;

TEST_CASE("exit codes match the documented contract", "[errors]") {
    // These numbers are in PLAN.md §4.9 and scripts depend on them.
    CHECK(wsldisk::exit_code_success == 0);
    CHECK(wsldisk::exit_code_for(ErrorCode::Generic) == 1);
    CHECK(wsldisk::exit_code_for(ErrorCode::Usage) == 2);
    CHECK(wsldisk::exit_code_for(ErrorCode::Preflight) == 3);
    CHECK(wsldisk::exit_code_for(ErrorCode::NeedsElevation) == 4);
    CHECK(wsldisk::exit_code_for(ErrorCode::Partial) == 5);
    CHECK(wsldisk::exit_code_for(ErrorCode::IntegrityCheckFailed) == 6);
    CHECK(wsldisk::exit_code_for(ErrorCode::DistroNotFound) == 10);
    CHECK(wsldisk::exit_code_for(ErrorCode::DistroBusy) == 11);
}

TEST_CASE("every error code has a stable json token", "[errors]") {
    CHECK(wsldisk::error_code_name(ErrorCode::Generic) == "generic");
    CHECK(wsldisk::error_code_name(ErrorCode::Usage) == "usage");
    CHECK(wsldisk::error_code_name(ErrorCode::Preflight) == "preflight");
    CHECK(wsldisk::error_code_name(ErrorCode::NeedsElevation) == "needs-elevation");
    CHECK(wsldisk::error_code_name(ErrorCode::Partial) == "partial");
    CHECK(wsldisk::error_code_name(ErrorCode::IntegrityCheckFailed) == "integrity-check-failed");
    CHECK(wsldisk::error_code_name(ErrorCode::DistroNotFound) == "distro-not-found");
    CHECK(wsldisk::error_code_name(ErrorCode::DistroBusy) == "distro-busy");
}

TEST_CASE("an error renders its remedy when it has one", "[errors]") {
    const Error with_remedy{ErrorCode::DistroBusy, "Ubuntu is running", "run wsl --terminate Ubuntu"};
    CHECK(with_remedy.to_string() == "Ubuntu is running -- run wsl --terminate Ubuntu");

    const Error without_remedy{ErrorCode::Generic, "something broke"};
    CHECK(without_remedy.to_string() == "something broke");
}

TEST_CASE("fail() builds an unexpected carrying the error", "[errors]") {
    const wsldisk::Result<int> result = wsldisk::fail(ErrorCode::Usage, "bad flag", "see --help");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Usage);
    CHECK(result.error().message == "bad flag");
    CHECK(result.error().remedy == "see --help");
}

TEST_CASE("a status can carry a failure with no value", "[errors]") {
    const wsldisk::Status status = wsldisk::fail(ErrorCode::Preflight, "not enough space");
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().remedy.empty());

    const wsldisk::Status ok{};
    CHECK(ok.has_value());
}

TEST_CASE("an unrecognised code falls back to the generic name", "[errors]") {
    // Not reachable from the enum, but reachable from a cast or from a number
    // parsed out of older or newer --json output.
    CHECK(wsldisk::error_code_name(static_cast<ErrorCode>(9999)) == "generic");
}
