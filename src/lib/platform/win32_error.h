#pragma once

#include <windows.h>

#include <string>
#include <string_view>

#include "../errors.h"

namespace wsldisk::platform {

/// Short, locale-independent description of a Win32 error code.
///
/// Deliberately not `FormatMessage`: the text ends up in `--json` output and in
/// bug reports, so it must read the same on every machine (PLAN.md, "localization-safe").
[[nodiscard]] std::string_view win32_error_name(DWORD code) noexcept;

/// Turns a Win32 error into an `Error` carrying the category the CLI maps to an
/// exit code, plus a remedy the user can act on. `context` describes what was
/// being attempted, e.g. "read the size of C:\wsl\ext4.vhdx".
[[nodiscard]] Error error_from_win32(DWORD code, std::string_view context);

}  // namespace wsldisk::platform
