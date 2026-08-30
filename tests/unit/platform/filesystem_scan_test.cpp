// Unit tests for the M1 additions to Win32FileSystem: directory scanning,
// allocated ranges, deletion and environment expansion. The M0 surface is
// covered in filesystem_test.cpp.

#include <windows.h>
#include <winioctl.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "errors.h"
#include "platform/filesystem.h"
#include "platform/win32_api.h"

using wsldisk::ErrorCode;
using wsldisk::platform::ScopedWin32Api;
using wsldisk::platform::Win32Api;
using wsldisk::platform::Win32FileSystem;

namespace {

const auto search_handle = reinterpret_cast<HANDLE>(0x40);
const auto file_handle = reinterpret_cast<HANDLE>(0x41);

/// One entry a fake FindFirstFileEx/FindNextFile pair will hand back.
struct Entry {
    std::wstring name;
    std::uint64_t size = 0;
    bool is_directory = false;
};

/// Copies a name into a fixed WIN32_FIND_DATAW field. Not wcsncpy: MSVC
/// deprecates it and /WX turns the warning into a build failure.
void set_name(wchar_t* field, std::wstring_view name) {
    const std::size_t count = std::min<std::size_t>(name.size(), MAX_PATH - 1);
    std::copy_n(name.begin(), count, field);
    field[count] = L'\0';
}

void fill(LPWIN32_FIND_DATAW data, const Entry& entry) {
    *data = WIN32_FIND_DATAW{};
    set_name(static_cast<wchar_t*>(data->cFileName), entry.name);
    data->nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);
    data->nFileSizeLow = static_cast<DWORD>(entry.size & 0xFFFFFFFF);
    data->dwFileAttributes = entry.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

/// A table that enumerates `entries` and then reports the end of the listing.
///
/// By value for the same reason as `ranges_of`: the table outlives the call.
Win32Api listing(std::vector<Entry> entries, std::shared_ptr<std::size_t> position) {
    Win32Api api;
    api.find_first_file_ex = [entries, position](LPCWSTR, FINDEX_INFO_LEVELS, LPVOID data, FINDEX_SEARCH_OPS,
                                                 LPVOID, DWORD) -> HANDLE {
        *position = 0;
        fill(static_cast<LPWIN32_FIND_DATAW>(data), entries.front());
        return search_handle;
    };
    api.find_next_file = [entries = std::move(entries), position](HANDLE, LPWIN32_FIND_DATAW data) -> BOOL {
        if (++*position >= entries.size()) {
            return FALSE;
        }
        fill(data, entries[*position]);
        return TRUE;
    };
    api.find_close = [](HANDLE) -> BOOL { return TRUE; };
    api.get_last_error = []() -> DWORD { return ERROR_NO_MORE_FILES; };
    return api;
}

}  // namespace

TEST_CASE("list_directory returns matching files with their sizes", "[platform][fs]") {
    const std::vector<Entry> entries{{.name = L"ext4.vhdx", .size = 1024},
                                     {.name = L"other.vhdx", .size = 2048}};
    const auto position = std::make_shared<std::size_t>(0);
    const ScopedWin32Api scoped{listing(entries, position)};

    const Win32FileSystem fs;
    const auto found = fs.list_directory("C:\\wsl", L"*.vhdx");

    REQUIRE(found.has_value());
    REQUIRE(found->size() == 2);
    CHECK((*found)[0].path == std::filesystem::path{"C:\\wsl\\ext4.vhdx"});
    CHECK((*found)[0].size == 1024);
    CHECK_FALSE((*found)[0].is_directory);
    CHECK((*found)[1].size == 2048);
}

TEST_CASE("list_directory skips the dot entries", "[platform][fs]") {
    // Every directory has them and nobody asked for them.
    const std::vector<Entry> entries{{.name = L".", .is_directory = true},
                                     {.name = L"..", .is_directory = true},
                                     {.name = L"ext4.vhdx", .size = 7}};
    const auto position = std::make_shared<std::size_t>(0);
    const ScopedWin32Api scoped{listing(entries, position)};

    const Win32FileSystem fs;
    const auto found = fs.list_directory("C:\\wsl", L"*");

    REQUIRE(found.has_value());
    REQUIRE(found->size() == 1);
    CHECK((*found)[0].path.filename() == "ext4.vhdx");
}

TEST_CASE("list_directory reports a directory with no size", "[platform][fs]") {
    const std::vector<Entry> entries{{.name = L"nested", .size = 99, .is_directory = true}};
    const auto position = std::make_shared<std::size_t>(0);
    const ScopedWin32Api scoped{listing(entries, position)};

    const Win32FileSystem fs;
    const auto found = fs.list_directory("C:\\wsl", L"*");

    REQUIRE(found.has_value());
    REQUIRE(found->size() == 1);
    CHECK((*found)[0].is_directory);
    CHECK((*found)[0].size == 0);
}

TEST_CASE("list_directory treats an empty directory as an empty answer", "[platform][fs]") {
    // `orphans` scans several directories and most of them hold nothing; that is
    // not a failure to report.
    Win32Api api;
    api.find_first_file_ex = [](LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID,
                                DWORD) -> HANDLE { return INVALID_HANDLE_VALUE; };
    api.get_last_error = []() -> DWORD { return ERROR_FILE_NOT_FOUND; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto found = fs.list_directory("C:\\empty", L"*.vhdx");

    REQUIRE(found.has_value());
    CHECK(found->empty());
}

TEST_CASE("list_directory treats no matches as an empty answer", "[platform][fs]") {
    Win32Api api;
    api.find_first_file_ex = [](LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID,
                                DWORD) -> HANDLE { return INVALID_HANDLE_VALUE; };
    api.get_last_error = []() -> DWORD { return ERROR_NO_MORE_FILES; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto found = fs.list_directory("C:\\wsl", L"*.nothing");

    REQUIRE(found.has_value());
    CHECK(found->empty());
}

TEST_CASE("list_directory reports a directory it cannot open", "[platform][fs]") {
    Win32Api api;
    api.find_first_file_ex = [](LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID,
                                DWORD) -> HANDLE { return INVALID_HANDLE_VALUE; };
    api.get_last_error = []() -> DWORD { return ERROR_ACCESS_DENIED; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto found = fs.list_directory("C:\\forbidden", L"*");

    REQUIRE_FALSE(found.has_value());
    CHECK(found.error().code == ErrorCode::NeedsElevation);
    CHECK(found.error().message.find("forbidden") != std::string::npos);
}

TEST_CASE("list_directory reports a listing that broke part way", "[platform][fs]") {
    // A drive pulled mid-scan must not look like the end of the listing.
    const std::vector<Entry> entries{{.name = L"first.vhdx", .size = 1}};
    const auto position = std::make_shared<std::size_t>(0);
    Win32Api api = listing(entries, position);
    api.get_last_error = []() -> DWORD { return ERROR_NOT_READY; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto found = fs.list_directory("C:\\wsl", L"*");

    REQUIRE_FALSE(found.has_value());
    CHECK(found.error().message.find("finish listing") != std::string::npos);
}

namespace {

/// A table that answers the range query with `ranges` in one go.
///
/// `ranges` is taken and captured by value on purpose: callers pass a temporary,
/// and a reference capture would dangle the moment the call expression ended --
/// which is exactly what AddressSanitizer caught the first time.
Win32Api ranges_of(std::uint64_t size, std::vector<FILE_ALLOCATED_RANGE_BUFFER> ranges) {
    Win32Api api;
    api.get_file_attributes_ex = [size](LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID data) -> BOOL {
        auto* attributes = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(data);
        *attributes = WIN32_FILE_ATTRIBUTE_DATA{};
        attributes->nFileSizeHigh = static_cast<DWORD>(size >> 32);
        attributes->nFileSizeLow = static_cast<DWORD>(size & 0xFFFFFFFF);
        return TRUE;
    };
    api.create_file = [](LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) -> HANDLE {
        return file_handle;
    };
    api.close_handle = [](HANDLE) -> BOOL { return TRUE; };
    api.device_io_control = [ranges = std::move(ranges)](HANDLE, DWORD, LPVOID, DWORD, LPVOID out, DWORD,
                                                         LPDWORD returned, LPOVERLAPPED) -> BOOL {
        auto* buffer = static_cast<FILE_ALLOCATED_RANGE_BUFFER*>(out);
        for (std::size_t index = 0; index < ranges.size(); ++index) {
            buffer[index] = ranges[index];
        }
        *returned = static_cast<DWORD>(ranges.size() * sizeof(FILE_ALLOCATED_RANGE_BUFFER));
        return TRUE;
    };
    api.get_last_error = []() -> DWORD { return ERROR_SUCCESS; };
    return api;
}

FILE_ALLOCATED_RANGE_BUFFER range(LONGLONG offset, LONGLONG length) {
    FILE_ALLOCATED_RANGE_BUFFER buffer{};
    buffer.FileOffset.QuadPart = offset;
    buffer.Length.QuadPart = length;
    return buffer;
}

}  // namespace

TEST_CASE("allocated_ranges reports what a sparse file occupies", "[platform][fs]") {
    const std::vector<FILE_ALLOCATED_RANGE_BUFFER> answer{range(0, 4096), range(1048576, 8192)};
    const ScopedWin32Api scoped{ranges_of(10485760, answer)};

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges("C:\\wsl\\ext4.vhdx");

    REQUIRE(ranges.has_value());
    REQUIRE(ranges->size() == 2);
    CHECK((*ranges)[0].offset == 0);
    CHECK((*ranges)[0].length == 4096);
    CHECK((*ranges)[1].offset == 1048576);
    CHECK((*ranges)[1].length == 8192);
}

TEST_CASE("allocated_ranges reports nothing for an empty file", "[platform][fs]") {
    // Asking the filesystem about a zero-length span has no answer to give.
    const ScopedWin32Api scoped{ranges_of(0, {})};

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges("C:\\empty.vhdx");

    REQUIRE(ranges.has_value());
    CHECK(ranges->empty());
}

TEST_CASE("allocated_ranges reports a fully sparse file as occupying nothing", "[platform][fs]") {
    const ScopedWin32Api scoped{ranges_of(1048576, {})};

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges("C:\\hole.vhdx");

    REQUIRE(ranges.has_value());
    CHECK(ranges->empty());
}

TEST_CASE("allocated_ranges continues when one query cannot hold every range", "[platform][fs]") {
    // ERROR_MORE_DATA means the answer was truncated; giving up there would
    // under-report how much of the disk is real.
    int calls = 0;
    Win32Api api = ranges_of(8192, {});
    api.device_io_control = [&calls](HANDLE, DWORD, LPVOID in, DWORD, LPVOID out, DWORD, LPDWORD returned,
                                     LPOVERLAPPED) -> BOOL {
        const auto* query = static_cast<const FILE_ALLOCATED_RANGE_BUFFER*>(in);
        auto* buffer = static_cast<FILE_ALLOCATED_RANGE_BUFFER*>(out);
        if (calls++ == 0) {
            CHECK(query->FileOffset.QuadPart == 0);
            buffer[0] = range(0, 4096);
            *returned = sizeof(FILE_ALLOCATED_RANGE_BUFFER);
            return FALSE;  // more to come
        }
        CHECK(query->FileOffset.QuadPart == 4096);
        buffer[0] = range(4096, 4096);
        *returned = sizeof(FILE_ALLOCATED_RANGE_BUFFER);
        return TRUE;
    };
    api.get_last_error = []() -> DWORD { return ERROR_MORE_DATA; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges("C:\\big.vhdx");

    REQUIRE(ranges.has_value());
    REQUIRE(ranges->size() == 2);
    CHECK((*ranges)[1].offset == 4096);
}

TEST_CASE("allocated_ranges stops when more data is promised but none arrives", "[platform][fs]") {
    // Continuing on an empty answer would loop until the process is killed.
    Win32Api api = ranges_of(8192, {});
    api.device_io_control = [](HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD returned,
                               LPOVERLAPPED) -> BOOL {
        *returned = 0;
        return FALSE;
    };
    api.get_last_error = []() -> DWORD { return ERROR_MORE_DATA; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges("C:\\odd.vhdx");

    REQUIRE(ranges.has_value());
    CHECK(ranges->empty());
}

TEST_CASE("allocated_ranges reports a file it cannot open", "[platform][fs]") {
    Win32Api api = ranges_of(4096, {});
    api.create_file = [](LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) -> HANDLE {
        return INVALID_HANDLE_VALUE;
    };
    api.get_last_error = []() -> DWORD { return ERROR_SHARING_VIOLATION; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges("C:\\busy.vhdx");

    REQUIRE_FALSE(ranges.has_value());
    CHECK(ranges.error().code == ErrorCode::DistroBusy);
}

TEST_CASE("allocated_ranges reports a size it cannot read", "[platform][fs]") {
    Win32Api api = ranges_of(4096, {});
    api.get_file_attributes_ex = [](LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID) -> BOOL { return FALSE; };
    api.get_last_error = []() -> DWORD { return ERROR_FILE_NOT_FOUND; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    CHECK_FALSE(fs.allocated_ranges("C:\\missing.vhdx").has_value());
}

TEST_CASE("allocated_ranges reports a failed query", "[platform][fs]") {
    Win32Api api = ranges_of(4096, {});
    api.device_io_control = [](HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) -> BOOL {
        return FALSE;
    };
    api.get_last_error = []() -> DWORD { return ERROR_NOT_SUPPORTED; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges("C:\\weird.vhdx");

    REQUIRE_FALSE(ranges.has_value());
    CHECK(ranges.error().message.find("allocated ranges") != std::string::npos);
}

TEST_CASE("remove deletes the file", "[platform][fs]") {
    std::wstring deleted;
    Win32Api api;
    api.delete_file = [&deleted](LPCWSTR path) -> BOOL {
        deleted = path;
        return TRUE;
    };
    const ScopedWin32Api scoped{api};

    Win32FileSystem fs;
    REQUIRE(fs.remove("C:\\wsl\\stale.vhdx").has_value());
    CHECK(deleted == L"C:\\wsl\\stale.vhdx");
}

TEST_CASE("remove reports a file it cannot delete", "[platform][fs]") {
    Win32Api api;
    api.delete_file = [](LPCWSTR) -> BOOL { return FALSE; };
    api.get_last_error = []() -> DWORD { return ERROR_SHARING_VIOLATION; };
    const ScopedWin32Api scoped{api};

    Win32FileSystem fs;
    const auto status = fs.remove("C:\\wsl\\busy.vhdx");

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::DistroBusy);
    CHECK(status.error().message.find("busy.vhdx") != std::string::npos);
}

TEST_CASE("expand_environment expands a variable", "[platform][fs]") {
    Win32Api api;
    api.expand_environment_strings = [](LPCWSTR, LPWSTR destination, DWORD size) -> DWORD {
        const std::wstring expanded{L"C:\\Users\\someone\\AppData\\Local"};
        if (destination == nullptr || size == 0) {
            return static_cast<DWORD>(expanded.size() + 1);
        }
        const std::size_t count = std::min<std::size_t>(expanded.size(), size - 1);
        std::copy_n(expanded.begin(), count, destination);
        destination[count] = L'\0';
        return static_cast<DWORD>(expanded.size() + 1);
    };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto expanded = fs.expand_environment("%LOCALAPPDATA%");

    REQUIRE(expanded.has_value());
    CHECK(*expanded == std::filesystem::path{"C:\\Users\\someone\\AppData\\Local"});
}

TEST_CASE("expand_environment reports a failure to size the result", "[platform][fs]") {
    Win32Api api;
    api.expand_environment_strings = [](LPCWSTR, LPWSTR, DWORD) -> DWORD { return 0; };
    api.get_last_error = []() -> DWORD { return ERROR_NOT_ENOUGH_MEMORY; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    CHECK_FALSE(fs.expand_environment("%LOCALAPPDATA%").has_value());
}

TEST_CASE("expand_environment reports a failure on the second call", "[platform][fs]") {
    int calls = 0;
    Win32Api api;
    api.expand_environment_strings = [&calls](LPCWSTR, LPWSTR, DWORD) -> DWORD {
        return calls++ == 0 ? 32 : 0;
    };
    api.get_last_error = []() -> DWORD { return ERROR_NOT_ENOUGH_MEMORY; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto expanded = fs.expand_environment("%LOCALAPPDATA%");

    REQUIRE_FALSE(expanded.has_value());
    CHECK(expanded.error().message.find("expand") != std::string::npos);
}

TEST_CASE("allocated_ranges stops once the query reaches the end of the file", "[platform][fs]") {
    // Two truncated answers that between them cover the whole file: the loop
    // has to notice it is done from the offset rather than from a final
    // "no more data" reply, which is the case a file whose ranges exactly fill
    // the last query produces.
    int calls = 0;
    Win32Api api = ranges_of(8192, {});
    api.device_io_control = [&calls](HANDLE, DWORD, LPVOID, DWORD, LPVOID out, DWORD, LPDWORD returned,
                                     LPOVERLAPPED) -> BOOL {
        auto* buffer = static_cast<FILE_ALLOCATED_RANGE_BUFFER*>(out);
        buffer[0] = calls++ == 0 ? range(0, 4096) : range(4096, 4096);
        *returned = sizeof(FILE_ALLOCATED_RANGE_BUFFER);
        return FALSE;  // always "more to come", even once there is not
    };
    api.get_last_error = []() -> DWORD { return ERROR_MORE_DATA; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges("C:\\exact.vhdx");

    REQUIRE(ranges.has_value());
    CHECK(ranges->size() == 2);
    CHECK(calls == 2);
}
