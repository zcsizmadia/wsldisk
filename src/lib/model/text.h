#pragma once

#include <string>
#include <string_view>

namespace wsldisk::model {

/// UTF-16 to UTF-8.
///
/// The registry and every Win32 `W` entry point speak UTF-16; the console, the
/// JSON output and `IWslHost` speak UTF-8. This is the one place that converts,
/// so a distribution name with a non-ASCII character survives the trip in one
/// piece instead of being narrowed to `?`.
///
/// Unpaired surrogates become U+FFFD rather than being dropped or thrown on: a
/// name that Windows accepted has to remain printable, and refusing to list a
/// distribution because its name is malformed would be worse than showing a
/// replacement character.
[[nodiscard]] std::string to_utf8(std::wstring_view text);

/// UTF-8 to UTF-16, for handing a name back to a `W` entry point.
///
/// Malformed input becomes U+FFFD on the same reasoning.
[[nodiscard]] std::wstring to_wide(std::string_view text);

}  // namespace wsldisk::model
