#include "model/size.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>

using wsldisk::ErrorCode;
using wsldisk::format_size;
using wsldisk::format_size_delta;
using wsldisk::parse_size;

namespace {

std::uint64_t parsed(std::string_view text) {
    const auto result = parse_size(text);
    REQUIRE(result.has_value());
    return *result;
}

void rejects(std::string_view text) {
    INFO("input: " << text);
    const auto result = parse_size(text);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Usage);
    CHECK_FALSE(result.error().remedy.empty());
}

constexpr std::uint64_t kib = 1024;
constexpr std::uint64_t mib = kib * 1024;
constexpr std::uint64_t gib = mib * 1024;
constexpr std::uint64_t tib = gib * 1024;
constexpr std::uint64_t pib = tib * 1024;

}  // namespace

TEST_CASE("a bare number is a byte count", "[size]") {
    CHECK(parsed("0") == 0);
    CHECK(parsed("1") == 1);
    CHECK(parsed("1048576") == 1048576);
    CHECK(parsed("18446744073709551615") == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("suffixes are binary multiples and case-insensitive", "[size]") {
    CHECK(parsed("1B") == 1);
    CHECK(parsed("1b") == 1);
    CHECK(parsed("1K") == kib);
    CHECK(parsed("1KB") == kib);
    CHECK(parsed("1KiB") == kib);
    CHECK(parsed("1m") == mib);
    CHECK(parsed("1MB") == mib);
    CHECK(parsed("1mib") == mib);
    CHECK(parsed("64G") == 64 * gib);
    CHECK(parsed("1GB") == gib);
    CHECK(parsed("1GiB") == gib);
    CHECK(parsed("1T") == tib);
    CHECK(parsed("1tb") == tib);
    CHECK(parsed("1TiB") == tib);
    CHECK(parsed("1P") == pib);
    CHECK(parsed("1PiB") == pib);
}

TEST_CASE("whitespace around and inside the value is ignored", "[size]") {
    CHECK(parsed("  64G") == 64 * gib);
    CHECK(parsed("64G  ") == 64 * gib);
    CHECK(parsed("\t64 G\n") == 64 * gib);
    // A value read from a CRLF file or a pasted line keeps its carriage return.
    CHECK(parsed("\r\n64G\r\n") == 64 * gib);
    CHECK(parsed("64   GiB") == 64 * gib);
}

TEST_CASE("fractional sizes are exact and truncate to whole bytes", "[size]") {
    CHECK(parsed("1.5G") == gib + gib / 2);
    CHECK(parsed("0.5K") == 512);
    CHECK(parsed("2.25M") == mib * 9 / 4);
    // Below one byte the fraction disappears rather than rounding up.
    CHECK(parsed("1.9") == 1);
    CHECK(parsed("0.999999999K") == 1023);
}

TEST_CASE("fraction digits past the ninth cannot change the result", "[size]") {
    // The parser keeps nine digits and drops the rest; both spellings agree.
    CHECK(parsed("1.1234567891234567G") == parsed("1.123456789G"));
    CHECK(parsed("1.999999999999999999P") == parsed("1.999999999P"));
}

TEST_CASE("the largest representable value is accepted at every unit", "[size]") {
    // u64 max divided by the multiplier: the last value that does not overflow.
    CHECK(parsed("18014398509481983K") == 18014398509481983ULL * kib);
    CHECK(parsed("16383P") == 16383ULL * pib);
}

TEST_CASE("nonsense is rejected with a usage error", "[size]") {
    rejects("");
    rejects("   ");
    rejects("G");
    rejects("abc");
    rejects(".5G");
    rejects("1.G");
    rejects("1.");
    rejects("1X");
    rejects("1 GG");
    rejects("-1G");
    rejects("1,5G");
    rejects("1.5.5G");
    rejects("1G extra");
}

TEST_CASE("values that overflow 64 bits are rejected", "[size]") {
    rejects("18446744073709551616");            // u64 max + 1
    rejects("999999999999999999999999999999");  // overflows while multiplying by 10
    rejects("18446744073709551615K");           // overflows when scaled by the suffix
    rejects("16384P");
}

TEST_CASE("format_size prints exact bytes below a kibibyte", "[size]") {
    CHECK(format_size(0) == "0 B");
    CHECK(format_size(1) == "1 B");
    CHECK(format_size(1023) == "1023 B");
}

TEST_CASE("format_size picks the largest unit that keeps the mantissa above one", "[size]") {
    CHECK(format_size(kib) == "1.0 KiB");
    CHECK(format_size(kib + kib / 2) == "1.5 KiB");
    CHECK(format_size(mib) == "1.0 MiB");
    CHECK(format_size(gib) == "1.0 GiB");
    CHECK(format_size(tib) == "1.0 TiB");
    CHECK(format_size(pib) == "1.0 PiB");
    // Beyond PiB the unit stops growing and the mantissa carries the magnitude.
    CHECK(format_size(std::numeric_limits<std::uint64_t>::max()) == "16384.0 PiB");
}

TEST_CASE("format_size_delta always shows the direction", "[size]") {
    CHECK(format_size_delta(0) == "0 B");
    CHECK(format_size_delta(512) == "+512 B");
    CHECK(format_size_delta(-512) == "-512 B");
    CHECK(format_size_delta(static_cast<std::int64_t>(gib)) == "+1.0 GiB");
    CHECK(format_size_delta(-static_cast<std::int64_t>(gib)) == "-1.0 GiB");
    // INT64_MIN has no positive counterpart; the magnitude is computed unsigned.
    CHECK(format_size_delta(std::numeric_limits<std::int64_t>::min()) == "-8192.0 PiB");
}
