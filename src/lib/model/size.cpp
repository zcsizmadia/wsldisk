#include "size.h"

#include <array>
#include <format>
#include <limits>
#include <optional>

namespace wsldisk {
namespace {

constexpr std::uint64_t kib = 1024;
constexpr std::uint64_t mib = kib * 1024;
constexpr std::uint64_t gib = mib * 1024;
constexpr std::uint64_t tib = gib * 1024;
constexpr std::uint64_t pib = tib * 1024;

struct Suffix {
    std::string_view text;
    std::uint64_t multiplier;
};

// Every suffix is a binary multiple; `KB` is an accepted alias for `KiB`, not 1000.
constexpr std::array<Suffix, 16> suffixes{{
    {"", 1},
    {"b", 1},
    {"k", kib},
    {"kb", kib},
    {"kib", kib},
    {"m", mib},
    {"mb", mib},
    {"mib", mib},
    {"g", gib},
    {"gb", gib},
    {"gib", gib},
    {"t", tib},
    {"tb", tib},
    {"tib", tib},
    {"p", pib},
    {"pib", pib},
}};

constexpr std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();

// Fraction digits past this point cannot change a byte count at any supported
// unit, and the cap is what keeps the fixed-point arithmetic below overflow-free.
constexpr std::size_t max_fraction_digits = 9;

/// A parsed `<whole>[.<fraction>]`, with the fraction kept as an exact ratio.
struct Number {
    std::uint64_t whole = 0;
    std::uint64_t frac_numerator = 0;
    std::uint64_t frac_denominator = 1;
};

[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::unexpected<Error> bad_size(std::string_view text) {
    return fail(ErrorCode::Usage, std::format("'{}' is not a valid size", text),
                "use a byte count or a binary suffix, for example 64G, 1.5TiB or 1048576");
}

/// `value * factor` with `factor > 0`, or nullopt on overflow.
[[nodiscard]] std::optional<std::uint64_t> checked_mul(std::uint64_t value, std::uint64_t factor) noexcept {
    if (value > u64_max / factor) {
        return std::nullopt;
    }
    return value * factor;
}

/// `a + b`, or nullopt on overflow.
[[nodiscard]] std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
    if (a > u64_max - b) {
        return std::nullopt;
    }
    return a + b;
}

/// Reads the digits before the decimal point. Fails on overflow or on no digits.
[[nodiscard]] std::optional<std::uint64_t> parse_whole(std::string_view input, std::size_t& pos) {
    std::uint64_t whole = 0;
    std::size_t digits = 0;
    while (pos < input.size() && is_digit(input[pos])) {
        const auto scaled = checked_mul(whole, 10);
        if (!scaled) {
            return std::nullopt;
        }
        const auto added = checked_add(*scaled, static_cast<std::uint64_t>(input[pos] - '0'));
        if (!added) {
            return std::nullopt;
        }
        whole = *added;
        ++digits;
        ++pos;
    }
    return digits == 0 ? std::nullopt : std::optional{whole};
}

/// Reads an optional `.<digits>` fraction into `number`. Fails on a bare `.`.
[[nodiscard]] bool parse_fraction(std::string_view input, std::size_t& pos, Number& number) {
    if (pos >= input.size() || input[pos] != '.') {
        return true;
    }
    ++pos;

    std::size_t digits = 0;
    while (pos < input.size() && is_digit(input[pos])) {
        if (digits < max_fraction_digits) {
            number.frac_numerator = number.frac_numerator * 10 + static_cast<std::uint64_t>(input[pos] - '0');
            number.frac_denominator *= 10;
            ++digits;
        }
        ++pos;
    }
    return digits > 0;
}

/// Reads the trailing unit and returns its multiplier, or nullopt if unknown.
[[nodiscard]] std::optional<std::uint64_t> parse_multiplier(std::string_view input, std::size_t pos) {
    while (pos < input.size() && is_space(input[pos])) {
        ++pos;
    }

    std::string unit;
    unit.reserve(input.size() - pos);
    for (std::size_t i = pos; i < input.size(); ++i) {
        unit.push_back(lower(input[i]));
    }

    for (const auto& suffix : suffixes) {
        if (suffix.text == unit) {
            return suffix.multiplier;
        }
    }
    return std::nullopt;
}

/// floor(numerator * multiplier / denominator) without an intermediate overflow,
/// given `numerator < denominator <= 10^9` and `multiplier <= 2^50`.
[[nodiscard]] std::uint64_t scale_fraction(const Number& number, std::uint64_t multiplier) noexcept {
    // numerator * (multiplier / denominator) < multiplier, and
    // numerator * (multiplier % denominator) < denominator^2 <= 10^18: both fit.
    return number.frac_numerator * (multiplier / number.frac_denominator) +
           (number.frac_numerator * (multiplier % number.frac_denominator)) / number.frac_denominator;
}

}  // namespace

Result<std::uint64_t> parse_size(std::string_view text) {
    const std::string_view input = trim(text);
    if (input.empty()) {
        return bad_size(text);
    }

    std::size_t pos = 0;
    Number number;

    const auto whole = parse_whole(input, pos);
    if (!whole) {
        return bad_size(text);
    }
    number.whole = *whole;

    if (!parse_fraction(input, pos, number)) {
        return bad_size(text);
    }

    const auto multiplier = parse_multiplier(input, pos);
    if (!multiplier) {
        return bad_size(text);
    }

    const auto whole_bytes = checked_mul(number.whole, *multiplier);
    if (!whole_bytes) {
        return bad_size(text);
    }
    // `whole * multiplier <= u64_max` and `multiplier` is a power of two, so at most
    // `multiplier - 1` bytes remain before the maximum -- exactly the range the
    // fractional part can add. The sum therefore cannot overflow.
    return *whole_bytes + scale_fraction(number, *multiplier);
}

std::string format_size(std::uint64_t bytes) {
    constexpr std::array<std::string_view, 5> units{"KiB", "MiB", "GiB", "TiB", "PiB"};

    if (bytes < kib) {
        return std::format("{} B", bytes);
    }

    auto value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit_index = 0;
    while (value >= 1024.0 && unit_index + 1 < units.size()) {
        value /= 1024.0;
        ++unit_index;
    }
    return std::format("{:.1f} {}", value, units[unit_index]);
}

std::string format_size_delta(std::int64_t bytes) {
    if (bytes == 0) {
        return "0 B";
    }
    // Negating INT64_MIN is undefined; go through the unsigned domain instead.
    const auto magnitude =
        bytes < 0 ? (~static_cast<std::uint64_t>(bytes) + 1) : static_cast<std::uint64_t>(bytes);
    return std::format("{}{}", bytes < 0 ? "-" : "+", format_size(magnitude));
}

}  // namespace wsldisk
