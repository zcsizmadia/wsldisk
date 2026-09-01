#include <windows.h>
#include <winioctl.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "errors.h"
#include "platform/filesystem.h"
#include "platform/win32_api.h"

using wsldisk::AllocatedRange;
using wsldisk::ErrorCode;
using wsldisk::platform::ScopedWin32Api;
using wsldisk::platform::Win32Api;
using wsldisk::platform::Win32FileSystem;

namespace {

constexpr std::uint64_t kilobyte = 1024;

/// Enough of a filesystem to copy a file through.
///
/// The sparse copy is the one operation whose correctness is not visible from a
/// single call: it seeks, reads and writes its way through a range list, and the
/// thing being asserted is *which bytes ended up where*. A table of one-shot
/// stubs cannot show that, so this models a source file with holes and a
/// destination that remembers what was written into it.
struct Disk {
    /// Source contents, holes included. Bytes inside a hole are never read.
    std::vector<std::byte> source;
    std::vector<AllocatedRange> ranges;

    /// What the destination ended up holding, keyed by offset, so a hole is
    /// visibly a gap rather than a run of zeroes that might have been written.
    std::map<std::uint64_t, std::byte> written;
    std::uint64_t destination_length = 0;

    /// Where each handle's file pointer is.
    std::map<HANDLE, std::uint64_t> positions;

    bool sparse_set = false;
    /// Reads that return this many bytes fewer than asked, for the short-read path.
    bool short_read = false;
    /// Writes that accept one byte less than offered.
    bool short_write = false;

    DWORD last_error = ERROR_SUCCESS;

    /// Which call should fail, by name, so a test names one branch and leaves
    /// the rest working.
    std::string failing;

    /// How many times to let `failing` succeed before failing it.
    ///
    /// The copy asks the same questions its helpers ask: `allocated_ranges`
    /// reads the file size and opens the source before the copy does either, and
    /// setting the destination's length seeks before the loop does. Failing
    /// those outright never reaches the copy's own handling of them, so the
    /// branch that is meant to be under test never runs.
    int allow_first = 0;
};

Disk* disk = nullptr;

// Distinct non-null handles, so the table can tell source from destination.
HANDLE source_handle() {
    static int tag = 0;
    return &tag;
}

HANDLE destination_handle() {
    static int tag = 0;
    return &tag;
}

[[nodiscard]] bool fails(std::string_view call) {
    if (disk->failing != call) {
        return false;
    }
    if (disk->allow_first > 0) {
        --disk->allow_first;
        return false;
    }
    return true;
}

/// A table that copies successfully. Tests break one call at a time.
Win32Api working() {
    Win32Api api;
    api.get_last_error = []() -> DWORD { return disk->last_error; };

    api.get_file_attributes_ex = [](LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID info) -> BOOL {
        if (fails("file_size")) {
            disk->last_error = ERROR_FILE_NOT_FOUND;
            return FALSE;
        }
        auto* data = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(info);
        *data = WIN32_FILE_ATTRIBUTE_DATA{};
        data->nFileSizeLow = static_cast<DWORD>(disk->source.size());
        return TRUE;
    };

    api.create_file = [](LPCWSTR name, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD,
                         HANDLE) -> HANDLE {
        const std::wstring path{name};
        const bool destination = path.find(L"copy") != std::wstring::npos;
        if (destination && fails("create_destination")) {
            disk->last_error = ERROR_FILE_EXISTS;
            return INVALID_HANDLE_VALUE;
        }
        if (!destination && fails("open_source")) {
            disk->last_error = ERROR_ACCESS_DENIED;
            return INVALID_HANDLE_VALUE;
        }
        // `allocated_ranges` opens the source too; only the copy asks for
        // CREATE_NEW, which is what distinguishes the two.
        if (destination) {
            REQUIRE(disposition == CREATE_NEW);
            return destination_handle();
        }
        return source_handle();
    };

    api.device_io_control = [](HANDLE, DWORD code, LPVOID in, DWORD, LPVOID out, DWORD out_size,
                               LPDWORD returned, LPOVERLAPPED) -> BOOL {
        if (code == FSCTL_SET_SPARSE) {
            if (fails("set_sparse")) {
                disk->last_error = ERROR_INVALID_FUNCTION;
                return FALSE;
            }
            disk->sparse_set = true;
            return TRUE;
        }
        REQUIRE(code == FSCTL_QUERY_ALLOCATED_RANGES);
        if (fails("query_ranges")) {
            disk->last_error = ERROR_ACCESS_DENIED;
            return FALSE;
        }
        // One pass: the paging behaviour has its own tests against the real API.
        const auto* query = static_cast<const FILE_ALLOCATED_RANGE_BUFFER*>(in);
        auto* answer = static_cast<FILE_ALLOCATED_RANGE_BUFFER*>(out);
        DWORD count = 0;
        for (const AllocatedRange& range : disk->ranges) {
            if (static_cast<LONGLONG>(range.offset) < query->FileOffset.QuadPart) {
                continue;
            }
            if ((count + 1) * sizeof(FILE_ALLOCATED_RANGE_BUFFER) > out_size) {
                break;
            }
            answer[count].FileOffset.QuadPart = static_cast<LONGLONG>(range.offset);
            answer[count].Length.QuadPart = static_cast<LONGLONG>(range.length);
            ++count;
        }
        *returned = static_cast<DWORD>(count * sizeof(FILE_ALLOCATED_RANGE_BUFFER));
        return TRUE;
    };

    api.set_file_pointer_ex = [](HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER, DWORD) -> BOOL {
        if (fails("seek")) {
            disk->last_error = ERROR_HANDLE_EOF;
            return FALSE;
        }
        if (file == destination_handle() && fails("seek_destination")) {
            disk->last_error = ERROR_HANDLE_EOF;
            return FALSE;
        }
        disk->positions[file] = static_cast<std::uint64_t>(distance.QuadPart);
        return TRUE;
    };

    api.set_end_of_file = [](HANDLE file) -> BOOL {
        if (fails("set_end_of_file")) {
            disk->last_error = ERROR_DISK_FULL;
            return FALSE;
        }
        disk->destination_length = disk->positions[file];
        return TRUE;
    };

    api.read_file = [](HANDLE file, LPVOID buffer, DWORD to_read, LPDWORD read, LPOVERLAPPED) -> BOOL {
        if (fails("read")) {
            disk->last_error = ERROR_READ_FAULT;
            return FALSE;
        }
        if (disk->short_read) {
            *read = 0;
            return TRUE;
        }
        const std::uint64_t at = disk->positions[file];
        const auto available = static_cast<DWORD>(disk->source.size() - at);
        const DWORD count = to_read < available ? to_read : available;
        std::memcpy(buffer, disk->source.data() + at, count);
        *read = count;
        return TRUE;
    };

    api.write_file = [](HANDLE file, LPCVOID buffer, DWORD to_write, LPDWORD written, LPOVERLAPPED) -> BOOL {
        if (fails("write")) {
            disk->last_error = ERROR_DISK_FULL;
            return FALSE;
        }
        const DWORD count = disk->short_write ? to_write - 1 : to_write;
        const std::uint64_t at = disk->positions[file];
        const auto* bytes = static_cast<const std::byte*>(buffer);
        for (DWORD index = 0; index < count; ++index) {
            disk->written[at + index] = bytes[index];
        }
        *written = count;
        return TRUE;
    };

    api.close_handle = [](HANDLE) -> BOOL { return TRUE; };
    return api;
}

/// A source of `length` bytes where every byte is its own offset modulo 251,
/// so a byte that landed at the wrong offset is visible rather than plausible.
void make_source(Disk& state, std::size_t length, std::vector<AllocatedRange> ranges) {
    state.source.resize(length);
    for (std::size_t index = 0; index < length; ++index) {
        state.source[index] = static_cast<std::byte>(index % 251);
    }
    state.ranges = std::move(ranges);
}

/// Ignores progress, for the tests that are not about it.
bool always_continue(std::uint64_t, std::uint64_t) {
    return true;
}

}  // namespace

TEST_CASE("a sparse copy writes the allocated ranges and leaves the holes alone", "[platform][copy]") {
    // The whole point. A WSL disk's logical length is its virtual size -- a
    // 12 GiB Ubuntu inside a 1 TiB VHDX -- so a copy that walks the length
    // instead of the ranges writes a terabyte to move twelve gigabytes.
    Disk state;
    make_source(state, 8 * kilobyte,
                {AllocatedRange{.offset = 0, .length = kilobyte},
                 AllocatedRange{.offset = 6 * kilobyte, .length = kilobyte}});
    disk = &state;
    const ScopedWin32Api api{working()};

    Win32FileSystem filesystem;
    const auto outcome =
        filesystem.copy_file_sparse(LR"(C:\wsl\ext4.vhdx)", LR"(D:\wsl\copy.vhdx)", always_continue);

    REQUIRE(outcome.has_value());
    CHECK(state.sparse_set);
    // The copy is the same length as the original even though 6 KiB of it was
    // never written.
    CHECK(state.destination_length == 8 * kilobyte);
    CHECK(state.written.size() == 2 * kilobyte);
    for (std::uint64_t offset = 0; offset < kilobyte; ++offset) {
        CHECK(state.written.at(offset) == state.source[offset]);
    }
    for (std::uint64_t offset = 6 * kilobyte; offset < 7 * kilobyte; ++offset) {
        CHECK(state.written.at(offset) == state.source[offset]);
    }
    // The hole between them was not written at all -- not even as zeroes.
    CHECK_FALSE(state.written.contains(kilobyte));
    CHECK_FALSE(state.written.contains(5 * kilobyte));
}

TEST_CASE("a sparse copy reports progress against the allocated bytes", "[platform][copy]") {
    // Against the bytes there are to copy, not the logical length: a progress
    // bar that stops at 1% on a mostly-empty disk is telling the user the wrong
    // thing about a copy that is nearly done.
    Disk state;
    make_source(state, 8 * kilobyte, {AllocatedRange{.offset = 0, .length = 2 * kilobyte}});
    disk = &state;
    const ScopedWin32Api api{working()};

    std::vector<std::pair<std::uint64_t, std::uint64_t>> reports;
    Win32FileSystem filesystem;
    const auto outcome = filesystem.copy_file_sparse(LR"(C:\wsl\ext4.vhdx)", LR"(D:\wsl\copy.vhdx)",
                                                     [&reports](std::uint64_t copied, std::uint64_t total) {
                                                         reports.emplace_back(copied, total);
                                                         return true;
                                                     });

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(reports.empty());
    CHECK(reports.back().first == 2 * kilobyte);
    CHECK(reports.back().second == 2 * kilobyte);
}

TEST_CASE("a copy of a file with no allocated ranges still makes the destination", "[platform][copy]") {
    // A freshly created VHDX can be all holes. There is nothing to write, but
    // there is still a file to create and a length to set.
    Disk state;
    make_source(state, 4 * kilobyte, {});
    disk = &state;
    const ScopedWin32Api api{working()};

    Win32FileSystem filesystem;
    const auto outcome =
        filesystem.copy_file_sparse(LR"(C:\wsl\ext4.vhdx)", LR"(D:\wsl\copy.vhdx)", always_continue);

    REQUIRE(outcome.has_value());
    CHECK(state.written.empty());
    CHECK(state.destination_length == 4 * kilobyte);
}

TEST_CASE("a cancelled copy stops and says the destination is partial", "[platform][copy]") {
    Disk state;
    make_source(state, 8 * kilobyte, {AllocatedRange{.offset = 0, .length = 4 * kilobyte}});
    disk = &state;
    const ScopedWin32Api api{working()};

    Win32FileSystem filesystem;
    const auto outcome = filesystem.copy_file_sparse(LR"(C:\wsl\ext4.vhdx)", LR"(D:\wsl\copy.vhdx)",
                                                     [](std::uint64_t, std::uint64_t) { return false; });

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::Partial);
    // The caller has to know there is a half-written file to deal with.
    CHECK(outcome.error().remedy.find("copy.vhdx") != std::string::npos);
}

TEST_CASE("a copy reports the call that failed", "[platform][copy]") {
    // One test per branch would be a dozen near-identical tests; the failing
    // call is the only thing that differs, so it is the only thing that varies.
    //
    // `allow_first` is how many times that call succeeds before it fails, which
    // is what reaches the copy's own error handling rather than a helper's: both
    // `allocated_ranges` and the length-setting seek get there first.
    struct Case {
        const char* call;
        int allow_first;
    };

    const auto broken =
        GENERATE(Case{"query_ranges", 0},  // inside allocated_ranges
                 Case{"file_size", 0},     // inside allocated_ranges
                 Case{"file_size", 1},     // the copy's own length read
                 Case{"open_source", 0},   // inside allocated_ranges
                 Case{"open_source", 1},   // the copy's own open
                 Case{"create_destination", 0}, Case{"set_sparse", 0}, Case{"set_end_of_file", 0},
                 Case{"seek_destination", 0},  // setting the length
                 Case{"seek_destination", 1},  // inside the copy loop
                 Case{"seek", 1},              // the source seek in the loop
                 Case{"read", 0}, Case{"write", 0});
    CAPTURE(broken.call, broken.allow_first);

    Disk state;
    make_source(state, 4 * kilobyte, {AllocatedRange{.offset = 0, .length = 2 * kilobyte}});
    state.failing = broken.call;
    state.allow_first = broken.allow_first;
    disk = &state;
    const ScopedWin32Api api{working()};

    Win32FileSystem filesystem;
    const auto outcome =
        filesystem.copy_file_sparse(LR"(C:\wsl\ext4.vhdx)", LR"(D:\wsl\copy.vhdx)", always_continue);

    REQUIRE_FALSE(outcome.has_value());
    CHECK_FALSE(outcome.error().message.empty());
}

TEST_CASE("a copy refuses a source that ends earlier than its ranges claim", "[platform][copy]") {
    // The filesystem said the range was allocated and the read returned nothing.
    // Something is writing to the file; a copy that carried on would be quietly
    // not the original.
    Disk state;
    make_source(state, 4 * kilobyte, {AllocatedRange{.offset = 0, .length = 2 * kilobyte}});
    state.short_read = true;
    disk = &state;
    const ScopedWin32Api api{working()};

    Win32FileSystem filesystem;
    const auto outcome =
        filesystem.copy_file_sparse(LR"(C:\wsl\ext4.vhdx)", LR"(D:\wsl\copy.vhdx)", always_continue);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("ended earlier") != std::string::npos);
}

TEST_CASE("a copy refuses a destination that took only part of a write", "[platform][copy]") {
    // A full volume is the usual cause, and a short write that went unnoticed
    // would leave a corrupt disk that looked like a successful copy.
    Disk state;
    make_source(state, 4 * kilobyte, {AllocatedRange{.offset = 0, .length = 2 * kilobyte}});
    state.short_write = true;
    disk = &state;
    const ScopedWin32Api api{working()};

    Win32FileSystem filesystem;
    const auto outcome =
        filesystem.copy_file_sparse(LR"(C:\wsl\ext4.vhdx)", LR"(D:\wsl\copy.vhdx)", always_continue);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("only part of a write") != std::string::npos);
    CHECK(outcome.error().remedy.find("free space") != std::string::npos);
}

TEST_CASE("rename asks for a rename and nothing else", "[platform][copy]") {
    // Deliberately not MOVEFILE_COPY_ALLOWED: across volumes Windows would copy
    // without preserving the holes, which for a VHDX is the difference between
    // moving twelve gigabytes and writing a terabyte.
    DWORD asked = 0xFFFF;
    Win32Api api;
    api.move_file_ex = [&asked](LPCWSTR, LPCWSTR, DWORD flags) -> BOOL {
        asked = flags;
        return TRUE;
    };
    const ScopedWin32Api scoped{api};

    Win32FileSystem filesystem;
    const auto outcome = filesystem.rename(LR"(C:\a.vhdx)", LR"(C:\b.vhdx)");

    REQUIRE(outcome.has_value());
    CHECK(asked == 0);
}

TEST_CASE("rename reports a failure with both paths", "[platform][copy]") {
    Win32Api api;
    api.move_file_ex = [](LPCWSTR, LPCWSTR, DWORD) -> BOOL { return FALSE; };
    api.get_last_error = []() -> DWORD { return ERROR_NOT_SAME_DEVICE; };
    const ScopedWin32Api scoped{api};

    Win32FileSystem filesystem;
    const auto outcome = filesystem.rename(LR"(C:\a.vhdx)", LR"(D:\b.vhdx)");

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("a.vhdx") != std::string::npos);
    CHECK(outcome.error().message.find("b.vhdx") != std::string::npos);
}

namespace {

/// A table whose volume lookup answers `root` for every path.
Win32Api volume_named(const wchar_t* first, const wchar_t* second) {
    Win32Api api;
    api.get_volume_path_name = [first, second](LPCWSTR path, LPWSTR buffer, DWORD size) -> BOOL {
        const std::wstring name{path};
        const wchar_t* answer = name.find(L"first") != std::wstring::npos ? first : second;
        const std::size_t length = std::wcslen(answer);
        REQUIRE(length + 1 <= size);
        std::memcpy(buffer, answer, (length + 1) * sizeof(wchar_t));
        return TRUE;
    };
    api.get_last_error = []() -> DWORD { return ERROR_PATH_NOT_FOUND; };
    return api;
}

}  // namespace

TEST_CASE("same_volume compares the volume, not the path", "[platform][copy]") {
    // Drive letters are not the answer: a directory can be a mount point for
    // another volume, and two paths under one letter can be on two volumes.
    const ScopedWin32Api scoped{volume_named(LR"(C:\)", LR"(C:\)")};

    Win32FileSystem filesystem;
    const auto outcome = filesystem.same_volume(LR"(C:\first\a.vhdx)", LR"(C:\second\b.vhdx)");

    REQUIRE(outcome.has_value());
    CHECK(*outcome);
}

TEST_CASE("same_volume says no when the volumes differ", "[platform][copy]") {
    const ScopedWin32Api scoped{volume_named(LR"(C:\)", LR"(D:\)")};

    Win32FileSystem filesystem;
    const auto outcome = filesystem.same_volume(LR"(C:\first\a.vhdx)", LR"(D:\second\b.vhdx)");

    REQUIRE(outcome.has_value());
    CHECK_FALSE(*outcome);
}

TEST_CASE("same_volume ignores case, because the filesystem does", "[platform][copy]") {
    const ScopedWin32Api scoped{volume_named(LR"(C:\)", LR"(c:\)")};

    Win32FileSystem filesystem;
    const auto outcome = filesystem.same_volume(LR"(C:\first\a.vhdx)", LR"(c:\second\b.vhdx)");

    REQUIRE(outcome.has_value());
    CHECK(*outcome);
}

TEST_CASE("same_volume reports a volume it could not resolve", "[platform][copy]") {
    const auto which = GENERATE(0, 1);
    CAPTURE(which);

    Win32Api api;
    api.get_volume_path_name = [which](LPCWSTR path, LPWSTR buffer, DWORD size) -> BOOL {
        const std::wstring name{path};
        const bool is_first = name.find(L"first") != std::wstring::npos;
        if ((which == 0 && is_first) || (which == 1 && !is_first)) {
            return FALSE;
        }
        const wchar_t* answer = LR"(C:\)";
        const std::size_t length = std::wcslen(answer);
        REQUIRE(length + 1 <= size);
        std::memcpy(buffer, answer, (length + 1) * sizeof(wchar_t));
        return TRUE;
    };
    api.get_last_error = []() -> DWORD { return ERROR_PATH_NOT_FOUND; };
    const ScopedWin32Api scoped{api};

    Win32FileSystem filesystem;
    const auto outcome = filesystem.same_volume(LR"(C:\first\a.vhdx)", LR"(D:\second\b.vhdx)");

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("volume holding") != std::string::npos);
}
