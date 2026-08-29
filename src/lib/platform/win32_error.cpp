#include "win32_error.h"

#include <format>

namespace wsldisk::platform {

std::string_view win32_error_name(DWORD code) noexcept {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
            return "file not found";
        case ERROR_PATH_NOT_FOUND:
            return "path not found";
        case ERROR_ACCESS_DENIED:
            return "access denied";
        case ERROR_INVALID_DRIVE:
            return "invalid drive";
        case ERROR_NOT_READY:
            return "device not ready";
        case ERROR_SHARING_VIOLATION:
            return "file is in use by another process";
        case ERROR_LOCK_VIOLATION:
            return "file region is locked";
        case ERROR_DISK_FULL:
            return "disk full";
        case ERROR_NOT_SUPPORTED:
            return "operation not supported on this volume";
        default:
            return "unexpected Win32 error";
    }
}

Error error_from_win32(DWORD code, std::string_view context) {
    const std::string message =
        std::format("failed to {}: {} (Win32 error {})", context, win32_error_name(code), code);

    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return Error{ErrorCode::Preflight, message,
                         "check that the path exists and is spelled correctly"};
        case ERROR_ACCESS_DENIED:
            return Error{ErrorCode::NeedsElevation, message,
                         "re-run from an elevated prompt, or pass --elevate"};
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return Error{ErrorCode::DistroBusy, message,
                         "close whatever is using the file; `wsldisk lock` names the process"};
        case ERROR_INVALID_DRIVE:
        case ERROR_NOT_READY:
            return Error{ErrorCode::Preflight, message, "check that the drive is present and ready"};
        case ERROR_DISK_FULL:
            return Error{ErrorCode::Preflight, message, "free space on the target volume and retry"};
        default:
            return Error{ErrorCode::Generic, message, {}};
    }
}

}  // namespace wsldisk::platform
