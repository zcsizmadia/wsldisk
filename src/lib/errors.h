#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace wsldisk {

/// Every failure the tool can report. The numeric values are the process exit
/// codes documented in PLAN.md §4.9 and are part of the public contract from
/// 1.0 onwards -- scripts depend on them, so they are never renumbered.
enum class ErrorCode {
    /// Something went wrong that does not fit any other category.
    Generic = 1,
    /// The command line was malformed or the arguments contradict each other.
    Usage = 2,
    /// A preflight check refused the operation. Nothing was changed.
    Preflight = 3,
    /// The operation needs an elevated token and could not get one.
    NeedsElevation = 4,
    /// Some steps succeeded and some did not; the output says which.
    Partial = 5,
    /// A read-only integrity check found problems (e.g. `e2fsck -n` errors).
    IntegrityCheckFailed = 6,
    /// The named distribution is not registered.
    DistroNotFound = 10,
    /// The distribution is running, or its disk is held open by another process.
    DistroBusy = 11,
};

/// Exit code for a successful run.
inline constexpr int exit_code_success = 0;

/// Maps an error code to the process exit code.
[[nodiscard]] constexpr int exit_code_for(ErrorCode code) noexcept {
    return static_cast<int>(code);
}

/// A failure with everything the CLI needs to print: what happened and what the
/// user should do about it. `remedy` may be empty only when no action helps.
struct Error {
    ErrorCode code = ErrorCode::Generic;
    std::string message;
    std::string remedy;

    Error(ErrorCode error_code, std::string error_message, std::string error_remedy = {})
        : code(error_code), message(std::move(error_message)), remedy(std::move(error_remedy)) {}

    /// One-line rendering: "message" or "message -- remedy".
    [[nodiscard]] std::string to_string() const;
};

/// Result of an operation that produces a value.
template <typename T>
using Result = std::expected<T, Error>;

/// Result of an operation that only succeeds or fails.
using Status = std::expected<void, Error>;

/// Convenience for `std::unexpected(Error{...})`.
[[nodiscard]] inline std::unexpected<Error> fail(ErrorCode code, std::string message,
                                                 std::string remedy = {}) {
    return std::unexpected(Error{code, std::move(message), std::move(remedy)});
}

/// Stable lower-case token for an error code, used in `--json` output.
[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

}  // namespace wsldisk
