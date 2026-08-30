#pragma once

#include <windows.h>
#include <virtdisk.h>

#include <functional>

namespace wsldisk::platform {

/// Indirection table for every Win32 function the platform layer calls.
///
/// Two reasons it exists: `platform/` stays the only place that includes
/// `<windows.h>`, and tests can swap the table to make any call fail with any
/// error code. That is what makes the 100% branch-coverage gate in
/// docs/TESTING.md reachable -- error paths that need a locked file, a full
/// volume or a denied ACL are exercised by injection instead of by fixture
/// gymnastics.
///
/// Signatures mirror the Win32 ones exactly so the real table is a plain
/// forwarding wrapper with no translation logic to get wrong.
struct Win32Api {
    std::function<DWORD(LPCWSTR file_name, LPDWORD file_size_high)> get_compressed_file_size;
    std::function<DWORD(LPCWSTR file_name)> get_file_attributes;
    std::function<BOOL(LPCWSTR file_name, GET_FILEEX_INFO_LEVELS info_level_id, LPVOID file_information)>
        get_file_attributes_ex;
    std::function<BOOL(LPCWSTR directory_name, PULARGE_INTEGER free_bytes_available_to_caller,
                       PULARGE_INTEGER total_number_of_bytes, PULARGE_INTEGER total_number_of_free_bytes)>
        get_disk_free_space_ex;
    std::function<BOOL(LPCWSTR root_path_name, LPWSTR volume_name_buffer, DWORD volume_name_size,
                       LPDWORD volume_serial_number, LPDWORD maximum_component_length,
                       LPDWORD file_system_flags, LPWSTR file_system_name_buffer,
                       DWORD file_system_name_size)>
        get_volume_information;
    std::function<BOOL(LPCWSTR file_name, LPWSTR volume_path_name, DWORD buffer_length)> get_volume_path_name;
    std::function<DWORD()> get_last_error;

    // Registry. These return an LSTATUS directly rather than setting the last
    // error, so their wrappers must not consult get_last_error.
    std::function<LSTATUS(HKEY key, LPCWSTR sub_key, DWORD options, REGSAM desired, PHKEY result)>
        reg_open_key_ex;
    std::function<LSTATUS(HKEY key)> reg_close_key;
    std::function<LSTATUS(HKEY key, DWORD index, LPWSTR name, LPDWORD name_length, LPDWORD reserved,
                          LPWSTR class_name, LPDWORD class_length, PFILETIME last_write_time)>
        reg_enum_key_ex;
    std::function<LSTATUS(HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                          LPDWORD data_length)>
        reg_query_value_ex;
    std::function<LSTATUS(HKEY key, LPCWSTR value_name, DWORD reserved, DWORD type, const BYTE* data,
                          DWORD data_length)>
        reg_set_value_ex;

    // Virtual Disk Service. Like the registry calls, these return an error code
    // rather than setting the last error.
    std::function<DWORD(PVIRTUAL_STORAGE_TYPE storage_type, PCWSTR path, VIRTUAL_DISK_ACCESS_MASK access_mask,
                        OPEN_VIRTUAL_DISK_FLAG flags, POPEN_VIRTUAL_DISK_PARAMETERS parameters,
                        PHANDLE handle)>
        open_virtual_disk;
    std::function<DWORD(HANDLE handle, GET_VIRTUAL_DISK_INFO_VERSION version, PULONG size,
                        PGET_VIRTUAL_DISK_INFO info, PULONG used_size)>
        get_virtual_disk_information;
    std::function<DWORD(HANDLE handle, COMPACT_VIRTUAL_DISK_FLAG flags,
                        PCOMPACT_VIRTUAL_DISK_PARAMETERS parameters, LPOVERLAPPED overlapped)>
        compact_virtual_disk;
    std::function<DWORD(HANDLE handle, LPOVERLAPPED overlapped, PVIRTUAL_DISK_PROGRESS progress)>
        get_virtual_disk_operation_progress;
    std::function<DWORD(PVIRTUAL_STORAGE_TYPE storage_type, PCWSTR path, VIRTUAL_DISK_ACCESS_MASK access_mask,
                        PSECURITY_DESCRIPTOR security_descriptor, CREATE_VIRTUAL_DISK_FLAG flags,
                        ULONG provider_flags, PCREATE_VIRTUAL_DISK_PARAMETERS parameters,
                        LPOVERLAPPED overlapped, PHANDLE handle)>
        create_virtual_disk;

    std::function<BOOL(HANDLE handle)> close_handle;
    std::function<HANDLE(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state,
                         LPCWSTR name)>
        create_event;
    std::function<DWORD(HANDLE handle, DWORD milliseconds)> wait_for_single_object;
};

/// The table that forwards to the real Win32 entry points.
[[nodiscard]] const Win32Api& real_win32_api();

/// The table the platform layer is currently using -- the real one unless a
/// `ScopedWin32Api` is in scope on this thread.
[[nodiscard]] const Win32Api& win32();

/// Installs a replacement table for the lifetime of the object. Test-only, and
/// deliberately not thread-safe: the override is process-wide, so tests that use
/// it must not run concurrently with other tests that touch `platform/`.
class ScopedWin32Api {
public:
    explicit ScopedWin32Api(Win32Api replacement);
    ~ScopedWin32Api();

    ScopedWin32Api(const ScopedWin32Api&) = delete;
    ScopedWin32Api& operator=(const ScopedWin32Api&) = delete;
    ScopedWin32Api(ScopedWin32Api&&) = delete;
    ScopedWin32Api& operator=(ScopedWin32Api&&) = delete;

private:
    const Win32Api* previous_;
    Win32Api replacement_;
};

}  // namespace wsldisk::platform
