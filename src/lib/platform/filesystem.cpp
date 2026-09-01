#include "filesystem.h"

#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves out the ioctl definitions, and
// FSCTL_QUERY_ALLOCATED_RANGES is one of them.
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cwctype>
#include <format>
#include <functional>
#include <numeric>
#include <ranges>
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

/// How much a sparse copy moves per read/write pair.
///
/// One megabyte: large enough that the per-call overhead disappears against the
/// I/O, small enough that a cancelled copy stops promptly and that the buffer is
/// not worth worrying about. It also matches the VHDX block size, so a chunk
/// rarely straddles two blocks.
constexpr std::size_t copy_chunk_bytes = std::size_t{1024} * 1024;

/// Moves a handle's file pointer to an absolute offset.
[[nodiscard]] Status seek(HANDLE file, std::uint64_t offset, const std::filesystem::path& path) {
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(offset);
    if (win32().set_file_pointer_ex(file, distance, nullptr, FILE_BEGIN) == FALSE) {
        return std::unexpected(error_from_win32(win32().get_last_error(),
                                                std::format("seek in {} to {}", path.string(), offset)));
    }
    return {};
}

/// The handles and paths one range copy needs.
///
/// A struct rather than nine parameters: the copy loop moved out of
/// `copy_file_sparse` to keep that function readable, and passing its whole
/// world through an argument list would have traded one problem for another.
struct CopyJob {
    HANDLE source = nullptr;
    HANDLE destination = nullptr;
    std::span<std::byte> buffer;
    const std::filesystem::path* from = nullptr;
    const std::filesystem::path* to = nullptr;
    /// Allocated bytes in the whole file, for the progress denominator.
    std::uint64_t total = 0;
    /// Allocated bytes copied so far, across every range.
    std::uint64_t copied = 0;
};

/// Gives a file its final logical length before anything is written into it.
///
/// On a sparse file this costs nothing: the length is metadata and the holes
/// stay holes. It is what makes the copy the same size as the original even
/// though only the allocated ranges are written.
[[nodiscard]] Status resize(HANDLE file, std::uint64_t length, const std::filesystem::path& path) {
    if (const Status sought = seek(file, length, path); !sought.has_value()) {
        return sought;
    }
    if (win32().set_end_of_file(file) == FALSE) {
        return std::unexpected(error_from_win32(
            win32().get_last_error(), std::format("set the length of {} to {}", path.string(), length)));
    }
    return {};
}

/// Copies one allocated range, a chunk at a time.
///
/// Seeks both handles for every chunk rather than relying on the pointers having
/// been left where the last read finished: the ranges are not contiguous, and a
/// copy that assumed they were would write the second island over the hole after
/// the first.
[[nodiscard]] Status copy_range(CopyJob& job, const AllocatedRange& range,
                                const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    std::uint64_t offset = range.offset;
    const std::uint64_t end = range.offset + range.length;
    while (offset < end) {
        const auto chunk = static_cast<DWORD>(std::min<std::uint64_t>(end - offset, job.buffer.size()));
        if (const Status sought = seek(job.source, offset, *job.from); !sought.has_value()) {
            return sought;
        }
        if (const Status sought = seek(job.destination, offset, *job.to); !sought.has_value()) {
            return sought;
        }

        DWORD read = 0;
        if (win32().read_file(job.source, job.buffer.data(), chunk, &read, nullptr) == FALSE) {
            return std::unexpected(
                error_from_win32(win32().get_last_error(), std::format("read {}", job.from->string())));
        }
        // A short read inside a range the filesystem just said was allocated
        // means the file changed underneath us. Carrying on would write a copy
        // that is quietly not the original.
        if (read == 0) {
            return fail(ErrorCode::Generic, std::format("{} ended earlier than expected", job.from->string()),
                        "something else is writing to the file; stop it and try again");
        }

        DWORD written = 0;
        if (win32().write_file(job.destination, job.buffer.data(), read, &written, nullptr) == FALSE) {
            return std::unexpected(
                error_from_win32(win32().get_last_error(), std::format("write {}", job.to->string())));
        }
        if (written != read) {
            return fail(ErrorCode::Generic, std::format("{} accepted only part of a write", job.to->string()),
                        "the volume may be full; check the free space and try again");
        }

        offset += read;
        job.copied += read;
        if (!progress(job.copied, job.total)) {
            return fail(ErrorCode::Partial, std::format("copying {} was cancelled", job.from->string()),
                        std::format("{} is a partial copy and can be deleted", job.to->string()));
        }
    }
    return {};
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
    if (win32().get_file_attributes(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }

    // `GetFileAttributesW` fails for reasons other than absence: no traverse
    // right on a parent directory, a delete pending, an unavailable device. All
    // of those used to read as "does not exist", so `relink` told a user whose
    // parent directory denies traverse that their disk was not there and
    // suggested they check the path -- sending them to hunt a typo when the
    // answer was permissions.
    //
    // Only the codes that genuinely mean absent are treated as absent. Anything
    // else answers "not known to be missing", so the caller proceeds and fails
    // at the real operation with the real Win32 error, which is a diagnosis
    // rather than a guess.
    switch (win32().get_last_error()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
            return false;
        default:
            return true;
    }
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

Result<bool> Win32FileSystem::is_locked(const std::filesystem::path& path) const {
    // Asking for exclusive access is the only reliable way to find out: a file
    // another process has open for writing refuses to open with no sharing.
    const ScopedHandle file{win32().create_file(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                                FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.get() != INVALID_HANDLE_VALUE) {
        return false;
    }

    const DWORD status = win32().get_last_error();
    if (status == ERROR_SHARING_VIOLATION || status == ERROR_LOCK_VIOLATION) {
        return true;
    }
    // Anything else -- gone, denied, on a drive that went away -- is not an
    // answer to the question that was asked.
    return std::unexpected(
        error_from_win32(status, std::format("check whether {} is in use", path.string())));
}

Status Win32FileSystem::copy_file_sparse(
    const std::filesystem::path& from, const std::filesystem::path& to,
    const std::function<bool(std::uint64_t copied, std::uint64_t total)>& progress) {
    // The ranges drive the whole copy: they are what to read, what to write and
    // what the progress is measured against. Asking first also fails early on a
    // source that cannot be read at all.
    const auto ranges = allocated_ranges(from);
    if (!ranges.has_value()) {
        return std::unexpected(ranges.error());
    }
    const auto length = file_size(from);
    if (!length.has_value()) {
        return std::unexpected(length.error());
    }
    const std::uint64_t total =
        std::accumulate(ranges->begin(), ranges->end(), std::uint64_t{0},
                        [](std::uint64_t sum, const AllocatedRange& range) { return sum + range.length; });

    const ScopedHandle source{win32().create_file(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (source.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("open {}", from.string())));
    }

    // CREATE_NEW, so an existing destination is an error rather than something
    // silently written over. A half-finished copy from a previous attempt is
    // exactly the file most worth not clobbering.
    const ScopedHandle destination{win32().create_file(to.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (destination.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("create {}", to.string())));
    }

    // Sparse before anything is written. Setting it afterwards would not give
    // back the space the writes had already committed.
    DWORD returned = 0;
    if (win32().device_io_control(destination.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                                  nullptr) == FALSE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("make {} a sparse file", to.string())));
    }

    // The logical length, holes included, so the copy is the same size as the
    // original even though only the allocated ranges get written.
    if (const Status sized = resize(destination.get(), *length, to); !sized.has_value()) {
        return sized;
    }

    std::vector<std::byte> buffer(copy_chunk_bytes);
    CopyJob job{.source = source.get(),
                .destination = destination.get(),
                .buffer = buffer,
                .from = &from,
                .to = &to,
                .total = total,
                .copied = 0};
    for (const AllocatedRange& range : *ranges) {
        if (const Status done = copy_range(job, range, progress); !done.has_value()) {
            return done;
        }
    }
    return {};
}

Status Win32FileSystem::rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    // No MOVEFILE_COPY_ALLOWED: across volumes Windows would fall back to a copy
    // that does not preserve the holes, filling them in on a disk WSL created
    // sparse. Let it fail, and let the caller copy properly.
    if (win32().move_file_ex(from.c_str(), to.c_str(), 0) == FALSE) {
        return std::unexpected(error_from_win32(win32().get_last_error(),
                                                std::format("rename {} to {}", from.string(), to.string())));
    }
    return {};
}

Result<bool> Win32FileSystem::same_volume(const std::filesystem::path& first,
                                          const std::filesystem::path& second) const {
    const auto root_of = [](const std::filesystem::path& path) -> Result<std::wstring> {
        std::array<wchar_t, MAX_PATH + 1> root{};
        if (win32().get_volume_path_name(path.c_str(), root.data(), static_cast<DWORD>(root.size())) ==
            FALSE) {
            return std::unexpected(error_from_win32(
                win32().get_last_error(), std::format("resolve the volume holding {}", path.string())));
        }
        return std::wstring{root.data()};
    };

    const auto first_root = root_of(first);
    if (!first_root.has_value()) {
        return std::unexpected(first_root.error());
    }
    const auto second_root = root_of(second);
    if (!second_root.has_value()) {
        return std::unexpected(second_root.error());
    }
    // Volume paths come back with consistent casing from the same API, but the
    // filesystem does not care about case and neither should this.
    return std::ranges::equal(*first_root, *second_root, [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    });
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

Result<std::string> Win32FileSystem::read_text_file(const std::filesystem::path& path) const {
    const ScopedHandle file{win32().create_file(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("open {}", path.string())));
    }

    std::string contents;
    std::array<char, 4096> buffer{};
    while (true) {  // LCOV_EXCL_BR_LINE -- every exit is a return or a break
        DWORD read = 0;
        if (win32().read_file(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ==
            FALSE) {
            return std::unexpected(
                error_from_win32(win32().get_last_error(), std::format("read {}", path.string())));
        }
        if (read == 0) {
            break;
        }
        contents.append(buffer.data(), read);
    }
    return contents;
}

Status Win32FileSystem::write_text_file(const std::filesystem::path& path, std::string_view contents) {
    // CREATE_ALWAYS rather than TRUNCATE_EXISTING: writing a config for the
    // first time is the ordinary case, not an error.
    const ScopedHandle file{win32().create_file(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("create {}", path.string())));
    }

    // One call: these files are a few hundred bytes, and a partial write would
    // leave a config that parses to something the user did not ask for.
    DWORD written = 0;
    if (win32().write_file(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &written,
                           nullptr) == FALSE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("write {}", path.string())));
    }
    if (written != contents.size()) {
        return fail(ErrorCode::Partial, std::format("{} was only partly written", path.string()),
                    "check there is free space on the volume and try again");
    }
    return {};
}

Status Win32FileSystem::create_directories(const std::filesystem::path& path) {
    // Walked into a list rather than recursed: the depth is bounded by the path,
    // but a reader has to prove that either way, and an explicit list says it.
    //
    // The drive root is never in the list. It exists by definition, and
    // `CreateDirectoryW("C:\\")` fails with ERROR_ACCESS_DENIED rather than
    // ERROR_ALREADY_EXISTS -- which would look like a real failure and sink
    // every call that reached it.
    std::vector<std::filesystem::path> levels;
    for (std::filesystem::path level = path; !level.empty() && level != level.root_path();
         level = level.parent_path()) {
        levels.push_back(level);
        if (!level.has_parent_path()) {
            break;
        }
    }

    // Parents first, so a config directory two levels below %APPDATA% works.
    for (const std::filesystem::path& level : std::views::reverse(levels)) {
        if (win32().create_directory(level.c_str(), nullptr) != FALSE) {
            continue;
        }
        // Already there is the ordinary case, not a failure: this is `mkdir -p`.
        if (const DWORD status = win32().get_last_error(); status != ERROR_ALREADY_EXISTS) {
            return std::unexpected(error_from_win32(status, std::format("create {}", level.string())));
        }
    }
    return {};
}
}  // namespace wsldisk::platform
