#include "win32_api.h"

#include <utility>

namespace wsldisk::platform {
namespace {

const Win32Api* g_active = nullptr;

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
    };
    return table;
}

const Win32Api& win32() {
    return g_active != nullptr ? *g_active : real_win32_api();
}

ScopedWin32Api::ScopedWin32Api(Win32Api replacement)
    : previous_(g_active), replacement_(std::move(replacement)) {
    g_active = &replacement_;
}

ScopedWin32Api::~ScopedWin32Api() {
    g_active = previous_;
}

}  // namespace wsldisk::platform
