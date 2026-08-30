// libFuzzer target for the size parser.
//
// `parse_size` is the first thing a user's input touches (`--to 64G`), it does
// its own fixed-point arithmetic with hand-written overflow checks, and it runs
// before any preflight would catch a bad value. That combination is what makes
// it worth fuzzing rather than only unit-testing.
//
// The target asserts invariants, not specific values -- a fuzzer cannot know
// what `64G` should mean, but it can prove the function never crashes, never
// disagrees with itself, and never returns a failure the CLI cannot render.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "model/size.h"

namespace {

// NDEBUG is defined in the Release configurations the fuzzers build in, so
// `assert` would vanish exactly where it is needed. Abort explicitly instead:
// libFuzzer treats the crash as a finding and writes the reproducer.
#define FUZZ_REQUIRE(condition, input)                                                  \
    do {                                                                                \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "invariant failed: %s\n  input: '%.*s'\n", #condition, \
                         static_cast<int>((input).size()), (input).data());             \
            std::abort();                                                               \
        }                                                                               \
    } while (false)

constexpr std::uint64_t kib = 1024;

/// Every rejection has to be something the CLI can print and act on.
void check_rejection(const wsldisk::Error& error, std::string_view input) {
    FUZZ_REQUIRE(error.code == wsldisk::ErrorCode::Usage, input);
    FUZZ_REQUIRE(!error.message.empty(), input);
    FUZZ_REQUIRE(!error.remedy.empty(), input);
    FUZZ_REQUIRE(!error.to_string().empty(), input);
}

/// Invariants that must hold for any accepted value.
void check_acceptance(std::uint64_t value, std::string_view input) {
    const std::string rendered = wsldisk::format_size(value);
    FUZZ_REQUIRE(!rendered.empty(), input);
    FUZZ_REQUIRE(rendered.find(' ') != std::string::npos, input);

    // Below a kibibyte the rendering is exact ("512 B"), so it must survive a
    // round trip unchanged. Above that it is deliberately lossy (one decimal),
    // and only has to stay parseable.
    const auto reparsed = wsldisk::parse_size(rendered);
    FUZZ_REQUIRE(reparsed.has_value(), input);
    if (value < kib) {
        FUZZ_REQUIRE(*reparsed == value, input);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input{reinterpret_cast<const char*>(data), size};

    const auto result = wsldisk::parse_size(input);

    // Parsing must be a pure function of its input.
    const auto again = wsldisk::parse_size(input);
    FUZZ_REQUIRE(result.has_value() == again.has_value(), input);
    if (result.has_value()) {
        FUZZ_REQUIRE(*result == *again, input);
        check_acceptance(*result, input);
    } else {
        check_rejection(result.error(), input);
    }

    // Reuse the same bytes as a number so the delta renderer is exercised too;
    // it has its own sign and INT64_MIN handling.
    std::uint64_t raw = 0;
    std::memcpy(&raw, data, size < sizeof(raw) ? size : sizeof(raw));
    const std::string delta = wsldisk::format_size_delta(static_cast<std::int64_t>(raw));
    FUZZ_REQUIRE(!delta.empty(), input);

    return 0;
}
