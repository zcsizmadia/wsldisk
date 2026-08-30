#include "platform/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "errors.h"
#include "platform/win32_api.h"

using wsldisk::ErrorCode;
using wsldisk::platform::ScopedWin32Api;
using wsldisk::platform::Win32Api;
using wsldisk::platform::Win32Registry;

namespace {

/// A table where every registry call fails; each test overrides only what it
/// needs, so nothing passes by accidentally reaching the real registry.
Win32Api all_failing(LSTATUS status) {
    Win32Api api;
    api.reg_open_key_ex = [status](HKEY, LPCWSTR, DWORD, REGSAM, PHKEY) { return status; };
    api.reg_close_key = [](HKEY) -> LSTATUS { return ERROR_SUCCESS; };
    api.reg_enum_key_ex = [status](HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR, LPDWORD, PFILETIME) {
        return status;
    };
    api.reg_query_value_ex = [status](HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return status; };
    api.reg_set_value_ex = [status](HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD) { return status; };
    return api;
}

/// A table whose keys open successfully; reads still fail unless overridden.
Win32Api opens_ok(LSTATUS read_status = ERROR_FILE_NOT_FOUND) {
    Win32Api api = all_failing(read_status);
    api.reg_open_key_ex = [](HKEY, LPCWSTR, DWORD, REGSAM, PHKEY result) -> LSTATUS {
        // Any non-null handle will do; the fake never dereferences it.
        *result = reinterpret_cast<HKEY>(1);
        return ERROR_SUCCESS;
    };
    return api;
}

/// Copies a string into the caller's buffer the way RegQueryValueEx does: a
/// null buffer means "tell me the size".
LSTATUS serve_string(std::wstring_view text, DWORD registry_type, LPDWORD type, LPBYTE data, LPDWORD bytes) {
    const auto needed = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    if (type != nullptr) {
        *type = registry_type;
    }
    if (data == nullptr) {
        *bytes = needed;
        return ERROR_SUCCESS;
    }
    if (*bytes < needed) {
        return ERROR_MORE_DATA;
    }
    std::memcpy(data, text.data(), text.size() * sizeof(wchar_t));
    reinterpret_cast<wchar_t*>(data)[text.size()] = L'\0';
    *bytes = needed;
    return ERROR_SUCCESS;
}

}  // namespace

TEST_CASE("subkeys enumerates until there are no more items", "[platform][registry]") {
    Win32Api api = opens_ok();
    api.reg_enum_key_ex = [](HKEY, DWORD index, LPWSTR name, LPDWORD length, LPDWORD, LPWSTR, LPDWORD,
                             PFILETIME) -> LSTATUS {
        static constexpr std::array<const wchar_t*, 2> names{L"{aaaa}", L"{bbbb}"};
        if (index >= names.size()) {
            return ERROR_NO_MORE_ITEMS;
        }
        const std::wstring_view text{names.at(index)};
        std::memcpy(name, text.data(), text.size() * sizeof(wchar_t));
        *length = static_cast<DWORD>(text.size());
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32Registry registry;
    const auto names = registry.subkeys(L"Lxss");

    REQUIRE(names.has_value());
    CHECK(*names == std::vector<std::wstring>{L"{aaaa}", L"{bbbb}"});
}

TEST_CASE("subkeys reports a key that cannot be opened", "[platform][registry]") {
    const ScopedWin32Api scoped{all_failing(ERROR_FILE_NOT_FOUND)};

    const Win32Registry registry;
    const auto names = registry.subkeys(L"Lxss");

    REQUIRE_FALSE(names.has_value());
    CHECK(names.error().code == ErrorCode::Preflight);
    CHECK(names.error().message.find("Lxss") != std::string::npos);
}

TEST_CASE("subkeys reports a failure part-way through enumeration", "[platform][registry]") {
    // A key deleted by another process mid-enumeration is the realistic case.
    Win32Api api = opens_ok();
    api.reg_enum_key_ex = [](HKEY, DWORD index, LPWSTR name, LPDWORD length, LPDWORD, LPWSTR, LPDWORD,
                             PFILETIME) -> LSTATUS {
        if (index == 0) {
            const std::wstring_view text{L"{aaaa}"};
            std::memcpy(name, text.data(), text.size() * sizeof(wchar_t));
            *length = static_cast<DWORD>(text.size());
            return ERROR_SUCCESS;
        }
        return ERROR_ACCESS_DENIED;
    };
    const ScopedWin32Api scoped{api};

    const Win32Registry registry;
    const auto names = registry.subkeys(L"Lxss");

    REQUIRE_FALSE(names.has_value());
    CHECK(names.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("read_string returns the stored text", "[platform][registry]") {
    Win32Api api = opens_ok();
    api.reg_query_value_ex = [](HKEY, LPCWSTR, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD bytes) {
        return serve_string(LR"(\\?\C:\Users\example\AppData\Local\Docker\wsl\main)", REG_SZ, type, data,
                            bytes);
    };
    const ScopedWin32Api scoped{api};

    const Win32Registry registry;
    const auto value = registry.read_string(L"Lxss\\{guid}", L"BasePath");

    REQUIRE(value.has_value());
    REQUIRE(value->has_value());
    // The extended-length prefix survives: move and relink write back what they read.
    CHECK(**value == LR"(\\?\C:\Users\example\AppData\Local\Docker\wsl\main)");
}

TEST_CASE("read_string accepts REG_EXPAND_SZ unexpanded", "[platform][registry]") {
    Win32Api api = opens_ok();
    api.reg_query_value_ex = [](HKEY, LPCWSTR, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD bytes) {
        return serve_string(LR"(%LOCALAPPDATA%\wsl)", REG_EXPAND_SZ, type, data, bytes);
    };
    const ScopedWin32Api scoped{api};

    const Win32Registry registry;
    const auto value = registry.read_string(L"Lxss\\{guid}", L"BasePath");

    REQUIRE(value.has_value());
    REQUIRE(value->has_value());
    CHECK(**value == LR"(%LOCALAPPDATA%\wsl)");
}

TEST_CASE("a value that does not exist is not an error", "[platform][registry]") {
    // VhdFileName is genuinely absent on the legacy packaged layout, so the
    // caller has to be able to tell "missing" from "failed".
    const ScopedWin32Api scoped{opens_ok(ERROR_FILE_NOT_FOUND)};

    const Win32Registry registry;

    const auto text = registry.read_string(L"Lxss\\{guid}", L"VhdFileName");
    REQUIRE(text.has_value());
    CHECK_FALSE(text->has_value());

    const auto number = registry.read_dword(L"Lxss\\{guid}", L"Modern");
    REQUIRE(number.has_value());
    CHECK_FALSE(number->has_value());
}

TEST_CASE("read_string rejects a value of the wrong type", "[platform][registry]") {
    Win32Api api = opens_ok();
    api.reg_query_value_ex = [](HKEY, LPCWSTR, LPDWORD, LPDWORD type, LPBYTE, LPDWORD bytes) {
        *type = REG_DWORD;
        *bytes = sizeof(DWORD);
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32Registry registry;
    const auto value = registry.read_string(L"Lxss\\{guid}", L"Version");

    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code == ErrorCode::Generic);
    CHECK(value.error().remedy.find("wsl --version") != std::string::npos);
}

TEST_CASE("read_string reports a failure on either query", "[platform][registry]") {
    SECTION("the sizing call fails") {
        const ScopedWin32Api scoped{opens_ok(ERROR_ACCESS_DENIED)};
        const Win32Registry registry;
        const auto value = registry.read_string(L"Lxss\\{guid}", L"BasePath");
        REQUIRE_FALSE(value.has_value());
        CHECK(value.error().code == ErrorCode::NeedsElevation);
    }

    SECTION("the read call fails after sizing succeeded") {
        // The value shrank between the two calls, which is what ERROR_MORE_DATA
        // would mean here.
        Win32Api api = opens_ok();
        int calls = 0;
        api.reg_query_value_ex = [&calls](HKEY, LPCWSTR, LPDWORD, LPDWORD type, LPBYTE data,
                                          LPDWORD bytes) -> LSTATUS {
            ++calls;
            if (data == nullptr) {
                *type = REG_SZ;
                *bytes = 64;
                return ERROR_SUCCESS;
            }
            return ERROR_MORE_DATA;
        };
        const ScopedWin32Api scoped{api};

        const Win32Registry registry;
        const auto value = registry.read_string(L"Lxss\\{guid}", L"BasePath");

        REQUIRE_FALSE(value.has_value());
        CHECK(calls == 2);
    }

    SECTION("the key cannot be opened") {
        const ScopedWin32Api scoped{all_failing(ERROR_FILE_NOT_FOUND)};
        const Win32Registry registry;
        CHECK_FALSE(registry.read_string(L"Lxss\\{guid}", L"BasePath").has_value());
    }
}

TEST_CASE("a stored string keeps its terminator out of the result", "[platform][registry]") {
    // RegQueryValueEx reports the byte count including the NUL, and some writers
    // store more than one.
    Win32Api api = opens_ok();
    api.reg_query_value_ex = [](HKEY, LPCWSTR, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD bytes) -> LSTATUS {
        static constexpr std::array<wchar_t, 8> stored{L'e', L'x', L't', L'4', L'\0', L'\0'};
        const auto needed = static_cast<DWORD>(stored.size() * sizeof(wchar_t));
        *type = REG_SZ;
        if (data == nullptr) {
            *bytes = needed;
            return ERROR_SUCCESS;
        }
        std::memcpy(data, stored.data(), needed);
        *bytes = needed;
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32Registry registry;
    const auto value = registry.read_string(L"Lxss\\{guid}", L"VhdFileName");

    REQUIRE(value.has_value());
    REQUIRE(value->has_value());
    CHECK(**value == L"ext4");
}

TEST_CASE("read_dword returns the stored number", "[platform][registry]") {
    Win32Api api = opens_ok();
    api.reg_query_value_ex = [](HKEY, LPCWSTR, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD bytes) -> LSTATUS {
        *type = REG_DWORD;
        const DWORD stored = 15;
        std::memcpy(data, &stored, sizeof(stored));
        *bytes = sizeof(stored);
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32Registry registry;
    const auto value = registry.read_dword(L"Lxss\\{guid}", L"Flags");

    REQUIRE(value.has_value());
    REQUIRE(value->has_value());
    CHECK(**value == 15);
}

TEST_CASE("read_dword rejects a value of the wrong type", "[platform][registry]") {
    Win32Api api = opens_ok();
    api.reg_query_value_ex = [](HKEY, LPCWSTR, LPDWORD, LPDWORD type, LPBYTE, LPDWORD bytes) {
        *type = REG_SZ;
        *bytes = sizeof(DWORD);
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    const Win32Registry registry;
    const auto value = registry.read_dword(L"Lxss\\{guid}", L"BasePath");

    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code == ErrorCode::Generic);
}

TEST_CASE("read_dword reports open and read failures", "[platform][registry]") {
    SECTION("the key cannot be opened") {
        const ScopedWin32Api scoped{all_failing(ERROR_FILE_NOT_FOUND)};
        const Win32Registry registry;
        CHECK_FALSE(registry.read_dword(L"Lxss\\{guid}", L"Version").has_value());
    }

    SECTION("the read fails") {
        const ScopedWin32Api scoped{opens_ok(ERROR_ACCESS_DENIED)};
        const Win32Registry registry;
        const auto value = registry.read_dword(L"Lxss\\{guid}", L"Version");
        REQUIRE_FALSE(value.has_value());
        CHECK(value.error().code == ErrorCode::NeedsElevation);
    }
}

TEST_CASE("write_string stores the text with its terminator", "[platform][registry]") {
    Win32Api api = opens_ok();
    DWORD written_bytes = 0;
    DWORD written_type = 0;
    std::wstring written_text;
    api.reg_set_value_ex = [&](HKEY, LPCWSTR, DWORD, DWORD type, const BYTE* data, DWORD bytes) -> LSTATUS {
        written_type = type;
        written_bytes = bytes;
        written_text = reinterpret_cast<const wchar_t*>(data);
        return ERROR_SUCCESS;
    };
    const ScopedWin32Api scoped{api};

    Win32Registry registry;
    const auto status = registry.write_string(L"Lxss\\{guid}", L"BasePath", LR"(D:\wsl\Ubuntu)");

    REQUIRE(status.has_value());
    CHECK(written_type == REG_SZ);
    CHECK(written_text == LR"(D:\wsl\Ubuntu)");
    // WSL's own reader expects the terminator to be counted.
    CHECK(written_bytes == (written_text.size() + 1) * sizeof(wchar_t));
}

TEST_CASE("write_string reports open and write failures", "[platform][registry]") {
    SECTION("the key cannot be opened for writing") {
        const ScopedWin32Api scoped{all_failing(ERROR_ACCESS_DENIED)};
        Win32Registry registry;
        const auto status = registry.write_string(L"Lxss\\{guid}", L"BasePath", L"D:\\x");
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::NeedsElevation);
        CHECK(status.error().message.find("for writing") != std::string::npos);
    }

    SECTION("the write itself fails") {
        Win32Api api = opens_ok();
        api.reg_set_value_ex = [](HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD) -> LSTATUS {
            return ERROR_ACCESS_DENIED;
        };
        const ScopedWin32Api scoped{api};
        Win32Registry registry;
        const auto status = registry.write_string(L"Lxss\\{guid}", L"BasePath", L"D:\\x");
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::NeedsElevation);
    }
}

TEST_CASE("a non-ASCII key name is still describable in a diagnostic", "[platform][registry]") {
    // Distribution names can contain anything; the message must stay printable.
    const ScopedWin32Api scoped{all_failing(ERROR_FILE_NOT_FOUND)};

    const Win32Registry registry;
    const auto names = registry.subkeys(L"Lxss\\\u00dcbuntu");

    REQUIRE_FALSE(names.has_value());
    CHECK(names.error().message.find("Lxss\\?buntu") != std::string::npos);
}
