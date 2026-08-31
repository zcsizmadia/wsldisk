// libFuzzer target for the config parsers.
//
// `config.toml` is the one file a user hand-edits, and it is parsed before any
// command does anything -- so a crash here is a crash in every command. The
// TOML parser itself is a third-party library being fed arbitrary bytes, which
// is exactly the shape that repays fuzzing.
//
// `.wslconfig` is here too, and matters more than it looks: it is *not* written
// by this tool, so its contents are entirely outside our control, and the
// hand-rolled INI reader that scans it has all the usual off-by-one hazards
// around empty lines, missing `=` and a file with no trailing newline.
//
// The target asserts invariants rather than values: a fuzzer cannot know what a
// given config should mean, but it can prove the parsers never crash, never
// disagree with themselves, and never produce a config that fails to survive a
// round trip through the serialiser.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "model/config.h"

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

/// Every rejection has to be something the CLI can print and act on.
void check_rejection(const wsldisk::Error& error, std::string_view input) {
    FUZZ_REQUIRE(error.code == wsldisk::ErrorCode::Usage, input);
    FUZZ_REQUIRE(!error.message.empty(), input);
    FUZZ_REQUIRE(!error.remedy.empty(), input);
}

/// Whatever parsed has to survive being written back out and read in again.
///
/// This is the invariant `config set` depends on: it goes value -> struct ->
/// text, and a struct the serialiser cannot express would silently lose the
/// user's setting.
void check_round_trip(const wsldisk::model::Config& config, std::string_view input) {
    const std::string rendered = wsldisk::model::render_config(config);
    const auto reparsed = wsldisk::model::parse_config(rendered);
    FUZZ_REQUIRE(reparsed.has_value(), input);
    FUZZ_REQUIRE(reparsed->scan_dirs == config.scan_dirs, input);
    FUZZ_REQUIRE(reparsed->compact_trim == config.compact_trim, input);
    FUZZ_REQUIRE(reparsed->compact_restart == config.compact_restart, input);
    FUZZ_REQUIRE(reparsed->unlock_timeout_seconds == config.unlock_timeout_seconds, input);
}

/// Every key the CLI offers has to be readable, and every value it reads has to
/// be one it can set back.
void check_keys(const wsldisk::model::Config& config, std::string_view input) {
    for (const std::string& key : wsldisk::model::config_keys()) {
        const auto value = wsldisk::model::get_config_value(config, key);
        FUZZ_REQUIRE(value.has_value(), input);

        wsldisk::model::Config copy = config;
        const auto status = wsldisk::model::set_config_value(copy, key, *value);
        FUZZ_REQUIRE(status.has_value(), input);
        FUZZ_REQUIRE(wsldisk::model::get_config_value(copy, key) == value, input);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input{reinterpret_cast<const char*>(data), size};

    const auto config = wsldisk::model::parse_config(input);

    // Parsing must be a pure function of its input.
    const auto again = wsldisk::model::parse_config(input);
    FUZZ_REQUIRE(config.has_value() == again.has_value(), input);

    if (config.has_value()) {
        check_round_trip(*config, input);
        check_keys(*config, input);
        // A value read out of a file must be one the operation can use.
        FUZZ_REQUIRE(config->unlock_timeout().count() >= 0, input);
    } else {
        check_rejection(config.error(), input);
    }

    // The same bytes as `.wslconfig`, which is INI rather than TOML and is read
    // by a hand-rolled scanner. It has no failure mode: anything it does not
    // recognise is simply absent.
    const auto wsl = wsldisk::model::parse_wslconfig(input);
    const bool any =
        wsl.default_vhd_size.has_value() || wsl.vhd_size.has_value() || wsl.swap_file.has_value();
    FUZZ_REQUIRE(wsl.empty() != any, input);

    return 0;
}
