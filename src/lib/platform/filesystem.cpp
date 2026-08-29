#include "filesystem.h"

#include <windows.h>

#include <array>
#include <format>
#include <string_view>

#include "win32_api.h"
#include "win32_error.h"

namespace wsldisk::platform {
namespace {

/// Joins the high/low halves Win32 hands back for 64-bit file sizes.
[[nodiscard]] std::uint64_t combine(DWORD high, DWORD low) noexcept {
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

/// Filesystem and volume names are ASCII in practice; anything else is replaced
/// rather than guessed at, so the value stays safe to print and compare.
[[nodiscard]] std::string ascii_narrow(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        result.push_back(character < 0x80 ? static_cast<char>(character) : '?');
    }
    return result;
}

}  // namespace

bool Win32FileSystem::exists(const std::filesystem::path& path) const {
    return win32().get_file_attributes(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

Result<std::uint64_t> Win32FileSystem::file_size(const std::filesystem::path& path) const {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!win32().get_file_attributes_ex(path.c_str(), GetFileExInfoStandard, &data)) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("read the size of {}", path.string())));
    }
    return combine(data.nFileSizeHigh, data.nFileSizeLow);
}

Result<std::uint64_t> Win32FileSystem::file_size_on_disk(const std::filesystem::path& path) const {
    DWORD high = 0;
    const DWORD low = win32().get_compressed_file_size(path.c_str(), &high);
    // INVALID_FILE_SIZE is a legitimate low word for a 4 GiB-aligned file, so the
    // only way to tell an error apart is to consult GetLastError.
    if (low == INVALID_FILE_SIZE) {
        const DWORD code = win32().get_last_error();
        if (code != NO_ERROR) {
            return std::unexpected(
                error_from_win32(code, std::format("read the on-disk size of {}", path.string())));
        }
    }
    return combine(high, low);
}

Result<bool> Win32FileSystem::is_sparse(const std::filesystem::path& path) const {
    const DWORD attributes = win32().get_file_attributes(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(error_from_win32(win32().get_last_error(),
                                                std::format("read the attributes of {}", path.string())));
    }
    return (attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
}

Result<VolumeInfo> Win32FileSystem::volume_info(const std::filesystem::path& path) const {
    // An extended-length path (\\?\D:\wsl\ext4.vhdx) or a mounted-folder path only resolves
    // to the right volume through GetVolumePathName; never assume `root_path()`.
    std::array<wchar_t, MAX_PATH + 1> volume_root{};
    if (!win32().get_volume_path_name(path.c_str(), volume_root.data(),
                                      static_cast<DWORD>(volume_root.size()))) {
        return std::unexpected(error_from_win32(win32().get_last_error(),
                                                std::format("resolve the volume holding {}", path.string())));
    }

    ULARGE_INTEGER free_to_caller{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER total_free{};
    if (!win32().get_disk_free_space_ex(volume_root.data(), &free_to_caller, &total, &total_free)) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(),
                             std::format("read free space on {}", ascii_narrow(volume_root.data()))));
    }

    std::array<wchar_t, MAX_PATH + 1> filesystem_name{};
    if (!win32().get_volume_information(volume_root.data(), nullptr, 0, nullptr, nullptr, nullptr,
                                        filesystem_name.data(), static_cast<DWORD>(filesystem_name.size()))) {
        return std::unexpected(error_from_win32(
            win32().get_last_error(),
            std::format("read volume information for {}", ascii_narrow(volume_root.data()))));
    }

    return VolumeInfo{
        .filesystem_name = ascii_narrow(filesystem_name.data()),
        .total_bytes = total.QuadPart,
        .free_bytes = free_to_caller.QuadPart,
    };
}

}  // namespace wsldisk::platform
