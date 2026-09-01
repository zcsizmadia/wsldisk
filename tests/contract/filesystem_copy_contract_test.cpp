// Contract tests for the sparse-preserving copy, against the real Win32 API on
// real files under %TEMP%. The fake in the unit tests can be made to agree with
// any belief about how sparse files behave; only NTFS can settle it.

#include <windows.h>
#include <winioctl.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "errors.h"
#include "platform/filesystem.h"

using wsldisk::ErrorCode;
using wsldisk::platform::Win32FileSystem;

namespace {

constexpr std::uint64_t megabyte = 1024 * 1024;

/// A path under %TEMP% that removes whatever is at it, whatever the test does.
class TempPath {
public:
    TempPath() : path_(make_path()) {}

    ~TempPath() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    TempPath(TempPath&&) = delete;
    TempPath& operator=(TempPath&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    static std::filesystem::path make_path() {
        static int counter = 0;
        return std::filesystem::temp_directory_path() /
               ("wsldisk-copy-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(++counter) +
                ".bin");
    }

    std::filesystem::path path_;
};

/// Writes a sparse file shaped like a VHDX: a large logical length with two
/// small islands of real data in it.
///
/// Returns false when the volume will not make the file sparse, which is not a
/// test failure -- %TEMP% on a machine that is not NTFS is a reason to skip.
[[nodiscard]] bool make_sparse_file(const std::filesystem::path& path, std::uint64_t length,
                                    const std::vector<std::uint64_t>& island_offsets,
                                    std::uint64_t island_length) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD returned = 0;
    if (::DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) == FALSE) {
        ::CloseHandle(file);
        return false;
    }

    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(length);
    if (::SetFilePointerEx(file, end, nullptr, FILE_BEGIN) == FALSE || ::SetEndOfFile(file) == FALSE) {
        ::CloseHandle(file);
        return false;
    }

    const std::vector<std::byte> block(island_length, std::byte{0xAB});
    for (const std::uint64_t offset : island_offsets) {
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(offset);
        DWORD written = 0;
        if (::SetFilePointerEx(file, at, nullptr, FILE_BEGIN) == FALSE ||
            ::WriteFile(file, block.data(), static_cast<DWORD>(block.size()), &written, nullptr) == FALSE) {
            ::CloseHandle(file);
            return false;
        }
    }
    ::CloseHandle(file);
    return true;
}

[[nodiscard]] bool ignore_progress(std::uint64_t, std::uint64_t) {
    return true;
}

}  // namespace

TEST_CASE("a sparse file copies to a sparse file on real NTFS", "[contract][filesystem]") {
    // The failure this exists to catch: a copy that walks the logical length
    // rather than the allocated ranges turns a 2 MiB file into a 64 MiB one.
    // A WSL disk is the same shape three orders of magnitude larger -- twelve
    // gigabytes of data inside a terabyte of virtual size.
    const TempPath source;
    const TempPath destination;
    if (!make_sparse_file(source.path(), 64 * megabyte, {0, 32 * megabyte}, megabyte)) {
        SKIP("this volume will not create sparse files");
    }

    Win32FileSystem fs;
    const auto source_on_disk = fs.file_size_on_disk(source.path());
    REQUIRE(source_on_disk.has_value());
    // The premise: the source really is mostly hole.
    REQUIRE(*source_on_disk < 8 * megabyte);

    const auto copied = fs.copy_file_sparse(source.path(), destination.path(), ignore_progress);
    REQUIRE(copied.has_value());

    const auto logical = fs.file_size(destination.path());
    REQUIRE(logical.has_value());
    CHECK(*logical == 64 * megabyte);

    const auto sparse = fs.is_sparse(destination.path());
    REQUIRE(sparse.has_value());
    CHECK(*sparse);

    const auto destination_on_disk = fs.file_size_on_disk(destination.path());
    REQUIRE(destination_on_disk.has_value());
    // Within a cluster or two of the source, and nowhere near the logical size.
    CHECK(*destination_on_disk <= *source_on_disk + (64 * 1024));
    CHECK(*destination_on_disk < 8 * megabyte);
}

TEST_CASE("a sparse copy reproduces the bytes, not just the size", "[contract][filesystem]") {
    const TempPath source;
    const TempPath destination;
    if (!make_sparse_file(source.path(), 16 * megabyte, {megabyte, 8 * megabyte}, 64 * 1024)) {
        SKIP("this volume will not create sparse files");
    }

    Win32FileSystem fs;
    REQUIRE(fs.copy_file_sparse(source.path(), destination.path(), ignore_progress).has_value());

    // Ranges first: the copy has to have the same holes in the same places, or
    // it is a different file that happens to read the same.
    const auto source_ranges = fs.allocated_ranges(source.path());
    const auto destination_ranges = fs.allocated_ranges(destination.path());
    REQUIRE(source_ranges.has_value());
    REQUIRE(destination_ranges.has_value());
    REQUIRE(source_ranges->size() == destination_ranges->size());
    for (std::size_t index = 0; index < source_ranges->size(); ++index) {
        CHECK((*source_ranges)[index].offset == (*destination_ranges)[index].offset);
        CHECK((*source_ranges)[index].length == (*destination_ranges)[index].length);
    }

    // Then the contents, island and hole alike. Reading a hole gives zeroes,
    // which is exactly what the original gives.
    const auto read_all = [](const std::filesystem::path& path) {
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(file != INVALID_HANDLE_VALUE);
        std::vector<std::byte> bytes(16 * megabyte);
        std::uint64_t total = 0;
        while (total < bytes.size()) {
            DWORD read = 0;
            const auto want = static_cast<DWORD>(bytes.size() - total);
            REQUIRE(::ReadFile(file, bytes.data() + total, want, &read, nullptr) != FALSE);
            if (read == 0) {
                break;
            }
            total += read;
        }
        ::CloseHandle(file);
        return bytes;
    };
    CHECK(read_all(source.path()) == read_all(destination.path()));
}

TEST_CASE("a copy refuses to overwrite a destination that exists", "[contract][filesystem]") {
    // A half-finished copy from a previous attempt is the file most worth not
    // clobbering, so this is CREATE_NEW rather than CREATE_ALWAYS.
    const TempPath source;
    const TempPath destination;
    REQUIRE(make_sparse_file(source.path(), megabyte, {0}, 4096));
    REQUIRE(make_sparse_file(destination.path(), megabyte, {0}, 4096));

    Win32FileSystem fs;
    const auto copied = fs.copy_file_sparse(source.path(), destination.path(), ignore_progress);

    REQUIRE_FALSE(copied.has_value());
    CHECK(copied.error().message.find("create") != std::string::npos);
}

TEST_CASE("a copy of a file that is not there fails before creating anything", "[contract][filesystem]") {
    const TempPath destination;
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "wsldisk-copy-nothing-here.bin";

    Win32FileSystem fs;
    const auto copied = fs.copy_file_sparse(missing, destination.path(), ignore_progress);

    REQUIRE_FALSE(copied.has_value());
    CHECK_FALSE(std::filesystem::exists(destination.path()));
}

TEST_CASE("a cancelled copy leaves the partial file for the caller", "[contract][filesystem]") {
    // Deliberately not deleted here: the operation that cancelled knows why, and
    // whether the half-copy is worth keeping is its decision, not this layer's.
    const TempPath source;
    const TempPath destination;
    if (!make_sparse_file(source.path(), 8 * megabyte, {0}, 4 * megabyte)) {
        SKIP("this volume will not create sparse files");
    }

    Win32FileSystem fs;
    const auto copied = fs.copy_file_sparse(source.path(), destination.path(),
                                            [](std::uint64_t, std::uint64_t) { return false; });

    REQUIRE_FALSE(copied.has_value());
    CHECK(copied.error().code == ErrorCode::Partial);
    CHECK(std::filesystem::exists(destination.path()));
}

TEST_CASE("rename moves a file within a volume", "[contract][filesystem]") {
    const TempPath source;
    const TempPath destination;
    REQUIRE(make_sparse_file(source.path(), megabyte, {0}, 4096));

    Win32FileSystem fs;
    REQUIRE(fs.rename(source.path(), destination.path()).has_value());

    CHECK_FALSE(std::filesystem::exists(source.path()));
    CHECK(std::filesystem::exists(destination.path()));
    const auto sparse = fs.is_sparse(destination.path());
    REQUIRE(sparse.has_value());
    // A rename does not rewrite the file, so the holes are untouched by
    // definition -- which is the whole reason the same-volume path exists.
    CHECK(*sparse);
}

TEST_CASE("rename reports a source that is not there", "[contract][filesystem]") {
    const TempPath destination;
    Win32FileSystem fs;
    const auto renamed = fs.rename(std::filesystem::temp_directory_path() / "wsldisk-rename-nothing-here.bin",
                                   destination.path());

    REQUIRE_FALSE(renamed.has_value());
    CHECK(renamed.error().code == ErrorCode::Preflight);
}

TEST_CASE("same_volume agrees with itself about %TEMP%", "[contract][filesystem]") {
    Win32FileSystem fs;
    const std::filesystem::path first = std::filesystem::temp_directory_path() / "a.bin";
    const std::filesystem::path second = std::filesystem::temp_directory_path() / "nested" / "b.bin";

    const auto same = fs.same_volume(first, second);
    REQUIRE(same.has_value());
    CHECK(*same);
}

TEST_CASE("same_volume reports a path it cannot resolve", "[contract][filesystem]") {
    Win32FileSystem fs;
    const auto same = fs.same_volume(std::filesystem::temp_directory_path(), LR"(\\?\Volume{no-such}\x)");

    // Either an error or a plain "no"; what it must not do is claim they match.
    if (same.has_value()) {
        CHECK_FALSE(*same);
    }
}
