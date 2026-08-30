#include "win32_api.h"

#include <utility>

namespace wsldisk::platform {
namespace {

/// The table currently installed by a `ScopedWin32Api`, or null for the real one.
///
/// A function-local static rather than a namespace-scope variable: it has no
/// static initialisation order to get wrong, and mutable global state is
/// something to keep deliberately narrow.
const Win32Api*& active_table() {
    static const Win32Api* table = nullptr;
    return table;
}

}  // namespace

const Win32Api& real_win32_api() {
    static const Win32Api table{
        .get_compressed_file_size =
            [](LPCWSTR file_name, LPDWORD file_size_high) {
                return ::GetCompressedFileSizeW(file_name, file_size_high);
            },
        .get_file_attributes = [](LPCWSTR file_name) { return ::GetFileAttributesW(file_name); },
        .get_file_attributes_ex =
            [](LPCWSTR file_name, GET_FILEEX_INFO_LEVELS info_level_id, LPVOID file_information) {
                return ::GetFileAttributesExW(file_name, info_level_id, file_information);
            },
        .get_disk_free_space_ex =
            [](LPCWSTR directory_name, PULARGE_INTEGER free_to_caller, PULARGE_INTEGER total_bytes,
               PULARGE_INTEGER total_free) {
                return ::GetDiskFreeSpaceExW(directory_name, free_to_caller, total_bytes, total_free);
            },
        .get_volume_information =
            [](LPCWSTR root_path_name, LPWSTR volume_name_buffer, DWORD volume_name_size,
               LPDWORD volume_serial_number, LPDWORD maximum_component_length, LPDWORD file_system_flags,
               LPWSTR file_system_name_buffer, DWORD file_system_name_size) {
                return ::GetVolumeInformationW(root_path_name, volume_name_buffer, volume_name_size,
                                               volume_serial_number, maximum_component_length,
                                               file_system_flags, file_system_name_buffer,
                                               file_system_name_size);
            },
        .get_volume_path_name =
            [](LPCWSTR file_name, LPWSTR volume_path_name, DWORD buffer_length) {
                return ::GetVolumePathNameW(file_name, volume_path_name, buffer_length);
            },
        .get_last_error = []() { return ::GetLastError(); },
        .reg_open_key_ex =
            [](HKEY key, LPCWSTR sub_key, DWORD options, REGSAM desired, PHKEY result) {
                return ::RegOpenKeyExW(key, sub_key, options, desired, result);
            },
        .reg_close_key = [](HKEY key) { return ::RegCloseKey(key); },
        .reg_enum_key_ex =
            [](HKEY key, DWORD index, LPWSTR name, LPDWORD name_length, LPDWORD reserved, LPWSTR class_name,
               LPDWORD class_length, PFILETIME last_write_time) {
                return ::RegEnumKeyExW(key, index, name, name_length, reserved, class_name, class_length,
                                       last_write_time);
            },
        .reg_query_value_ex =
            [](HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type, LPBYTE data,
               LPDWORD data_length) {
                return ::RegQueryValueExW(key, value_name, reserved, type, data, data_length);
            },
        .reg_set_value_ex =
            [](HKEY key, LPCWSTR value_name, DWORD reserved, DWORD type, const BYTE* data,
               DWORD data_length) {
                return ::RegSetValueExW(key, value_name, reserved, type, data, data_length);
            },
        .open_virtual_disk =
            [](PVIRTUAL_STORAGE_TYPE storage_type, PCWSTR path, VIRTUAL_DISK_ACCESS_MASK access_mask,
               OPEN_VIRTUAL_DISK_FLAG flags, POPEN_VIRTUAL_DISK_PARAMETERS parameters, PHANDLE handle) {
                return ::OpenVirtualDisk(storage_type, path, access_mask, flags, parameters, handle);
            },
        .get_virtual_disk_information =
            [](HANDLE handle, GET_VIRTUAL_DISK_INFO_VERSION version, PULONG size, PGET_VIRTUAL_DISK_INFO info,
               PULONG used_size) {
                // GetVirtualDiskInformation reads the version out of the struct.
                // Taking it as a parameter keeps the table signature explicit
                // about what is being asked for.
                info->Version = version;
                // size and used_size are in the order the API declares them.
                // NOLINTNEXTLINE(readability-suspicious-call-argument)
                return ::GetVirtualDiskInformation(handle, size, info, used_size);
            },
        .compact_virtual_disk =
            [](HANDLE handle, COMPACT_VIRTUAL_DISK_FLAG flags, PCOMPACT_VIRTUAL_DISK_PARAMETERS parameters,
               LPOVERLAPPED overlapped) {
                return ::CompactVirtualDisk(handle, flags, parameters, overlapped);
            },
        .get_virtual_disk_operation_progress =
            [](HANDLE handle, LPOVERLAPPED overlapped, PVIRTUAL_DISK_PROGRESS progress) {
                return ::GetVirtualDiskOperationProgress(handle, overlapped, progress);
            },
        .create_virtual_disk =
            [](PVIRTUAL_STORAGE_TYPE storage_type, PCWSTR path, VIRTUAL_DISK_ACCESS_MASK access_mask,
               PSECURITY_DESCRIPTOR security_descriptor, CREATE_VIRTUAL_DISK_FLAG flags, ULONG provider_flags,
               PCREATE_VIRTUAL_DISK_PARAMETERS parameters, LPOVERLAPPED overlapped, PHANDLE handle) {
                return ::CreateVirtualDisk(storage_type, path, access_mask, security_descriptor, flags,
                                           provider_flags, parameters, overlapped, handle);
            },
        .close_handle = [](HANDLE handle) { return ::CloseHandle(handle); },
        .create_event =
            [](LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state, LPCWSTR name) {
                return ::CreateEventW(attributes, manual_reset, initial_state, name);
            },
        .wait_for_single_object =
            [](HANDLE handle, DWORD milliseconds) { return ::WaitForSingleObject(handle, milliseconds); },
    };
    return table;
}

const Win32Api& win32() {
    const Win32Api* const active = active_table();
    return active != nullptr ? *active : real_win32_api();
}

ScopedWin32Api::ScopedWin32Api(Win32Api replacement)
    : previous_(active_table()), replacement_(std::move(replacement)) {
    active_table() = &replacement_;
}

ScopedWin32Api::~ScopedWin32Api() {
    active_table() = previous_;
}

}  // namespace wsldisk::platform
