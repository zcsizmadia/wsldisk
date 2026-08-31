#include "app.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
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

namespace {

/// A streambuf that fails every write, so the top-level handler can be exercised.
class FailingBuffer : public std::streambuf {
protected:
    int_type overflow(int_type) override { throw std::runtime_error("stream is broken"); }
};

}  // namespace

TEST_CASE("main_entry drops argv[0] and runs the rest", "[cli]") {
    std::ostringstream out;
    std::ostringstream err;
    std::array<wchar_t*, 2> argv{const_cast<wchar_t*>(L"wsldisk"), const_cast<wchar_t*>(L"--version")};

    const int exit_code = wsldisk::cli::main_entry(static_cast<int>(argv.size()), argv.data(), out, err);

    CHECK(exit_code == wsldisk::exit_code_success);
    CHECK(out.str().find(wsldisk::version_banner()) != std::string::npos);
}

TEST_CASE("main_entry tolerates an empty argv", "[cli]") {
    // argc of zero is legal, if unusual; indexing argv[0] would be undefined.
    std::ostringstream out;
    std::ostringstream err;

    const int exit_code = wsldisk::cli::main_entry(0, nullptr, out, err);

    CHECK(exit_code == wsldisk::exit_code_success);
    CHECK(out.str().find("wsldisk [OPTIONS]") != std::string::npos);
}

TEST_CASE("main_entry reports an exception instead of letting it escape", "[cli]") {
    FailingBuffer broken;
    std::ostream failing_out{&broken};
    // Without this an ostream swallows a streambuf failure into badbit.
    failing_out.exceptions(std::ios::badbit);
    std::ostringstream err;
    std::array<wchar_t*, 2> argv{const_cast<wchar_t*>(L"wsldisk"), const_cast<wchar_t*>(L"--version")};

    // Writing the banner throws; main_entry is noexcept, so it must catch it.
    const int exit_code =
        wsldisk::cli::main_entry(static_cast<int>(argv.size()), argv.data(), failing_out, err);

    CHECK(exit_code == wsldisk::exit_code_for(wsldisk::ErrorCode::Generic));
    CHECK(err.str().find("stream is broken") != std::string::npos);
}

TEST_CASE("main_entry survives an error stream that is also broken", "[cli]") {
    // Nothing can be reported, but terminating would be strictly worse than
    // returning the exit code -- and main_entry is noexcept, so it must not throw.
    FailingBuffer broken_out;
    FailingBuffer broken_err;
    std::ostream failing_out{&broken_out};
    std::ostream failing_err{&broken_err};
    failing_out.exceptions(std::ios::badbit);
    failing_err.exceptions(std::ios::badbit);
    std::array<wchar_t*, 2> argv{const_cast<wchar_t*>(L"wsldisk"), const_cast<wchar_t*>(L"--version")};

    const int exit_code =
        wsldisk::cli::main_entry(static_cast<int>(argv.size()), argv.data(), failing_out, failing_err);

    CHECK(exit_code == wsldisk::exit_code_for(wsldisk::ErrorCode::Generic));
}

TEST_CASE("an unknown subcommand is a usage error", "[cli]") {
    // Anything positional that is not a subcommand is rejected rather than
    // silently ignored.
    //
    // The name here must be one that will never become a real command. `run()`
    // wires up the *real* Win32 services, so a name that later becomes a
    // mutating subcommand would make this unit test act on the machine it runs
    // on -- which is exactly what happened when `compact` landed and this case
    // still said `{"compact", "Ubuntu"}`.
    const auto result = invoke({"wsldisk-not-a-command", "wsldisk-no-such-distro"});
    CHECK(result.exit_code == wsldisk::exit_code_for(wsldisk::ErrorCode::Usage));
    CHECK(result.err.find("error:") != std::string::npos);
}
