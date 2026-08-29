#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "../errors.h"

namespace wsldisk {

/// Parses a human-written size such as `64G`, `1.5TiB`, `512MB` or `1048576`.
///
/// All suffixes are binary multiples: `K`/`KB`/`KiB` are all 1024 bytes, and so
/// on through `P`. A bare number is bytes. Whitespace around the value and
/// between the number and the suffix is ignored; the suffix is case-insensitive.
/// A fractional value is allowed (`1.5G`) and truncated to whole bytes.
///
/// Returns `ErrorCode::Usage` for anything that is not a size.
[[nodiscard]] Result<std::uint64_t> parse_size(std::string_view text);

/// Renders a byte count for humans: `0 B`, `512 B`, `1.5 GiB`, `2.0 TiB`.
///
/// Values below 1 KiB are printed exactly; larger ones use the biggest binary
/// unit that keeps the mantissa at or above 1, with one decimal place.
[[nodiscard]] std::string format_size(std::uint64_t bytes);

/// Renders a signed difference with an explicit sign: `-1.5 GiB`, `+2.0 MiB`, `0 B`.
[[nodiscard]] std::string format_size_delta(std::int64_t bytes);

}  // namespace wsldisk
