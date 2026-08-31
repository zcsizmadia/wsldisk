#include "platform/filesystem.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <string>

#include "errors.h"
#include "platform/win32_api.h"

using wsldisk::ErrorCode;
using wsldisk::IFileSystem;
using wsldisk::platform::ScopedWin32Api;
using wsldisk::platform::Win32Api;
using wsldisk::platform::Win32FileSystem;

namespace {

/// A table where every entry fails; individual tests override the calls they
/// care about. Starting from "everything fails" keeps a test from accidentally
/// passing because it fell through to the real Win32.
Win32Api all_failing(DWORD error_code) {
    Win32Api api;
    api.get_compressed_file_size = [](LPCWSTR, LPDWORD) -> DWORD { return INVALID_FILE_SIZE; };
    api.get_file_attributes = [](LPCWSTR) -> DWORD { return INVALID_FILE_ATTRIBUTES; };
    api.get_file_attributes_ex = [](LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID) -> BOOL { return FALSE; };
    api.get_disk_free_space_ex = [](LPCWSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER) -> BOOL {
        return FALSE;
    };
    api.get_volume_information = [](LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR,
                                    DWORD) -> BOOL { return FALSE; };
    api.get_volume_path_name = [](LPCWSTR, LPWSTR, DWORD) -> BOOL { return FALSE; };
    api.get_last_error = [error_code]() { return error_code; };
    return api;
}

/// Fills the caller buffer with `text`, the way the real Win32 calls do.
void write_buffer(LPWSTR buffer, DWORD size, const wchar_t* text) {
    const std::size_t length = std::wcslen(text);
    REQUIRE(length + 1 <= size);
    std::memcpy(buffer, text, (length + 1) * sizeof(wchar_t));
}

/// A table where the volume calls all succeed, describing an NTFS volume.
Win32Api healthy_volume() {
    Win32Api api = all_failing(NO_ERROR);
    api.get_volume_path_name = [](LPCWSTR, LPWSTR buffer, DWORD size) -> BOOL {
        write_buffer(buffer, size, L"D:\\");
        return TRUE;
    };
    api.get_disk_free_space_ex = [](LPCWSTR, PULARGE_INTEGER free_to_caller, PULARGE_INTEGER total,
                                    PULARGE_INTEGER total_free) -> BOOL {
        free_to_caller->QuadPart = 42;
        total->QuadPart = 1000;
        total_free->QuadPart = 50;
        return TRUE;
    };
    api.get_volume_information = [](LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR filesystem_name,
                                    DWORD size) -> BOOL {
        write_buffer(filesystem_name, size, L"NTFS");
        return TRUE;
    };
    return api;
}

}  // namespace

TEST_CASE("exists reflects whether the attribute query succeeded", "[platform][filesystem]") {
    const Win32FileSystem fs;

    SECTION("a readable path exists") {
        Win32Api api = all_failing(NO_ERROR);
        api.get_file_attributes = [](LPCWSTR) -> DWORD { return FILE_ATTRIBUTE_NORMAL; };
        const ScopedWin32Api scoped{api};
        CHECK(fs.exists("C:\\present.vhdx"));
    }

    SECTION("an unreadable path does not") {
        const ScopedWin32Api scoped{all_failing(ERROR_FILE_NOT_FOUND)};
        CHECK_FALSE(fs.exists("C:\\missing.vhdx"));
    }
}

TEST_CASE("file_size joins the two halves Win32 returns", "[platform][filesystem]") {
    const Win32FileSystem fs;

    SECTION("sizes above 4 GiB survive the round trip") {
        Win32Api api = all_failing(NO_ERROR);
        api.get_file_attributes_ex = [](LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID information) -> BOOL {
            auto* data = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(information);
            data->nFileSizeHigh = 3;
            data->nFileSizeLow = 7;
            return TRUE;
        };
        const ScopedWin32Api scoped{api};
        const auto size = fs.file_size("C:\\big.vhdx");
        REQUIRE(size.has_value());
        CHECK(*size == (std::uint64_t{3} << 32) + 7);
    }

    SECTION("a failure carries the Win32 code and a remedy") {
        const ScopedWin32Api scoped{all_failing(ERROR_ACCESS_DENIED)};
        const auto size = fs.file_size("C:\\denied.vhdx");
        REQUIRE_FALSE(size.has_value());
        CHECK(size.error().code == ErrorCode::NeedsElevation);
        CHECK(size.error().message.find("denied.vhdx") != std::string::npos);
    }
}

TEST_CASE("file_size_on_disk tells a 4 GiB boundary apart from a failure", "[platform][filesystem]") {
    const Win32FileSystem fs;

    SECTION("an ordinary size is returned as is") {
        Win32Api api = all_failing(NO_ERROR);
        api.get_compressed_file_size = [](LPCWSTR, LPDWORD high) -> DWORD {
            *high = 1;
            return 4096;
        };
        const ScopedWin32Api scoped{api};
        const auto size = fs.file_size_on_disk("C:\\sparse.vhdx");
        REQUIRE(size.has_value());
        CHECK(*size == (std::uint64_t{1} << 32) + 4096);
    }

    SECTION("INVALID_FILE_SIZE with no error is a real size, not a failure") {
        // A file whose low dword happens to be 0xFFFFFFFF is legal; only
        // GetLastError distinguishes it from an error.
        Win32Api api = all_failing(NO_ERROR);
        api.get_compressed_file_size = [](LPCWSTR, LPDWORD high) -> DWORD {
            *high = 0;
            return INVALID_FILE_SIZE;
        };
        const ScopedWin32Api scoped{api};
        const auto size = fs.file_size_on_disk("C:\\odd.vhdx");
        REQUIRE(size.has_value());
        CHECK(*size == INVALID_FILE_SIZE);
    }

    SECTION("INVALID_FILE_SIZE with an error is a failure") {
        const ScopedWin32Api scoped{all_failing(ERROR_SHARING_VIOLATION)};
        const auto size = fs.file_size_on_disk("C:\\locked.vhdx");
        REQUIRE_FALSE(size.has_value());
        CHECK(size.error().code == ErrorCode::DistroBusy);
    }
}

TEST_CASE("is_sparse reads the sparse attribute", "[platform][filesystem]") {
    const Win32FileSystem fs;

    SECTION("a sparse file reports true") {
        Win32Api api = all_failing(NO_ERROR);
        api.get_file_attributes = [](LPCWSTR) -> DWORD {
            return FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_SPARSE_FILE;
        };
        const ScopedWin32Api scoped{api};
        const auto sparse = fs.is_sparse("C:\\sparse.vhdx");
        REQUIRE(sparse.has_value());
        CHECK(*sparse);
    }

    SECTION("a dense file reports false") {
        Win32Api api = all_failing(NO_ERROR);
        api.get_file_attributes = [](LPCWSTR) -> DWORD { return FILE_ATTRIBUTE_NORMAL; };
        const ScopedWin32Api scoped{api};
        const auto sparse = fs.is_sparse("C:\\dense.vhdx");
        REQUIRE(sparse.has_value());
        CHECK_FALSE(*sparse);
    }

    SECTION("an unreadable file is a failure") {
        const ScopedWin32Api scoped{all_failing(ERROR_PATH_NOT_FOUND)};
        const auto sparse = fs.is_sparse("C:\\nowhere\\x.vhdx");
        REQUIRE_FALSE(sparse.has_value());
        CHECK(sparse.error().code == ErrorCode::Preflight);
    }
}

TEST_CASE("volume_info describes the volume holding a path", "[platform][filesystem]") {
    const Win32FileSystem fs;

    SECTION("an NTFS volume is reported with its free space") {
        const ScopedWin32Api scoped{healthy_volume()};
        const auto info = fs.volume_info("D:\\wsl\\ext4.vhdx");
        REQUIRE(info.has_value());
        CHECK(info->filesystem_name == "NTFS");
        CHECK(info->total_bytes == 1000);
        CHECK(info->free_bytes == 42);
        CHECK(info->supports_vhdx());
    }

    SECTION("a non-ASCII filesystem name is sanitised rather than guessed at") {
        Win32Api api = healthy_volume();
        api.get_volume_information = [](LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD,
                                        LPWSTR filesystem_name, DWORD size) -> BOOL {
            write_buffer(filesystem_name, size, L"NT\u00c4S");
            return TRUE;
        };
        const ScopedWin32Api scoped{api};
        const auto info = fs.volume_info("D:\\wsl\\ext4.vhdx");
        REQUIRE(info.has_value());
        CHECK(info->filesystem_name == "NT?S");
        CHECK_FALSE(info->supports_vhdx());
    }

    SECTION("failing to resolve the volume is a failure") {
        const ScopedWin32Api scoped{all_failing(ERROR_INVALID_DRIVE)};
        const auto info = fs.volume_info("Q:\\ext4.vhdx");
        REQUIRE_FALSE(info.has_value());
        CHECK(info.error().code == ErrorCode::Preflight);
        CHECK(info.error().message.find("resolve the volume") != std::string::npos);
    }

    SECTION("failing to read free space is a failure") {
        Win32Api api = healthy_volume();
        api.get_disk_free_space_ex = [](LPCWSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER) -> BOOL {
            return FALSE;
        };
        api.get_last_error = []() -> DWORD { return ERROR_NOT_READY; };
        const ScopedWin32Api scoped{api};
        const auto info = fs.volume_info("D:\\wsl\\ext4.vhdx");
        REQUIRE_FALSE(info.has_value());
        CHECK(info.error().message.find("free space on D:\\") != std::string::npos);
    }

    SECTION("failing to read volume information is a failure") {
        Win32Api api = healthy_volume();
        api.get_volume_information = [](LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR,
                                        DWORD) -> BOOL { return FALSE; };
        api.get_last_error = []() -> DWORD { return ERROR_NOT_SUPPORTED; };
        const ScopedWin32Api scoped{api};
        const auto info = fs.volume_info("D:\\wsl\\ext4.vhdx");
        REQUIRE_FALSE(info.has_value());
        CHECK(info.error().code == ErrorCode::Generic);
        CHECK(info.error().message.find("volume information") != std::string::npos);
    }
}

TEST_CASE("Win32FileSystem can be owned and destroyed through the interface", "[platform][filesystem]") {
    // Operations hold an IFileSystem&, and something has to own the concrete
    // object; a non-virtual destructor here would leak or corrupt on teardown.
    std::unique_ptr<IFileSystem> fs = std::make_unique<Win32FileSystem>();

    Win32Api api = all_failing(NO_ERROR);
    api.get_file_attributes = [](LPCWSTR) -> DWORD { return FILE_ATTRIBUTE_NORMAL; };
    const ScopedWin32Api scoped{api};
    CHECK(fs->exists("C:\\anything"));

    fs.reset();
    CHECK(fs == nullptr);
}

TEST_CASE("ReFS is an acceptable home for a VHDX, FAT is not", "[platform][filesystem]") {
    CHECK(wsldisk::VolumeInfo{.filesystem_name = "ReFS"}.supports_vhdx());
    CHECK_FALSE(wsldisk::VolumeInfo{.filesystem_name = "exFAT"}.supports_vhdx());
}

TEST_CASE("the injected table is restored when the scope ends", "[platform][win32api]") {
    const Win32FileSystem fs;
    {
        const ScopedWin32Api scoped{all_failing(ERROR_FILE_NOT_FOUND)};
        CHECK_FALSE(fs.exists("C:\\anything"));
        {
            Win32Api inner = all_failing(NO_ERROR);
            inner.get_file_attributes = [](LPCWSTR) -> DWORD { return FILE_ATTRIBUTE_NORMAL; };
            const ScopedWin32Api nested{inner};
            CHECK(fs.exists("C:\\anything"));
        }
        CHECK_FALSE(fs.exists("C:\\anything"));
    }
    // Back to the real API: the Windows directory is always there.
    CHECK(fs.exists("C:\\Windows"));
}

TEST_CASE("exists reports a file it is not allowed to look at as present", "[platform][fs]") {
    // `GetFileAttributesW` fails with ERROR_ACCESS_DENIED when a parent
    // directory denies traverse -- the file is there, we just cannot look. That
    // used to read as "does not exist", so `relink` told the user to check the
    // path when the answer was permissions.
    //
    // FakeFileSystem::exists is plain map membership, so no test using the fake
    // could see this. That is the fourth time a fake has agreed with a bug here.
    Win32Api api;
    api.get_file_attributes = [](LPCWSTR) -> DWORD { return INVALID_FILE_ATTRIBUTES; };
    api.get_last_error = []() -> DWORD { return ERROR_ACCESS_DENIED; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem filesystem;

    CHECK(filesystem.exists(R"(C:\denied\ext4.vhdx)"));
}

TEST_CASE("exists reports a file that is genuinely absent as absent", "[platform][fs]") {
    Win32Api api;
    api.get_file_attributes = [](LPCWSTR) -> DWORD { return INVALID_FILE_ATTRIBUTES; };
    api.get_last_error = []() -> DWORD { return ERROR_FILE_NOT_FOUND; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem filesystem;

    CHECK_FALSE(filesystem.exists(R"(C:\gone\ext4.vhdx)"));
}

TEST_CASE("exists treats a malformed path as absent rather than present", "[platform][fs]") {
    Win32Api api;
    api.get_file_attributes = [](LPCWSTR) -> DWORD { return INVALID_FILE_ATTRIBUTES; };
    api.get_last_error = []() -> DWORD { return ERROR_INVALID_NAME; };
    const ScopedWin32Api scoped{api};

    const Win32FileSystem filesystem;

    CHECK_FALSE(filesystem.exists("not a path"));
}
