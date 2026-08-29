#include "app.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

#include "errors.h"
#include "version.h"

namespace {

struct Invocation {
    int exit_code = 0;
    std::string out;
    std::string err;
};

Invocation invoke(const std::vector<std::string>& args) {
    std::ostringstream out;
    std::ostringstream err;
    const int exit_code = wsldisk::cli::run(args, out, err);
    return {.exit_code = exit_code, .out = out.str(), .err = err.str()};
}

}  // namespace

TEST_CASE("the version flag prints the banner and succeeds", "[cli]") {
    for (const char* flag : {"--version", "-V"}) {
        const auto result = invoke({flag});
        CHECK(result.exit_code == wsldisk::exit_code_success);
        CHECK(result.out.find(wsldisk::version_banner()) != std::string::npos);
        CHECK(result.err.empty());
    }
}

TEST_CASE("the help flag prints usage on stdout and succeeds", "[cli]") {
    const auto result = invoke({"--help"});
    CHECK(result.exit_code == wsldisk::exit_code_success);
    CHECK(result.out.find("wsldisk") != std::string::npos);
    CHECK(result.err.empty());
}

TEST_CASE("no arguments prints usage rather than doing something surprising", "[cli]") {
    const auto result = invoke({});
    CHECK(result.exit_code == wsldisk::exit_code_success);
    CHECK(result.out.find("wsldisk [OPTIONS]") != std::string::npos);
}

TEST_CASE("an unknown flag is a usage error on stderr", "[cli]") {
    const auto result = invoke({"--nonsense"});
    CHECK(result.exit_code == wsldisk::exit_code_for(wsldisk::ErrorCode::Usage));
    CHECK(result.err.find("error:") != std::string::npos);
    CHECK(result.out.empty());
}

TEST_CASE("an unknown subcommand is a usage error", "[cli]") {
    // Commands land in M1; until then anything positional is rejected rather
    // than silently ignored.
    const auto result = invoke({"compact", "Ubuntu"});
    CHECK(result.exit_code == wsldisk::exit_code_for(wsldisk::ErrorCode::Usage));
    CHECK(result.err.find("error:") != std::string::npos);
}
