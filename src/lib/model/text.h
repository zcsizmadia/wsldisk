#pragma once

#include <filesystem>
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

/// A filesystem path as UTF-8, for stdout and for JSON.
///
/// Not `path::string()`. On MSVC that converts through the *active code page*,
/// which is UTF-8 only if the machine has been set that way. On a Japanese
/// locale (ACP 932) it produces Shift-JIS bytes, and handing those to
/// nlohmann's `dump()` throws `type_error.316` for invalid UTF-8 -- so `list
/// --json` fails with a JSON-library message instead of listing anything. On a
/// 1252 machine an unmappable character silently becomes `?`, which is worse:
/// the output parses and names a file that does not exist.
///
/// Every path that reaches the user goes through here. `base_path` already did,
/// which is how the inconsistency was noticed.
///
/// A distinct name rather than an overload of `to_utf8`: a `const wchar_t*`
/// converts to both `std::wstring_view` and `std::filesystem::path`, so every
/// call with a wide literal became ambiguous. An overload set that silently
/// picks a different function is how the `std::quoted` collision happened in
/// model/config.cpp -- MSVC and clang chose differently.
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

/// UTF-8 to UTF-16, for handing a name back to a `W` entry point.
///
/// Malformed input becomes U+FFFD on the same reasoning.
[[nodiscard]] std::wstring to_wide(std::string_view text);

}  // namespace wsldisk::model
