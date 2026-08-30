// Contract tests for the M1 filesystem additions, against the real API on real
// files under %TEMP%. The sparse-file case is the one that matters: spike #4
// found the sparse attribute cannot say how much of a file is real, so `list`
// reports allocated ranges instead, and this proves the number is true.

#include <windows.h>
#include <winioctl.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include "errors.h"
#include "platform/clock.h"
#include "platform/filesystem.h"

using wsldisk::AllocatedRange;
using wsldisk::ErrorCode;
using wsldisk::platform::Win32FileSystem;

namespace {

/// A directory under %TEMP% that removes itself and everything in it.
class TempDirectory {
public:
    TempDirectory()
        : path_(
              std::filesystem::temp_directory_path() /
              ("wsldisk-scan-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(++counter))) {
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Writes a file of `size` bytes of zeros.
    [[nodiscard]] std::filesystem::path write(const std::string& name, std::size_t size) const {
        const std::filesystem::path file = path_ / name;
        std::ofstream stream(file, std::ios::binary);
        const std::vector<char> zeros(size, '\0');
        stream.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        return file;
    }

private:
    static int counter;
    std::filesystem::path path_;
};

int TempDirectory::counter = 0;

/// Bytes the ranges add up to.
[[nodiscard]] std::uint64_t total(const std::vector<AllocatedRange>& ranges) {
    return std::accumulate(ranges.begin(), ranges.end(), std::uint64_t{0},
                           [](std::uint64_t sum, const AllocatedRange& range) { return sum + range.length; });
}

/// Marks a file sparse and punches a hole in it, the way a VHDX ends up sparse.
/// Returns false when the volume will not do it, which is not a test failure.
[[nodiscard]] bool punch_hole(const std::filesystem::path& path, std::uint64_t from, std::uint64_t to) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD returned = 0;
    if (::DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) == FALSE) {
        ::CloseHandle(file);
        return false;
    }

    FILE_ZERO_DATA_INFORMATION zero{};
    zero.FileOffset.QuadPart = static_cast<LONGLONG>(from);
    zero.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(to);
    const BOOL punched =
        ::DeviceIoControl(file, FSCTL_SET_ZERO_DATA, &zero, sizeof(zero), nullptr, 0, &returned, nullptr);
    ::CloseHandle(file);
    return punched != FALSE;
}

constexpr std::size_t megabyte = 1024 * 1024;

}  // namespace

TEST_CASE("allocated ranges are smaller than the logical size for a sparse file", "[contract][fs]") {
    const TempDirectory directory;
    const auto file = directory.write("sparse.bin", 4 * megabyte);

    if (!punch_hole(file, megabyte, 3 * megabyte)) {
        SUCCEED("this volume does not support sparse files");
        return;
    }

    const Win32FileSystem fs;
    const auto sparse = fs.is_sparse(file);
    REQUIRE(sparse.has_value());
    CHECK(*sparse);

    const auto ranges = fs.allocated_ranges(file);
    REQUIRE(ranges.has_value());
    // The hole is 2 MiB of a 4 MiB file, so at most half of it can still be
    // allocated. This is the number `list` reports, and the whole reason it does
    // not just read the sparse attribute.
    CHECK(total(*ranges) <= 2 * megabyte);
    CHECK(total(*ranges) > 0);
    for (const AllocatedRange& range : *ranges) {
        CHECK(range.length > 0);
        CHECK(range.offset + range.length <= 4 * megabyte);
    }
}

TEST_CASE("a file with no holes reports its whole length as allocated", "[contract][fs]") {
    const TempDirectory directory;
    const auto file = directory.write("dense.bin", megabyte);

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges(file);

    REQUIRE(ranges.has_value());
    REQUIRE(ranges->size() == 1);
    CHECK((*ranges)[0].offset == 0);
    CHECK((*ranges)[0].length == megabyte);
}

TEST_CASE("an empty file occupies nothing", "[contract][fs]") {
    const TempDirectory directory;
    const auto file = directory.write("empty.bin", 0);

    const Win32FileSystem fs;
    const auto ranges = fs.allocated_ranges(file);

    REQUIRE(ranges.has_value());
    CHECK(ranges->empty());
}

TEST_CASE("allocated_ranges reports a file that is not there", "[contract][fs]") {
    const Win32FileSystem fs;
    const auto missing = std::filesystem::temp_directory_path() / "wsldisk-no-such-file.bin";
    REQUIRE_FALSE(std::filesystem::exists(missing));

    CHECK_FALSE(fs.allocated_ranges(missing).has_value());
}

TEST_CASE("list_directory finds only what the pattern matches", "[contract][fs]") {
    const TempDirectory directory;
    std::ignore = directory.write("ext4.vhdx", 1024);
    std::ignore = directory.write("other.vhdx", 2048);
    std::ignore = directory.write("notes.txt", 16);
    std::filesystem::create_directory(directory.path() / "nested");

    const Win32FileSystem fs;
    const auto found = fs.list_directory(directory.path(), L"*.vhdx");

    REQUIRE(found.has_value());
    REQUIRE(found->size() == 2);
    for (const auto& entry : *found) {
        CHECK(entry.path.extension() == ".vhdx");
        CHECK_FALSE(entry.is_directory);
        CHECK(entry.size > 0);
        CHECK(entry.path.parent_path() == directory.path());
    }
}

TEST_CASE("list_directory reports directories and files apart", "[contract][fs]") {
    const TempDirectory directory;
    std::ignore = directory.write("ext4.vhdx", 32);
    std::filesystem::create_directory(directory.path() / "nested");

    const Win32FileSystem fs;
    const auto found = fs.list_directory(directory.path(), L"*");

    REQUIRE(found.has_value());
    REQUIRE(found->size() == 2);
    // `.` and `..` are real entries on disk and must not appear.
    for (const auto& entry : *found) {
        CHECK(entry.path.filename() != ".");
        CHECK(entry.path.filename() != "..");
    }
    const auto directories =
        std::ranges::count_if(*found, [](const auto& entry) { return entry.is_directory; });
    CHECK(directories == 1);
}

TEST_CASE("listing an empty directory is not a failure", "[contract][fs]") {
    const TempDirectory directory;

    const Win32FileSystem fs;
    const auto found = fs.list_directory(directory.path(), L"*.vhdx");

    REQUIRE(found.has_value());
    CHECK(found->empty());
}

TEST_CASE("listing a directory that does not exist is a preflight failure", "[contract][fs]") {
    const Win32FileSystem fs;
    const auto missing = std::filesystem::temp_directory_path() / "wsldisk-no-such-directory";
    REQUIRE_FALSE(std::filesystem::exists(missing));

    const auto found = fs.list_directory(missing, L"*");

    // FindFirstFileEx reports ERROR_PATH_NOT_FOUND for a missing directory,
    // which is a failure -- unlike an existing directory with no matches.
    REQUIRE_FALSE(found.has_value());
    CHECK(found.error().code == ErrorCode::Preflight);
}

TEST_CASE("remove deletes a real file", "[contract][fs]") {
    const TempDirectory directory;
    const auto file = directory.write("stale.vhdx", 8);
    REQUIRE(std::filesystem::exists(file));

    Win32FileSystem fs;
    REQUIRE(fs.remove(file).has_value());

    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("remove reports a file that is not there", "[contract][fs]") {
    Win32FileSystem fs;
    const auto missing = std::filesystem::temp_directory_path() / "wsldisk-no-such-file.vhdx";

    const auto status = fs.remove(missing);

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::Preflight);
}

TEST_CASE("expand_environment expands a variable Windows always sets", "[contract][fs]") {
    const Win32FileSystem fs;
    const auto expanded = fs.expand_environment("%SystemRoot%\\System32");

    REQUIRE(expanded.has_value());
    CHECK(expanded->wstring().find(L'%') == std::wstring::npos);
    CHECK(std::filesystem::exists(*expanded));
}

TEST_CASE("expand_environment leaves a path with no variables alone", "[contract][fs]") {
    const Win32FileSystem fs;
    const auto expanded = fs.expand_environment("C:\\wsl\\ext4.vhdx");

    REQUIRE(expanded.has_value());
    CHECK(*expanded == std::filesystem::path{"C:\\wsl\\ext4.vhdx"});
}

TEST_CASE("the system clock really sleeps", "[contract][clock]") {
    // The only test that calls the real Sleep. Short enough not to matter, long
    // enough that a wrapper passing zero would show up.
    const wsldisk::platform::SystemClock clock;
    const auto before = clock.now();
    clock.sleep_for(std::chrono::milliseconds{5});
    const auto elapsed = clock.now() - before;

    CHECK(elapsed >= std::chrono::milliseconds{1});
}
