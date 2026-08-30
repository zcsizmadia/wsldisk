// Contract tests: Win32Registry against the real registry, confined to a scratch
// key under HKCU that is deleted again even when an assertion fails. Nothing here
// reads or writes the real Lxss key -- except one read-only enumeration, which is
// the whole point of the last test.

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <tuple>

#include "errors.h"
#include "platform/registry.h"

using wsldisk::ErrorCode;
using wsldisk::platform::Win32Registry;

namespace {

/// A key under `HKCU\Software\wsldisk-test\<pid>-<n>` that removes itself.
///
/// Setup uses the raw API on purpose: `IRegistry` deliberately cannot create or
/// delete keys, because production never needs to, and a test helper should not
/// widen the interface.
class ScratchKey {
public:
    ScratchKey() : path_(make_path()) {
        HKEY handle = nullptr;
        const LSTATUS created =
            ::RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                              KEY_ALL_ACCESS, nullptr, &handle, nullptr);
        REQUIRE(created == ERROR_SUCCESS);
        ::RegCloseKey(handle);
    }

    ~ScratchKey() {
        // Deletes the subtree; ignore the status, the test has already finished.
        ::RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
        ::RegDeleteKeyW(HKEY_CURRENT_USER, path_.c_str());
    }

    ScratchKey(const ScratchKey&) = delete;
    ScratchKey& operator=(const ScratchKey&) = delete;
    ScratchKey(ScratchKey&&) = delete;
    ScratchKey& operator=(ScratchKey&&) = delete;

    [[nodiscard]] const std::wstring& path() const noexcept { return path_; }

    /// Creates a subkey and returns its path relative to HKCU.
    [[nodiscard]] std::wstring add_subkey(const std::wstring& name) const {
        const std::wstring full = path_ + L"\\" + name;
        HKEY handle = nullptr;
        const LSTATUS created =
            ::RegCreateKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                              KEY_ALL_ACCESS, nullptr, &handle, nullptr);
        REQUIRE(created == ERROR_SUCCESS);
        ::RegCloseKey(handle);
        return full;
    }

    void set_string(const std::wstring& key, const wchar_t* name, const std::wstring& data) const {
        HKEY handle = nullptr;
        REQUIRE(::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &handle) == ERROR_SUCCESS);
        const auto bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
        const LSTATUS status =
            ::RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()), bytes);
        ::RegCloseKey(handle);
        REQUIRE(status == ERROR_SUCCESS);
    }

    void set_dword(const std::wstring& key, const wchar_t* name, DWORD data) const {
        HKEY handle = nullptr;
        REQUIRE(::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &handle) == ERROR_SUCCESS);
        const LSTATUS status =
            ::RegSetValueExW(handle, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
        ::RegCloseKey(handle);
        REQUIRE(status == ERROR_SUCCESS);
    }

private:
    static std::wstring make_path() {
        static int counter = 0;
        return L"Software\\wsldisk-test\\" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
               std::to_wstring(++counter);
    }

    std::wstring path_;
};

}  // namespace

TEST_CASE("subkeys enumerates a real key", "[contract][registry]") {
    const ScratchKey scratch;
    std::ignore = scratch.add_subkey(L"{aaaaaaaa-0000-0000-0000-000000000001}");
    std::ignore = scratch.add_subkey(L"{bbbbbbbb-0000-0000-0000-000000000002}");

    const Win32Registry registry;
    const auto names = registry.subkeys(scratch.path());

    REQUIRE(names.has_value());
    CHECK(names->size() == 2);
}

TEST_CASE("a key with no subkeys enumerates to nothing", "[contract][registry]") {
    const ScratchKey scratch;

    const Win32Registry registry;
    const auto names = registry.subkeys(scratch.path());

    REQUIRE(names.has_value());
    CHECK(names->empty());
}

TEST_CASE("a key that does not exist is a preflight failure", "[contract][registry]") {
    const Win32Registry registry;
    const auto names = registry.subkeys(L"Software\\wsldisk-test\\definitely-not-here");

    REQUIRE_FALSE(names.has_value());
    CHECK(names.error().code == ErrorCode::Preflight);
}

TEST_CASE("strings and dwords round-trip through the real registry", "[contract][registry]") {
    const ScratchKey scratch;
    const std::wstring key = scratch.add_subkey(L"{guid}");

    // The two BasePath forms that coexist on a real machine (spike #4).
    const std::wstring bare = LR"(C:\Users\example\AppData\Local\wsl\{guid})";
    const std::wstring extended = LR"(\\?\C:\Users\example\AppData\Local\Docker\wsl\main)";
    scratch.set_string(key, L"BasePath", bare);
    scratch.set_string(key, L"OtherPath", extended);
    scratch.set_dword(key, L"Flags", 15);

    const Win32Registry registry;

    const auto read_bare = registry.read_string(key, L"BasePath");
    REQUIRE(read_bare.has_value());
    REQUIRE(read_bare->has_value());
    CHECK(**read_bare == bare);

    const auto read_extended = registry.read_string(key, L"OtherPath");
    REQUIRE(read_extended.has_value());
    REQUIRE(read_extended->has_value());
    CHECK(**read_extended == extended);

    const auto flags = registry.read_dword(key, L"Flags");
    REQUIRE(flags.has_value());
    REQUIRE(flags->has_value());
    CHECK(**flags == 15);
}

TEST_CASE("an absent value reads as nothing, not as a failure", "[contract][registry]") {
    const ScratchKey scratch;
    const std::wstring key = scratch.add_subkey(L"{guid}");
    scratch.set_string(key, L"DistributionName", L"Example");

    const Win32Registry registry;

    const auto missing = registry.read_string(key, L"VhdFileName");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->has_value());

    const auto missing_number = registry.read_dword(key, L"Modern");
    REQUIRE(missing_number.has_value());
    CHECK_FALSE(missing_number->has_value());
}

TEST_CASE("reading a dword as a string is refused, and the reverse", "[contract][registry]") {
    const ScratchKey scratch;
    const std::wstring key = scratch.add_subkey(L"{guid}");
    scratch.set_string(key, L"BasePath", L"C:\\x");
    scratch.set_dword(key, L"Version", 2);

    const Win32Registry registry;
    CHECK_FALSE(registry.read_string(key, L"Version").has_value());
    CHECK_FALSE(registry.read_dword(key, L"BasePath").has_value());
}

TEST_CASE("write_string is readable back, including an extended-length path", "[contract][registry]") {
    const ScratchKey scratch;
    const std::wstring key = scratch.add_subkey(L"{guid}");
    scratch.set_string(key, L"BasePath", L"C:\\old");

    Win32Registry registry;
    const std::wstring repointed = LR"(\\?\D:\wsl\Ubuntu)";
    REQUIRE(registry.write_string(key, L"BasePath", repointed).has_value());

    const auto read_back = registry.read_string(key, L"BasePath");
    REQUIRE(read_back.has_value());
    REQUIRE(read_back->has_value());
    CHECK(**read_back == repointed);
}

TEST_CASE("an empty string round-trips", "[contract][registry]") {
    // The terminator-trimming loop must not run off the end of an empty value.
    const ScratchKey scratch;
    const std::wstring key = scratch.add_subkey(L"{guid}");

    Win32Registry registry;
    REQUIRE(registry.write_string(key, L"Empty", L"").has_value());

    const auto read_back = registry.read_string(key, L"Empty");
    REQUIRE(read_back.has_value());
    REQUIRE(read_back->has_value());
    CHECK((*read_back)->empty());
}

TEST_CASE("writing to a key that does not exist fails without creating it", "[contract][registry]") {
    Win32Registry registry;
    const std::wstring missing = L"Software\\wsldisk-test\\not-created-by-a-write";

    const auto status = registry.write_string(missing, L"BasePath", L"D:\\x");

    REQUIRE_FALSE(status.has_value());
    HKEY handle = nullptr;
    CHECK(::RegOpenKeyExW(HKEY_CURRENT_USER, missing.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS);
}

TEST_CASE("the real Lxss key can be enumerated read-only", "[contract][registry]") {
    // The one test that touches the real thing, and only to read. A runner with
    // no WSL has no such key, which is a valid outcome rather than a failure.
    const Win32Registry registry;
    const auto names = registry.subkeys(L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss");

    if (!names.has_value()) {
        CHECK(names.error().code == ErrorCode::Preflight);
        return;
    }
    // Every subkey should look like a GUID; anything else means the layout moved.
    for (const std::wstring& name : *names) {
        std::string printable;
        printable.reserve(name.size());
        for (const wchar_t character : name) {
            printable.push_back(character < 0x80 ? static_cast<char>(character) : '?');
        }
        INFO("subkey: " << printable);
        CHECK(name.starts_with(L"{"));
        CHECK(name.ends_with(L"}"));
    }
}
