#include "lookup.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

#include "errors.h"
#include "fake_registry.h"
#include "logger.h"
#include "lxss_hives.h"

using wsldisk::ErrorCode;
using wsldisk::cli::distro_not_found;
using wsldisk::cli::find_distro;
using wsldisk::cli::nearest_names;
using wsldisk::cli::NullLogger;
using wsldisk::testing::FakeRegistry;
namespace hives = wsldisk::testing::hives;

TEST_CASE("suggestions are ordered closest first", "[cli][lookup]") {
    const std::vector<std::string> registered{"Ubuntu24", "Ubuntu", "Alpine"};

    const auto names = nearest_names("Ubunt", registered);

    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Ubuntu");
    CHECK(names[1] == "Ubuntu24");
}

TEST_CASE("a name that only shares a prefix is not suggested", "[cli][lookup]") {
    // `Ubuntu-20.04` is seven edits from `Ubunt`. Offering it would be the tool
    // guessing rather than correcting.
    const std::vector<std::string> registered{"Ubuntu-20.04"};

    CHECK(nearest_names("Ubunt", registered).empty());
}

TEST_CASE("suggestions ignore case", "[cli][lookup]") {
    const std::vector<std::string> registered{"Ubuntu"};

    CHECK(nearest_names("UBUNTU", registered) == std::vector<std::string>{"Ubuntu"});
}

TEST_CASE("nothing is suggested when nothing is close", "[cli][lookup]") {
    const std::vector<std::string> registered{"Ubuntu"};

    CHECK(nearest_names("zzzzzzzzzzzz", registered).empty());
}

TEST_CASE("suggestions from an empty registry are empty", "[cli][lookup]") {
    CHECK(nearest_names("Ubuntu", {}).empty());
}

TEST_CASE("the not-found error names the closest matches", "[cli][lookup]") {
    const auto error = distro_not_found("Ubunt", {"Ubuntu", "Alpine"});

    CHECK(error.code == ErrorCode::DistroNotFound);
    CHECK(error.message == "no distribution named Ubunt");
    CHECK(error.remedy.find("did you mean Ubuntu?") != std::string::npos);
}

TEST_CASE("the not-found error falls back to `wsldisk list`", "[cli][lookup]") {
    // Nothing close enough to suggest. A dead end still gets a next step.
    const auto error = distro_not_found("zzzzzzzzzzzz", {"Ubuntu"});

    CHECK(error.remedy == "run `wsldisk list` to see what is registered");
}

TEST_CASE("find_distro finds a registered distribution by name", "[cli][lookup]") {
    auto registry = hives::measured();
    std::ostringstream errors;
    NullLogger logger{errors};

    const auto distro = find_distro(registry, "Ubuntu", logger);

    REQUIRE(distro.has_value());
    CHECK(distro->name == "Ubuntu");
}

TEST_CASE("find_distro matches a name case-insensitively", "[cli][lookup]") {
    // `wsl.exe` matches names that way, and a tool beside it that did not would
    // be its own kind of surprise.
    auto registry = hives::measured();
    std::ostringstream errors;
    NullLogger logger{errors};

    const auto distro = find_distro(registry, "UBUNTU", logger);

    REQUIRE(distro.has_value());
    CHECK(distro->name == "Ubuntu");
}

TEST_CASE("find_distro reports a name that is not registered", "[cli][lookup]") {
    auto registry = hives::measured();
    std::ostringstream errors;
    NullLogger logger{errors};

    const auto distro = find_distro(registry, "Ubunt", logger);

    REQUIRE_FALSE(distro.has_value());
    CHECK(distro.error().code == ErrorCode::DistroNotFound);
    CHECK(distro.error().remedy.find("did you mean Ubuntu?") != std::string::npos);
}

TEST_CASE("find_distro reports a registry it cannot read", "[cli][lookup]") {
    FakeRegistry registry;
    registry.fail_with(wsldisk::Error{ErrorCode::NeedsElevation, "cannot read", "run as the owning user"});
    std::ostringstream errors;
    NullLogger logger{errors};

    const auto distro = find_distro(registry, "Ubuntu", logger);

    REQUIRE_FALSE(distro.has_value());
    CHECK(distro.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("find_distro passes on the warnings enumeration produced", "[cli][lookup]") {
    // A key that had to be skipped is worth saying even when the distribution
    // being looked for was found.
    auto registry = hives::everything();
    std::ostringstream errors;
    NullLogger logger{errors};

    const auto distro = find_distro(registry, "Ubuntu", logger);

    REQUIRE(distro.has_value());
    CHECK(errors.str().find("DistributionName") != std::string::npos);
}
