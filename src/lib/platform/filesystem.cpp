#include "filesystem.h"

#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves out the ioctl definitions, and
// FSCTL_QUERY_ALLOCATED_RANGES is one of them.
#include <winioctl.h>

#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scoped_handle.h"
#include "win32_api.h"
#include "win32_error.h"

namespace wsldisk::platform {
namespace {

/// Joins the high/low halves Win32 hands back for 64-bit file sizes.
[[nodiscard]] std::uint64_t combine(DWORD high, DWORD low) noexcept {
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

/// Closes a FindFirstFileEx search through the injection table.
///
/// Not ScopedHandle: a search handle is closed with FindClose, and an unused
/// one is INVALID_HANDLE_VALUE rather than null.
class ScopedFindHandle {
public:
    explicit ScopedFindHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedFindHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            std::ignore = win32().find_close(handle_);
        }
    }

    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;
    ScopedFindHandle(ScopedFindHandle&&) = delete;
    ScopedFindHandle& operator=(ScopedFindHandle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

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
    if (win32().get_file_attributes_ex(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
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
    if (win32().get_volume_path_name(path.c_str(), volume_root.data(),
                                     static_cast<DWORD>(volume_root.size())) == FALSE) {
        return std::unexpected(error_from_win32(win32().get_last_error(),
                                                std::format("resolve the volume holding {}", path.string())));
    }

    ULARGE_INTEGER free_to_caller{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER total_free{};
    if (win32().get_disk_free_space_ex(volume_root.data(), &free_to_caller, &total, &total_free) == FALSE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(),
                             std::format("read free space on {}", ascii_narrow(volume_root.data()))));
    }

    std::array<wchar_t, MAX_PATH + 1> filesystem_name{};
    if (win32().get_volume_information(volume_root.data(), nullptr, 0, nullptr, nullptr, nullptr,
                                       filesystem_name.data(),
                                       static_cast<DWORD>(filesystem_name.size())) == FALSE) {
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

Result<std::vector<DirectoryEntry>> Win32FileSystem::list_directory(const std::filesystem::path& directory,
                                                                    std::wstring_view pattern) const {
    const std::filesystem::path query = directory / pattern;

    WIN32_FIND_DATAW found{};
    // FindExInfoBasic skips the 8.3 short name, which nothing here reads and
    // which the filesystem would otherwise have to look up per entry.
    const ScopedFindHandle search{win32().find_first_file_ex(query.c_str(), FindExInfoBasic, &found,
                                                             FindExSearchNameMatch, nullptr, 0)};
    if (search.get() == INVALID_HANDLE_VALUE) {
        const DWORD status = win32().get_last_error();
        // An empty directory, or one where nothing matches, is an answer rather
        // than a failure -- `orphans` scans several directories and most hold
        // nothing.
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_NO_MORE_FILES) {
            return std::vector<DirectoryEntry>{};
        }
        return std::unexpected(error_from_win32(status, std::format("list {}", directory.string())));
    }

    std::vector<DirectoryEntry> entries;
    BOOL another = TRUE;
    while (another != FALSE) {
        const std::wstring_view name{static_cast<const wchar_t*>(found.cFileName)};
        // `.` and `..` are not entries anyone asked for.
        if (name != L"." && name != L"..") {
            const bool is_directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entries.push_back(
                DirectoryEntry{.path = directory / name,
                               .size = is_directory ? 0 : combine(found.nFileSizeHigh, found.nFileSizeLow),
                               .is_directory = is_directory});
        }
        another = win32().find_next_file(search.get(), &found);
    }

    // Reaching the end of the listing is ERROR_NO_MORE_FILES; anything else --
    // a drive pulled mid-scan -- must not look like a complete answer.
    if (const DWORD status = win32().get_last_error(); status != ERROR_NO_MORE_FILES) {
        return std::unexpected(
            error_from_win32(status, std::format("finish listing {}", directory.string())));
    }
    return entries;
}

Result<std::vector<AllocatedRange>> Win32FileSystem::allocated_ranges(
    const std::filesystem::path& path) const {
    const auto length = file_size(path);
    if (!length.has_value()) {
        return std::unexpected(length.error());
    }
    // An empty file occupies nothing, and asking the filesystem about a
    // zero-length span is a call with no answer to give.
    if (*length == 0) {
        return std::vector<AllocatedRange>{};
    }

    const ScopedHandle file{win32().create_file(path.c_str(), GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("open {}", path.string())));
    }

    std::vector<AllocatedRange> ranges;
    FILE_ALLOCATED_RANGE_BUFFER query{};
    query.FileOffset.QuadPart = 0;
    query.Length.QuadPart = static_cast<LONGLONG>(*length);

    // The ioctl answers as much as fits and reports ERROR_MORE_DATA, so the
    // query restarts after the last range it managed to return. The cursor is
    // unsigned and the query is rebuilt from it each pass, rather than the
    // signed LARGE_INTEGER being both the state and the loop bound.
    std::uint64_t offset = 0;
    while (offset < *length) {
        query.FileOffset.QuadPart = static_cast<LONGLONG>(offset);
        query.Length.QuadPart = static_cast<LONGLONG>(*length - offset);
        std::array<FILE_ALLOCATED_RANGE_BUFFER, 64> answer{};
        DWORD returned = 0;
        const BOOL complete =
            win32().device_io_control(file.get(), FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query),
                                      answer.data(), static_cast<DWORD>(sizeof(answer)), &returned, nullptr);
        const DWORD status = complete != FALSE ? ERROR_SUCCESS : win32().get_last_error();
        if (complete == FALSE && status != ERROR_MORE_DATA) {
            return std::unexpected(
                error_from_win32(status, std::format("read the allocated ranges of {}", path.string())));
        }

        const std::span answered{answer.data(), returned / sizeof(FILE_ALLOCATED_RANGE_BUFFER)};
        for (const FILE_ALLOCATED_RANGE_BUFFER& entry : answered) {
            ranges.push_back(AllocatedRange{.offset = static_cast<std::uint64_t>(entry.FileOffset.QuadPart),
                                            .length = static_cast<std::uint64_t>(entry.Length.QuadPart)});
        }
        if (status != ERROR_MORE_DATA) {
            break;
        }
        // Nothing came back but more was promised: continuing would spin.
        if (answered.empty()) {
            break;
        }
        const FILE_ALLOCATED_RANGE_BUFFER& last = answered.back();
        offset = static_cast<std::uint64_t>(last.FileOffset.QuadPart) +
                 static_cast<std::uint64_t>(last.Length.QuadPart);
    }
    return ranges;
}

Status Win32FileSystem::remove(const std::filesystem::path& path) {
    if (win32().delete_file(path.c_str()) == FALSE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("delete {}", path.string())));
    }
    return {};
}

Result<std::filesystem::path> Win32FileSystem::expand_environment(const std::filesystem::path& path) const {
    const std::wstring source = path.wstring();
    // The first call sizes the result, including the terminator.
    const DWORD needed = win32().expand_environment_strings(source.c_str(), nullptr, 0);
    if (needed == 0) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("expand {}", path.string())));
    }

    std::wstring expanded(needed, L'\0');
    const DWORD written = win32().expand_environment_strings(source.c_str(), expanded.data(), needed);
    if (written == 0) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("expand {}", path.string())));
    }
    // The count includes the terminator, which does not belong in the path.
    expanded.resize(written - 1);
    return std::filesystem::path{expanded};
}
}  // namespace wsldisk::platform
