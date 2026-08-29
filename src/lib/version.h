#pragma once

#include <string_view>

namespace wsldisk {

/// Semantic version of the build, e.g. "0.1.0".
[[nodiscard]] std::string_view version() noexcept;

/// Short git commit the build came from, or "unknown" outside a git checkout.
[[nodiscard]] std::string_view git_revision() noexcept;

/// Full one-line banner: "wsldisk 0.1.0 (abc1234)".
[[nodiscard]] std::string_view version_banner() noexcept;

}  // namespace wsldisk
