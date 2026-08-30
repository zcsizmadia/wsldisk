#include "virtual_disk.h"

#include <windows.h>
#include <virtdisk.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "win32_api.h"
#include "win32_error.h"

namespace wsldisk::platform {
namespace {

/// How often the compaction loop asks for progress. The operation is I/O bound
/// and runs for seconds to minutes, so this is about the update rate a progress
/// bar wants, not about responsiveness.
constexpr DWORD progress_poll_interval_ms = 250;

/// Reads the parent path out of a GET_VIRTUAL_DISK_INFO buffer without running
/// off the end of it. The API is not required to terminate the string when it
/// exactly fills the buffer, so the capacity bounds the read rather than a NUL.
std::wstring parent_location(const std::vector<GET_VIRTUAL_DISK_INFO>& storage) {
    constexpr std::size_t header = offsetof(GET_VIRTUAL_DISK_INFO, ParentLocation.ParentLocationBuffer);
    const std::size_t bytes = (storage.size() * sizeof(GET_VIRTUAL_DISK_INFO)) - header;
    const std::wstring_view all{
        static_cast<const wchar_t*>(storage.front().ParentLocation.ParentLocationBuffer),
        bytes / sizeof(wchar_t)};
    return std::wstring{all.substr(0, all.find(L'\0'))};
}

/// Closes a Win32 handle through the injection table, so a fake table sees the
/// close as well as the open.
class ScopedHandle {
public:
    ScopedHandle() = default;

    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() {
        // Only nullptr is checked. Every producer that fills a ScopedHandle --
        // OpenVirtualDisk, CreateVirtualDisk, CreateEvent -- reports failure
        // through a return code or a null handle and never yields
        // INVALID_HANDLE_VALUE, so testing for it would add a branch no test
        // could reach. A CreateFile-style producer would need it back.
        if (handle_ != nullptr) {
            std::ignore = win32().close_handle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&&) = delete;
    ScopedHandle& operator=(ScopedHandle&&) = delete;

    [[nodiscard]] PHANDLE put() noexcept { return &handle_; }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    /// Hands ownership to the caller, so a guard held across a successful open
    /// does not close the handle it is about to return.
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

private:
    HANDLE handle_ = nullptr;
};

/// The open handle, and the compaction that runs against it.
class Win32VirtualDiskHandle final : public IVirtualDiskHandle {
public:
    Win32VirtualDiskHandle(HANDLE handle, std::filesystem::path path)
        : handle_(handle), path_(std::move(path)) {}

    [[nodiscard]] Result<VirtualDiskInfo> information() const override;
    [[nodiscard]] Status compact(const ProgressCallback& progress) override;

private:
    ScopedHandle handle_;
    std::filesystem::path path_;
};

Result<VirtualDiskInfo> Win32VirtualDiskHandle::information() const {
    VirtualDiskInfo result;

    GET_VIRTUAL_DISK_INFO info{};
    ULONG size = sizeof(info);
    const DWORD sizes = win32().get_virtual_disk_information(handle_.get(), GET_VIRTUAL_DISK_INFO_SIZE, &size,
                                                             &info, nullptr);
    if (sizes != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(sizes, std::format("read the size of {}", path_.string())));
    }
    result.virtual_size = info.Size.VirtualSize;
    result.physical_size = info.Size.PhysicalSize;
    result.block_size = info.Size.BlockSize;
    result.sector_size = info.Size.SectorSize;

    // A differencing disk names its parent; every other kind reports
    // ERROR_NOT_FOUND here, which is not a failure.
    //
    // ParentLocationBuffer is a variable-length WCHAR array at the tail of the
    // union, so a plain GET_VIRTUAL_DISK_INFO has room for exactly one
    // character. Anything longer needs a bigger allocation, which the API asks
    // for with ERROR_INSUFFICIENT_BUFFER. A vector of whole structs is used
    // rather than a byte buffer so the storage stays correctly aligned.
    std::vector<GET_VIRTUAL_DISK_INFO> storage(1);
    ULONG parent_size = sizeof(GET_VIRTUAL_DISK_INFO);
    DWORD parent_status = win32().get_virtual_disk_information(
        handle_.get(), GET_VIRTUAL_DISK_INFO_PARENT_LOCATION, &parent_size, storage.data(), nullptr);
    if (parent_status == ERROR_INSUFFICIENT_BUFFER) {
        storage.assign((parent_size + sizeof(GET_VIRTUAL_DISK_INFO) - 1) / sizeof(GET_VIRTUAL_DISK_INFO),
                       GET_VIRTUAL_DISK_INFO{});
        parent_size = static_cast<ULONG>(storage.size() * sizeof(GET_VIRTUAL_DISK_INFO));
        parent_status = win32().get_virtual_disk_information(
            handle_.get(), GET_VIRTUAL_DISK_INFO_PARENT_LOCATION, &parent_size, storage.data(), nullptr);
    }
    if (parent_status == ERROR_SUCCESS) {
        result.parent_path = parent_location(storage);
    }
    return result;
}

Status Win32VirtualDiskHandle::compact(const ProgressCallback& progress) {
    // An event is needed for the asynchronous form: CompactVirtualDisk returns
    // ERROR_IO_PENDING and the operation runs until the event signals.
    const ScopedHandle event{win32().create_event(nullptr, TRUE, FALSE, nullptr)};
    if (event.get() == nullptr) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), "create the event for the compaction"));
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    COMPACT_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = COMPACT_VIRTUAL_DISK_VERSION_1;

    const DWORD started =
        win32().compact_virtual_disk(handle_.get(), COMPACT_VIRTUAL_DISK_FLAG_NONE, &parameters, &overlapped);
    if (started != ERROR_SUCCESS && started != ERROR_IO_PENDING) {
        return std::unexpected(error_from_win32(started, std::format("compact {}", path_.string())));
    }

    // A synchronous completion still has to report progress once, so callers see
    // a finished bar rather than an empty one.
    if (started == ERROR_SUCCESS) {
        std::ignore = progress(DiskProgress{.current = 1, .total = 1});
        return {};
    }

    // Every way out of this loop is a return, so the "condition was false" edge
    // does not exist to be covered.
    while (true) {  // LCOV_EXCL_BR_LINE
        const DWORD waited = win32().wait_for_single_object(event.get(), progress_poll_interval_ms);
        if (waited == WAIT_FAILED) {
            return std::unexpected(error_from_win32(
                win32().get_last_error(), std::format("wait for the compaction of {}", path_.string())));
        }

        VIRTUAL_DISK_PROGRESS raw{};
        const DWORD polled = win32().get_virtual_disk_operation_progress(handle_.get(), &overlapped, &raw);
        if (polled != ERROR_SUCCESS) {
            return std::unexpected(
                error_from_win32(polled, std::format("read the compaction progress of {}", path_.string())));
        }

        if (raw.OperationStatus == ERROR_IO_PENDING) {
            if (!progress(DiskProgress{.current = raw.CurrentValue, .total = raw.CompletionValue})) {
                return fail(ErrorCode::Partial, std::format("compaction of {} was cancelled", path_.string()),
                            "the disk is still usable; re-run to finish reclaiming space");
            }
            continue;
        }

        if (raw.OperationStatus != ERROR_SUCCESS) {
            return std::unexpected(
                error_from_win32(raw.OperationStatus, std::format("compact {}", path_.string())));
        }

        std::ignore = progress(DiskProgress{.current = raw.CompletionValue, .total = raw.CompletionValue});
        return {};
    }
}

}  // namespace

VIRTUAL_STORAGE_TYPE vhdx_storage_type() noexcept {
    // virtdisk.h declares VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT with DEFINE_GUID,
    // so the symbol only exists in a translation unit that defines INITGUID
    // first -- which then exports every other GUID in every header it pulls in.
    // Spelling the value out avoids that, and this one is fixed by the VHDX
    // format rather than by an SDK version. Verified against the real API in the
    // M0 compaction spike.
    constexpr GUID microsoft_vendor{
        0xEC984AEC, 0xA0F9, 0x47E9, {0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B}};
    return VIRTUAL_STORAGE_TYPE{.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, .VendorId = microsoft_vendor};
}

Result<std::unique_ptr<IVirtualDiskHandle>> Win32VirtualDisk::open(const std::filesystem::path& path) const {
    VIRTUAL_STORAGE_TYPE storage_type = vhdx_storage_type();

    // Version 2 with VIRTUAL_DISK_ACCESS_NONE. See the class comment: this is
    // measured, not preference.
    OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    parameters.Version2.GetInfoOnly = FALSE;
    parameters.Version2.ReadOnly = FALSE;

    ScopedHandle handle;
    const DWORD opened = win32().open_virtual_disk(&storage_type, path.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                                                   OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, handle.put());
    if (opened != ERROR_SUCCESS) {
        return std::unexpected(
            error_from_win32(opened, std::format("open the virtual disk {}", path.string())));
    }

    return std::make_unique<Win32VirtualDiskHandle>(handle.release(), path);
}

Status Win32VirtualDisk::create(const std::filesystem::path& path, std::uint64_t maximum_size) const {
    VIRTUAL_STORAGE_TYPE storage_type = vhdx_storage_type();

    CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    parameters.Version2.MaximumSize = maximum_size;
    // Zero means "let the provider choose", which is what WSL itself does.
    parameters.Version2.BlockSizeInBytes = 0;
    parameters.Version2.SectorSizeInBytes = 0;

    ScopedHandle handle;
    const DWORD created =
        win32().create_virtual_disk(&storage_type, path.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr,
                                    CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &parameters, nullptr, handle.put());
    if (created != ERROR_SUCCESS) {
        return std::unexpected(
            error_from_win32(created, std::format("create the virtual disk {}", path.string())));
    }
    return {};
}

}  // namespace wsldisk::platform
