#include "errors.h"

#include <array>

namespace wsldisk {

std::string Error::to_string() const {
    if (remedy.empty()) {
        return message;
    }
    return message + " -- " + remedy;
}

namespace {

struct ErrorCodeName {
    ErrorCode code;
    std::string_view name;
};

// A table rather than a switch: an exhaustive switch still carries an implicit
// "no case matched" edge that no test can reach, whereas the fall-through here is
// reachable -- and therefore tested -- for a value that came from a cast.
constexpr std::array<ErrorCodeName, 8> error_code_names{{
    {.code = ErrorCode::Generic, .name = "generic"},
    {.code = ErrorCode::Usage, .name = "usage"},
    {.code = ErrorCode::Preflight, .name = "preflight"},
    {.code = ErrorCode::NeedsElevation, .name = "needs-elevation"},
    {.code = ErrorCode::Partial, .name = "partial"},
    {.code = ErrorCode::IntegrityCheckFailed, .name = "integrity-check-failed"},
    {.code = ErrorCode::DistroNotFound, .name = "distro-not-found"},
    {.code = ErrorCode::DistroBusy, .name = "distro-busy"},
}};

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
    for (const auto& entry : error_code_names) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    // Only reachable for a value that never was an enumerator -- a cast, or a
    // number read back from `--json` output by something older or newer. Naming
    // it "generic" keeps the caller on the safe side of the exit-code contract.
    return "generic";
}

}  // namespace wsldisk
