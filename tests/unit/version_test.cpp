#include "version.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("the version banner names the tool and the build", "[version]") {
    const std::string banner{wsldisk::version_banner()};
    const std::string version{wsldisk::version()};
    const std::string revision{wsldisk::git_revision()};

    CHECK_FALSE(version.empty());
    CHECK_FALSE(revision.empty());
    CHECK(banner.starts_with("wsldisk "));
    CHECK(banner.find(version) != std::string::npos);
    CHECK(banner.find(revision) != std::string::npos);
}
