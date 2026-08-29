#include "platform/win32_error.h"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "errors.h"

using wsldisk::ErrorCode;
using wsldisk::platform::error_from_win32;
using wsldisk::platform::win32_error_name;

TEST_CASE("every mapped Win32 code has a locale-independent name", "[platform][errors]") {
    CHECK(win32_error_name(ERROR_FILE_NOT_FOUND) == "file not found");
    CHECK(win32_error_name(ERROR_PATH_NOT_FOUND) == "path not found");
    CHECK(win32_error_name(ERROR_ACCESS_DENIED) == "access denied");
    CHECK(win32_error_name(ERROR_INVALID_DRIVE) == "invalid drive");
    CHECK(win32_error_name(ERROR_NOT_READY) == "device not ready");
    CHECK(win32_error_name(ERROR_SHARING_VIOLATION) == "file is in use by another process");
    CHECK(win32_error_name(ERROR_LOCK_VIOLATION) == "file region is locked");
    CHECK(win32_error_name(ERROR_DISK_FULL) == "disk full");
    CHECK(win32_error_name(ERROR_NOT_SUPPORTED) == "operation not supported on this volume");
    CHECK(win32_error_name(ERROR_INVALID_HANDLE) == "unexpected Win32 error");
}

TEST_CASE("a missing path is a preflight failure", "[platform][errors]") {
    for (const DWORD code : {DWORD{ERROR_FILE_NOT_FOUND}, DWORD{ERROR_PATH_NOT_FOUND}}) {
        const auto error = error_from_win32(code, "open the disk");
        CHECK(error.code == ErrorCode::Preflight);
        CHECK(error.remedy.find("spelled correctly") != std::string::npos);
    }
}

TEST_CASE("access denied points at elevation", "[platform][errors]") {
    const auto error = error_from_win32(ERROR_ACCESS_DENIED, "attach the disk");
    CHECK(error.code == ErrorCode::NeedsElevation);
    CHECK(error.remedy.find("--elevate") != std::string::npos);
}

TEST_CASE("a locked file points at wsldisk lock", "[platform][errors]") {
    for (const DWORD code : {DWORD{ERROR_SHARING_VIOLATION}, DWORD{ERROR_LOCK_VIOLATION}}) {
        const auto error = error_from_win32(code, "compact the disk");
        CHECK(error.code == ErrorCode::DistroBusy);
        CHECK(error.remedy.find("wsldisk lock") != std::string::npos);
    }
}

TEST_CASE("drive problems are preflight failures", "[platform][errors]") {
    for (const DWORD code : {DWORD{ERROR_INVALID_DRIVE}, DWORD{ERROR_NOT_READY}}) {
        const auto error = error_from_win32(code, "read the volume");
        CHECK(error.code == ErrorCode::Preflight);
        CHECK(error.remedy.find("present and ready") != std::string::npos);
    }
}

TEST_CASE("a full disk is a preflight failure with a clear remedy", "[platform][errors]") {
    const auto error = error_from_win32(ERROR_DISK_FULL, "copy the disk");
    CHECK(error.code == ErrorCode::Preflight);
    CHECK(error.remedy.find("free space") != std::string::npos);
}

TEST_CASE("an unmapped code degrades to a generic error with no remedy", "[platform][errors]") {
    const auto error = error_from_win32(ERROR_INVALID_HANDLE, "close the disk");
    CHECK(error.code == ErrorCode::Generic);
    CHECK(error.remedy.empty());
}

TEST_CASE("the message names the attempt, the reason and the raw code", "[platform][errors]") {
    const auto error = error_from_win32(ERROR_ACCESS_DENIED, "attach C:\\wsl\\ext4.vhdx");
    CHECK(error.message == "failed to attach C:\\wsl\\ext4.vhdx: access denied (Win32 error 5)");
}
