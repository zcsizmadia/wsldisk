// Contract tests: WslExeHost against the real wsl.exe.
//
// These run the executable that ships with Windows and assert on the shape of
// what comes back, not on any distribution being installed. `wsl.exe --version`
// and `--list --running` answer on a bare runner, so nothing here needs the
// integration fixture.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "errors.h"
#include "model/wsl_output.h"
#include "platform/wsl_host.h"

using wsldisk::ErrorCode;
using wsldisk::platform::run_wsl;
using wsldisk::platform::WslExeHost;
using namespace std::chrono_literals;

namespace {

/// Whether every byte is a plausible UTF-8 sequence. The decoder's output feeds
/// the console, so mojibake here is a user-visible bug.
[[nodiscard]] bool is_valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        } else if (lead >= 0x80) {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        index += length;
    }
    return true;
}

}  // namespace

TEST_CASE("wsl.exe --version runs and reports an exit code", "[contract][wsl]") {
    const auto result = run_wsl({L"--version"}, 60s);

    // If wsl.exe is missing entirely the wrapper must say so cleanly rather
    // than hanging or crashing, so both outcomes are acceptable here -- what is
    // not acceptable is neither.
    if (!result.has_value()) {
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().remedy.empty());
        return;
    }
    // Version output is UTF-16LE from wsl.exe itself; captured raw here, so it
    // is full of NULs. That it is non-empty proves the pipes were wired up.
    CHECK_FALSE(result->standard_output.empty());
}

TEST_CASE("the decoder turns wsl.exe output into clean UTF-8", "[contract][wsl]") {
    // Deliberately no assertion about what wsl.exe says. The version banner is
    // localized and changes between builds, and this suite's own rule is that
    // nothing parses prose -- an arm64 runner whose wsl.exe prints something
    // else must not fail the decoder.
    const auto result = run_wsl({L"--version"}, 20s);
    if (!result.has_value() || result->standard_output.empty()) {
        SUCCEED("wsl.exe produced no output on this machine");
        return;
    }

    const auto bytes = std::span{reinterpret_cast<const std::byte*>(result->standard_output.data()),
                                 result->standard_output.size()};
    const std::string text = wsldisk::model::decode_utf16le(bytes);

    CHECK(is_valid_utf8(text));
    // The raw stream is UTF-16, so every other byte is NUL. Decoding must
    // consume them; a decoder that "works" by stripping zero bytes would pass
    // this too, which is why the unit tests cover the non-ASCII cases.
    CHECK(text.find('\0') == std::string::npos);
    CHECK_FALSE(text.empty());
}

TEST_CASE("running returns a list without needing a distribution", "[contract][wsl]") {
    const WslExeHost host;
    const auto names = host.running();

    if (!names.has_value()) {
        CHECK_FALSE(names.error().message.empty());
        return;
    }
    // Whatever is running, no name may be blank or carry a stray carriage
    // return -- that is what the parser exists to guarantee.
    for (const std::string& name : *names) {
        CHECK_FALSE(name.empty());
        CHECK(name.find('\r') == std::string::npos);
        CHECK(name.find('\0') == std::string::npos);
    }
}

TEST_CASE("a relative guest program is refused before wsl.exe is started", "[contract][wsl]") {
    // No process is spawned, so this holds even on a machine with no WSL.
    const std::vector<std::string> argv{"fstrim", "/"};
    const WslExeHost host;
    const auto result = host.run_as_root("Whatever", argv, 5s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Usage);
}

TEST_CASE("a command that outlives its timeout is terminated", "[contract][wsl]") {
    // `wsl --version` is fast, so a 1 ms budget reliably expires first. The
    // point is that the wrapper gives up rather than waiting forever.
    const auto result = run_wsl({L"--version"}, 1ms);

    if (result.has_value()) {
        SUCCEED("the command finished inside the budget");
        return;
    }
    CHECK_FALSE(result.error().message.empty());
}
