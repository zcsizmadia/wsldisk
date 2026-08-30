#pragma once

#include <windows.h>

#include <utility>

#include "win32_api.h"

namespace wsldisk::platform {

/// Closes a Win32 handle through the injection table, so a fake table sees the
/// close as well as the open.
///
/// Both null and INVALID_HANDLE_VALUE are checked. Every producer that fills one
/// of these today -- OpenVirtualDisk, CreateVirtualDisk, CreateEvent,
/// CreatePipe, CreateProcess -- reports failure through a return code or a null
/// handle, so nothing currently reaches the second check and no test can cover
/// it. It stays anyway: CreateFile-style producers signal failure with
/// INVALID_HANDLE_VALUE, and a guard that quietly stops guarding the day someone
/// adds one is worse than an uncoverable branch.
class ScopedHandle {
public:
    ScopedHandle() = default;

    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() { close(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&&) = delete;
    ScopedHandle& operator=(ScopedHandle&&) = delete;

    [[nodiscard]] PHANDLE put() noexcept { return &handle_; }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    /// Hands ownership to the caller, so a guard held across a successful open
    /// does not close the handle it is about to return.
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

    /// Closes early. A pipe's write end has to be closed in the parent before
    /// the read end will ever report end-of-file, which is sooner than the
    /// guard would otherwise do it.
    void close() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {  // LCOV_EXCL_BR_LINE
            std::ignore = win32().close_handle(std::exchange(handle_, nullptr));
        }
    }

private:
    HANDLE handle_ = nullptr;
};

}  // namespace wsldisk::platform
