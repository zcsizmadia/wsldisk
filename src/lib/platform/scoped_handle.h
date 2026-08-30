#pragma once

#include <windows.h>

#include <utility>

#include "win32_api.h"

namespace wsldisk::platform {

/// Closes a Win32 handle through the injection table, so a fake table sees the
/// close as well as the open.
///
/// Only nullptr is checked on the way out. Every producer that fills one of
/// these -- OpenVirtualDisk, CreateVirtualDisk, CreateEvent, CreatePipe,
/// CreateProcess -- reports failure through a return code or a null handle and
/// never yields INVALID_HANDLE_VALUE, so testing for it would add a branch no
/// test could reach. A CreateFile-style producer would need it back.
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
        if (handle_ != nullptr) {
            std::ignore = win32().close_handle(std::exchange(handle_, nullptr));
        }
    }

private:
    HANDLE handle_ = nullptr;
};

}  // namespace wsldisk::platform
