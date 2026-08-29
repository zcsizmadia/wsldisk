#include "errors.h"

namespace wsldisk {

std::string Error::to_string() const {
    if (remedy.empty()) {
        return message;
    }
    return message + " -- " + remedy;
}

std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Generic:
            return "generic";
        case ErrorCode::Usage:
            return "usage";
        case ErrorCode::Preflight:
            return "preflight";
        case ErrorCode::NeedsElevation:
            return "needs-elevation";
        case ErrorCode::Partial:
            return "partial";
        case ErrorCode::IntegrityCheckFailed:
            return "integrity-check-failed";
        case ErrorCode::DistroNotFound:
            return "distro-not-found";
        case ErrorCode::DistroBusy:
            return "distro-busy";
    }
    // Unreachable: the switch above is exhaustive and /W4 flags any new enumerator.
    return "generic";  // LCOV_EXCL_LINE
}

}  // namespace wsldisk
