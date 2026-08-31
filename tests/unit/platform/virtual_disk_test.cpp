#include "platform/virtual_disk.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "errors.h"
#include "platform/win32_api.h"

using wsldisk::DiskProgress;
using wsldisk::ErrorCode;
using wsldisk::platform::ScopedWin32Api;
using wsldisk::platform::Win32Api;
using wsldisk::platform::Win32VirtualDisk;

namespace {

/// A table where every virtual-disk call fails; tests override what they need.
Win32Api all_failing(DWORD status) {
    Win32Api api;
    api.open_virtual_disk = [status](PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK,
                                     OPEN_VIRTUAL_DISK_FLAG, POPEN_VIRTUAL_DISK_PARAMETERS,
                                     PHANDLE) { return status; };
    api.get_virtual_disk_information = [status](HANDLE, GET_VIRTUAL_DISK_INFO_VERSION, PULONG,
                                                PGET_VIRTUAL_DISK_INFO, PULONG) { return status; };
    api.compact_virtual_disk = [status](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                        LPOVERLAPPED) { return status; };
    api.get_virtual_disk_operation_progress = [status](HANDLE, LPOVERLAPPED, PVIRTUAL_DISK_PROGRESS) {
        return status;
    };
    api.create_virtual_disk = [status](PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK,
                                       PSECURITY_DESCRIPTOR, CREATE_VIRTUAL_DISK_FLAG, ULONG,
                                       PCREATE_VIRTUAL_DISK_PARAMETERS, LPOVERLAPPED,
                                       PHANDLE) { return status; };
    api.close_handle = [](HANDLE) -> BOOL { return TRUE; };
    api.create_event = [](LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR) -> HANDLE {
        return reinterpret_cast<HANDLE>(2);
    };
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_OBJECT_0; };
    // Succeeds and reports the operation finished, which is the ordinary answer:
    // `compact` only calls this on a path where it is abandoning a started
    // compaction, and every one of those paths waits for it to stop.
    api.cancel_io_ex = [](HANDLE, LPOVERLAPPED) -> BOOL { return TRUE; };
    api.get_last_error = []() -> DWORD { return ERROR_ACCESS_DENIED; };
    return api;
}

/// A table whose disks open; everything after that still fails unless overridden.
Win32Api opens_ok(DWORD other_status = ERROR_ACCESS_DENIED) {
    Win32Api api = all_failing(other_status);
    api.open_virtual_disk = [](PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK,
                               OPEN_VIRTUAL_DISK_FLAG, POPEN_VIRTUAL_DISK_PARAMETERS,
                               PHANDLE handle) -> DWORD {
        *handle = reinterpret_cast<HANDLE>(1);
        return ERROR_SUCCESS;
    };
    return api;
}

}  // namespace

TEST_CASE("open uses the V2 parameters that spike 1 measured", "[platform][vdisk]") {
    // The whole point of D10: V2 accepts VIRTUAL_DISK_ACCESS_NONE and nothing
    // else, so any other mask here stops the disk opening at all.
    VIRTUAL_DISK_ACCESS_MASK seen_mask = VIRTUAL_DISK_ACCESS_ALL;
    ULONG seen_version = 0;

    Win32Api api = opens_ok();
    api.open_virtual_disk = [&](PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK mask,
                                OPEN_VIRTUAL_DISK_FLAG, POPEN_VIRTUAL_DISK_PARAMETERS parameters,
                                PHANDLE handle) -> DWORD {
        seen_mask = mask;
        seen_version = parameters->Version;
        *handle = reinterpret_cast<HANDLE>(1);
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\wsl\\ext4.vhdx");

    REQUIRE(handle.has_value());
    CHECK(seen_mask == VIRTUAL_DISK_ACCESS_NONE);
    CHECK(seen_version == OPEN_VIRTUAL_DISK_VERSION_2);
}

TEST_CASE("open reports a disk that cannot be opened", "[platform][vdisk]") {
    const ScopedWin32Api scoped{all_failing(ERROR_FILE_NOT_FOUND)};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\missing.vhdx");

    REQUIRE_FALSE(handle.has_value());
    CHECK(handle.error().code == ErrorCode::Preflight);
    CHECK(handle.error().message.find("missing.vhdx") != std::string::npos);
}

TEST_CASE("open reports a disk held by something else", "[platform][vdisk]") {
    // What a running distribution's disk looks like: the utility VM has it.
    const ScopedWin32Api scoped{all_failing(ERROR_SHARING_VIOLATION)};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\busy.vhdx");

    REQUIRE_FALSE(handle.has_value());
    CHECK(handle.error().code == ErrorCode::DistroBusy);
    CHECK(handle.error().remedy.find("wsldisk lock") != std::string::npos);
}

TEST_CASE("information reports sizes and geometry", "[platform][vdisk]") {
    Win32Api api = opens_ok();
    api.get_virtual_disk_information = [](HANDLE, GET_VIRTUAL_DISK_INFO_VERSION version, PULONG,
                                          PGET_VIRTUAL_DISK_INFO info, PULONG) -> DWORD {
        if (version == GET_VIRTUAL_DISK_INFO_SIZE) {
            info->Size.VirtualSize = 1099511627776ULL;  // the 1 TiB WSL default
            info->Size.PhysicalSize = 14799601664ULL;
            info->Size.BlockSize = 2097152;
            info->Size.SectorSize = 512;
            return ERROR_SUCCESS;
        }
        // Not a differencing disk.
        return ERROR_NOT_FOUND;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
    REQUIRE(handle.has_value());

    const auto info = (*handle)->information();
    REQUIRE(info.has_value());
    CHECK(info->virtual_size == 1099511627776ULL);
    CHECK(info->physical_size == 14799601664ULL);
    CHECK(info->block_size == 2097152);
    CHECK(info->sector_size == 512);
    CHECK(info->parent_path.empty());
}

/// Bytes of GET_VIRTUAL_DISK_INFO that precede the variable-length parent path.
constexpr std::size_t parent_header = offsetof(GET_VIRTUAL_DISK_INFO, ParentLocation.ParentLocationBuffer);

TEST_CASE("information reports a differencing disk's parent", "[platform][vdisk]") {
    // A real parent path does not fit in a lone GET_VIRTUAL_DISK_INFO, whose
    // tail array holds one WCHAR. The API answers the first, too-small call with
    // ERROR_INSUFFICIENT_BUFFER and the size it wants; this fake does the same,
    // so the growth path is exercised rather than assumed.
    const std::wstring_view parent{L"C:\\wsl\\base.vhdx"};
    const auto needed = static_cast<ULONG>(parent_header + ((parent.size() + 1) * sizeof(wchar_t)));

    Win32Api api = opens_ok();
    api.get_virtual_disk_information = [parent, needed](HANDLE, GET_VIRTUAL_DISK_INFO_VERSION version,
                                                        PULONG size, PGET_VIRTUAL_DISK_INFO info,
                                                        PULONG) -> DWORD {
        if (version == GET_VIRTUAL_DISK_INFO_SIZE) {
            info->Size.VirtualSize = 1024;
            return ERROR_SUCCESS;
        }
        if (*size < needed) {
            *size = needed;
            return ERROR_INSUFFICIENT_BUFFER;
        }
        std::memcpy(info->ParentLocation.ParentLocationBuffer, parent.data(),
                    (parent.size() + 1) * sizeof(wchar_t));
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\wsl\\child.vhdx");
    REQUIRE(handle.has_value());

    const auto info = (*handle)->information();
    REQUIRE(info.has_value());
    CHECK(info->parent_path == L"C:\\wsl\\base.vhdx");
}

TEST_CASE("a parent path that fills the buffer is not read past its end", "[platform][vdisk]") {
    // Nothing promises a terminating NUL when the string exactly fills the
    // buffer. Reading to the capacity instead of to a NUL is what keeps that
    // case from walking off the allocation.
    Win32Api api = opens_ok();
    api.get_virtual_disk_information = [](HANDLE, GET_VIRTUAL_DISK_INFO_VERSION version, PULONG size,
                                          PGET_VIRTUAL_DISK_INFO info, PULONG) -> DWORD {
        if (version == GET_VIRTUAL_DISK_INFO_SIZE) {
            info->Size.VirtualSize = 1024;
            return ERROR_SUCCESS;
        }
        const std::size_t capacity = (*size - parent_header) / sizeof(wchar_t);
        auto* chars = static_cast<wchar_t*>(info->ParentLocation.ParentLocationBuffer);
        std::fill_n(chars, capacity, L'A');
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\wsl\\child.vhdx");
    REQUIRE(handle.has_value());

    const auto info = (*handle)->information();
    REQUIRE(info.has_value());
    const std::size_t capacity = (sizeof(GET_VIRTUAL_DISK_INFO) - parent_header) / sizeof(wchar_t);
    CHECK(info->parent_path == std::wstring(capacity, L'A'));
}

TEST_CASE("information reports a failed size query", "[platform][vdisk]") {
    const ScopedWin32Api scoped{opens_ok(ERROR_ACCESS_DENIED)};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
    REQUIRE(handle.has_value());

    const auto info = (*handle)->information();
    REQUIRE_FALSE(info.has_value());
    CHECK(info.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("compact drives the asynchronous loop to completion", "[platform][vdisk]") {
    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
    int polls = 0;
    api.get_virtual_disk_operation_progress = [&polls](HANDLE, LPOVERLAPPED,
                                                       PVIRTUAL_DISK_PROGRESS progress) -> DWORD {
        ++polls;
        progress->CompletionValue = 100;
        if (polls < 3) {
            progress->OperationStatus = ERROR_IO_PENDING;
            progress->CurrentValue = static_cast<ULONGLONG>(polls) * 25;
        } else {
            progress->OperationStatus = ERROR_SUCCESS;
            progress->CurrentValue = 100;
        }
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
    REQUIRE(handle.has_value());

    std::vector<DiskProgress> reported;
    const auto status = (*handle)->compact([&reported](const DiskProgress& p) {
        reported.push_back(p);
        return true;
    });

    REQUIRE(status.has_value());
    REQUIRE(reported.size() == 3);
    CHECK(reported.front().current == 25);
    // The last report is a full bar, so a caller never leaves one part-drawn.
    CHECK(reported.back().current == reported.back().total);
}

TEST_CASE("compact handles a synchronous completion", "[platform][vdisk]") {
    // A disk with nothing to reclaim can finish before the call returns.
    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_SUCCESS; };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
    REQUIRE(handle.has_value());

    int reports = 0;
    const auto status = (*handle)->compact([&reports](const DiskProgress&) {
        ++reports;
        return true;
    });

    REQUIRE(status.has_value());
    CHECK(reports == 1);
}

TEST_CASE("compact stops when the callback asks it to", "[platform][vdisk]") {
    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
    api.get_virtual_disk_operation_progress = [](HANDLE, LPOVERLAPPED,
                                                 PVIRTUAL_DISK_PROGRESS progress) -> DWORD {
        progress->OperationStatus = ERROR_IO_PENDING;
        progress->CurrentValue = 10;
        progress->CompletionValue = 100;
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
    REQUIRE(handle.has_value());

    const auto status = (*handle)->compact([](const DiskProgress&) { return false; });

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Partial);
    // Cancelling must not read as data loss.
    CHECK(status.error().remedy.find("still usable") != std::string::npos);
}

TEST_CASE("compact reports the ways it can fail", "[platform][vdisk]") {
    const Win32VirtualDisk disks;

    SECTION("the event cannot be created") {
        Win32Api api = opens_ok();
        api.create_event = [](LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR) -> HANDLE { return nullptr; };
        const ScopedWin32Api scoped{api};
        const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
        REQUIRE(handle.has_value());
        const auto status = (*handle)->compact([](const DiskProgress&) { return true; });
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("event") != std::string::npos);
    }

    SECTION("the call is refused outright") {
        const ScopedWin32Api scoped{opens_ok(ERROR_SHARING_VIOLATION)};
        const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
        REQUIRE(handle.has_value());
        const auto status = (*handle)->compact([](const DiskProgress&) { return true; });
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::DistroBusy);
    }

    SECTION("the wait fails") {
        Win32Api api = opens_ok();
        api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                      LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
        api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_FAILED; };
        const ScopedWin32Api scoped{api};
        const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
        REQUIRE(handle.has_value());
        const auto status = (*handle)->compact([](const DiskProgress&) { return true; });
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("wait") != std::string::npos);
    }

    SECTION("the progress query fails") {
        Win32Api api = opens_ok();
        api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                      LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
        api.get_virtual_disk_operation_progress = [](HANDLE, LPOVERLAPPED, PVIRTUAL_DISK_PROGRESS) -> DWORD {
            return ERROR_INVALID_HANDLE;
        };
        const ScopedWin32Api scoped{api};
        const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
        REQUIRE(handle.has_value());
        const auto status = (*handle)->compact([](const DiskProgress&) { return true; });
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("progress") != std::string::npos);
    }

    SECTION("the operation itself ends in an error") {
        Win32Api api = opens_ok();
        api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                      LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
        api.get_virtual_disk_operation_progress = [](HANDLE, LPOVERLAPPED,
                                                     PVIRTUAL_DISK_PROGRESS progress) -> DWORD {
            progress->OperationStatus = ERROR_DISK_FULL;
            return ERROR_SUCCESS;
        };
        const ScopedWin32Api scoped{api};
        const auto handle = disks.open("C:\\wsl\\ext4.vhdx");
        REQUIRE(handle.has_value());
        const auto status = (*handle)->compact([](const DiskProgress&) { return true; });
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::Preflight);
    }
}

TEST_CASE("create makes a disk with the requested maximum", "[platform][vdisk]") {
    ULONGLONG seen_maximum = 0;
    ULONG seen_version = 0;
    Win32Api api = opens_ok();
    api.create_virtual_disk = [&](PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK,
                                  PSECURITY_DESCRIPTOR, CREATE_VIRTUAL_DISK_FLAG, ULONG,
                                  PCREATE_VIRTUAL_DISK_PARAMETERS parameters, LPOVERLAPPED,
                                  PHANDLE handle) -> DWORD {
        seen_version = parameters->Version;
        seen_maximum = parameters->Version2.MaximumSize;
        *handle = reinterpret_cast<HANDLE>(3);
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto status = disks.create("C:\\temp\\test.vhdx", 64ULL * 1024 * 1024);

    REQUIRE(status.has_value());
    CHECK(seen_version == CREATE_VIRTUAL_DISK_VERSION_2);
    CHECK(seen_maximum == 64ULL * 1024 * 1024);
}

TEST_CASE("create reports a failure", "[platform][vdisk]") {
    const ScopedWin32Api scoped{all_failing(ERROR_DISK_FULL)};

    const Win32VirtualDisk disks;
    const auto status = disks.create("C:\\temp\\test.vhdx", 1024);

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Preflight);
    CHECK(status.error().remedy.find("free space") != std::string::npos);
}

// `CompactVirtualDisk` runs asynchronously against an OVERLAPPED and an event
// that both live on `compact`'s stack. Three paths used to return while the
// operation was still ERROR_IO_PENDING, leaving the kernel a pointer into a
// frame about to be reused and an event handle about to be closed. The tests
// below pin that every one of them now stops the operation first.

TEST_CASE("a cancelled compaction is stopped before compact returns", "[platform][vdisk]") {
    bool cancelled = false;
    bool pending = true;

    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
    api.get_virtual_disk_operation_progress = [&pending](HANDLE, LPOVERLAPPED,
                                                         PVIRTUAL_DISK_PROGRESS progress) -> DWORD {
        progress->OperationStatus = pending ? ERROR_IO_PENDING : ERROR_SUCCESS;
        progress->CurrentValue = 10;
        progress->CompletionValue = 100;
        return ERROR_SUCCESS;
    };
    api.cancel_io_ex = [&cancelled, &pending](HANDLE, LPOVERLAPPED) -> BOOL {
        cancelled = true;
        // The request is not the acknowledgement: the operation stops on the
        // next poll, not on the call.
        pending = false;
        return TRUE;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open(R"(C:\wsl\ext4.vhdx)");
    REQUIRE(handle.has_value());

    const auto status = (*handle)->compact([](const DiskProgress&) { return false; });

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Partial);
    CHECK(cancelled);
    CHECK_FALSE(pending);
}

TEST_CASE("a failed progress poll stops the compaction before returning", "[platform][vdisk]") {
    // Reachable today, unlike the cancellation path: nothing in the tool returns
    // false from the progress callback, but a Win32 failure part-way through a
    // real compaction is entirely ordinary.
    bool cancelled = false;

    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
    api.get_virtual_disk_operation_progress = [&cancelled](HANDLE, LPOVERLAPPED,
                                                           PVIRTUAL_DISK_PROGRESS) -> DWORD {
        // Fails the first time, then answers so the drain can finish.
        return cancelled ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
    };
    api.cancel_io_ex = [&cancelled](HANDLE, LPOVERLAPPED) -> BOOL {
        cancelled = true;
        return TRUE;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open(R"(C:\wsl\ext4.vhdx)");
    REQUIRE(handle.has_value());

    const auto status = (*handle)->compact([](const DiskProgress&) { return true; });

    REQUIRE_FALSE(status.has_value());
    CHECK(cancelled);
}

TEST_CASE("a failed wait stops the compaction before returning", "[platform][vdisk]") {
    bool cancelled = false;

    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
    api.wait_for_single_object = [&cancelled](HANDLE, DWORD) -> DWORD {
        // Fails the first time; the drain's own wait then succeeds.
        return cancelled ? WAIT_OBJECT_0 : WAIT_FAILED;
    };
    api.cancel_io_ex = [&cancelled](HANDLE, LPOVERLAPPED) -> BOOL {
        cancelled = true;
        return TRUE;
    };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open(R"(C:\wsl\ext4.vhdx)");
    REQUIRE(handle.has_value());

    const auto status = (*handle)->compact([](const DiskProgress&) { return true; });

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().message.find("wait for the compaction") != std::string::npos);
    CHECK(cancelled);
}

TEST_CASE("a compaction that will not stop is given up on rather than waited on forever",
          "[platform][vdisk]") {
    // The drain is bounded. A compaction that ignores CancelIoEx is a worse
    // problem than this code can solve, and hanging the tool is not a fix.
    int polls = 0;

    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_TIMEOUT; };
    api.get_virtual_disk_operation_progress = [&polls](HANDLE, LPOVERLAPPED,
                                                       PVIRTUAL_DISK_PROGRESS progress) -> DWORD {
        ++polls;
        progress->OperationStatus = ERROR_IO_PENDING;
        return ERROR_SUCCESS;
    };
    api.cancel_io_ex = [](HANDLE, LPOVERLAPPED) -> BOOL { return TRUE; };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open(R"(C:\wsl\ext4.vhdx)");
    REQUIRE(handle.has_value());

    const auto status = (*handle)->compact([](const DiskProgress&) { return false; });

    REQUIRE_FALSE(status.has_value());
    // It returned rather than spinning: bounded, not unbounded.
    CHECK(polls > 1);
}

TEST_CASE("the drain gives up when it cannot read progress either", "[platform][vdisk]") {
    // Cancelling asks; this is the case where nothing can confirm it stopped.
    // Returning is still right -- there is nothing further to try, and the
    // alternative is spinning until the bounded loop runs out.
    int polls = 0;

    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_TIMEOUT; };
    api.get_virtual_disk_operation_progress = [&polls](HANDLE, LPOVERLAPPED,
                                                       PVIRTUAL_DISK_PROGRESS progress) -> DWORD {
        ++polls;
        if (polls == 1) {
            // The compaction loop's own poll: still running, so the callback is
            // asked and says stop.
            progress->OperationStatus = ERROR_IO_PENDING;
            return ERROR_SUCCESS;
        }
        // The drain's poll, which cannot answer.
        return ERROR_ACCESS_DENIED;
    };
    api.cancel_io_ex = [](HANDLE, LPOVERLAPPED) -> BOOL { return TRUE; };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open(R"(C:\wsl\ext4.vhdx)");
    REQUIRE(handle.has_value());

    const auto status = (*handle)->compact([](const DiskProgress&) { return false; });

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Partial);
    // It stopped asking rather than exhausting the bound.
    CHECK(polls == 2);
}

TEST_CASE("the drain notices the operation stopped even if the event never signals", "[platform][vdisk]") {
    // The event is not the only way to learn it finished. If the wait keeps
    // timing out but the progress poll says the operation is no longer pending,
    // it has stopped and there is nothing left to wait for.
    int polls = 0;

    Win32Api api = opens_ok();
    api.compact_virtual_disk = [](HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS,
                                  LPOVERLAPPED) -> DWORD { return ERROR_IO_PENDING; };
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_TIMEOUT; };
    api.get_virtual_disk_operation_progress = [&polls](HANDLE, LPOVERLAPPED,
                                                       PVIRTUAL_DISK_PROGRESS progress) -> DWORD {
        ++polls;
        // Pending for the compaction loop's own poll, finished for the drain's.
        progress->OperationStatus = polls == 1 ? ERROR_IO_PENDING : ERROR_SUCCESS;
        return ERROR_SUCCESS;
    };
    api.cancel_io_ex = [](HANDLE, LPOVERLAPPED) -> BOOL { return TRUE; };
    const ScopedWin32Api scoped{api};

    const Win32VirtualDisk disks;
    const auto handle = disks.open(R"(C:\wsl\ext4.vhdx)");
    REQUIRE(handle.has_value());

    const auto status = (*handle)->compact([](const DiskProgress&) { return false; });

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Partial);
    // Stopped on the second poll rather than running the bound out.
    CHECK(polls == 2);
}
