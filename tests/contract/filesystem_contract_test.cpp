// Contract tests: the platform wrappers against the real Win32 API, on real
// files under %TEMP%. No WSL and no elevation required; everything created here
// is removed again even when an assertion fails.

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "errors.h"
#include "platform/filesystem.h"

using wsldisk::ErrorCode;
using wsldisk::platform::Win32FileSystem;

namespace {

/// A file under %TEMP% that deletes itself, whatever the test does.
class TempFile {
public:
    explicit TempFile(std::uint64_t bytes) : path_(make_path()) {
        std::ofstream stream(path_, std::ios::binary);
        REQUIRE(stream.good());
        const std::vector<char> block(4096, 'x');
        for (std::uint64_t written = 0; written < bytes; written += block.size()) {
            stream.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
        stream.close();
        REQUIRE(std::filesystem::exists(path_));
    }

    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    static std::filesystem::path make_path() {
        static int counter = 0;
        return std::filesystem::temp_directory_path() /
               ("wsldisk-contract-" + std::to_string(::GetCurrentProcessId()) + "-" +
                std::to_string(++counter) + ".bin");
    }

    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("sizes of a real file match what the OS reports", "[contract][filesystem]") {
    const Win32FileSystem fs;
    const TempFile file{16 * 4096};

    CHECK(fs.exists(file.path()));

    const auto logical = fs.file_size(file.path());
    REQUIRE(logical.has_value());
    CHECK(*logical == 16 * 4096);

    const auto on_disk = fs.file_size_on_disk(file.path());
    REQUIRE(on_disk.has_value());
    // A dense file occupies at least its logical size, rounded up to a cluster.
    CHECK(*on_disk >= *logical);

    const auto sparse = fs.is_sparse(file.path());
    REQUIRE(sparse.has_value());
    CHECK_FALSE(*sparse);
}

TEST_CASE("an empty file is reported as zero bytes", "[contract][filesystem]") {
    const Win32FileSystem fs;
    const TempFile file{0};

    const auto logical = fs.file_size(file.path());
    REQUIRE(logical.has_value());
    CHECK(*logical == 0);

    const auto on_disk = fs.file_size_on_disk(file.path());
    REQUIRE(on_disk.has_value());
    CHECK(*on_disk == 0);
}

TEST_CASE("a path that does not exist reports a preflight failure", "[contract][filesystem]") {
    const Win32FileSystem fs;
    const auto missing = std::filesystem::temp_directory_path() / "wsldisk-does-not-exist.vhdx";
    REQUIRE_FALSE(std::filesystem::exists(missing));

    CHECK_FALSE(fs.exists(missing));

    const auto logical = fs.file_size(missing);
    REQUIRE_FALSE(logical.has_value());
    CHECK(logical.error().code == ErrorCode::Preflight);

    const auto on_disk = fs.file_size_on_disk(missing);
    REQUIRE_FALSE(on_disk.has_value());
    CHECK(on_disk.error().code == ErrorCode::Preflight);

    const auto sparse = fs.is_sparse(missing);
    REQUIRE_FALSE(sparse.has_value());
    CHECK(sparse.error().code == ErrorCode::Preflight);
}

TEST_CASE("the volume holding %TEMP% is described", "[contract][filesystem]") {
    const Win32FileSystem fs;
    const auto info = fs.volume_info(std::filesystem::temp_directory_path());
    REQUIRE(info.has_value());

    CHECK_FALSE(info->filesystem_name.empty());
    CHECK(info->total_bytes > 0);
    CHECK(info->free_bytes <= info->total_bytes);
    // CI and developer machines run on NTFS or ReFS; anything else would mean a
    // VHDX could not live there, which is worth failing loudly on.
    CHECK(info->supports_vhdx());
}

TEST_CASE("volume_info on a drive letter that is not mounted fails", "[contract][filesystem]") {
    const Win32FileSystem fs;
    // GetVolumePathName still resolves the syntax, so the failure surfaces from
    // the free-space query; either way the caller gets an Error, not a crash.
    const auto info = fs.volume_info("Q:\\definitely\\not\\mounted\\ext4.vhdx");
    if (info.has_value()) {
        // A machine really does have a Q: drive; the data must still be sane.
        CHECK(info->total_bytes > 0);
    } else {
        // Whatever Windows says, the caller gets a described failure, not a crash.
        CHECK_FALSE(info.error().message.empty());
    }
}
