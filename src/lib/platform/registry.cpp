#include "registry.h"

#include <windows.h>

#include <array>
#include <format>
#include <string>

#include "win32_api.h"
#include "win32_error.h"

namespace wsldisk::platform {
namespace {

/// A registry key name is at most 255 characters, so enumeration never needs a
/// growable buffer -- unlike values, which do.
constexpr DWORD max_key_name_length = 256;

/// Closes an `HKEY` through the injection table, so a test's fake table sees the
/// close as well as the open. `wil::unique_hkey` would call `RegCloseKey`
/// directly and step around the table, which is the one invariant `platform/`
/// keeps.
class ScopedKey {
public:
    ScopedKey() = default;

    ~ScopedKey() {
        if (key_ != nullptr) {
            std::ignore = win32().reg_close_key(key_);
        }
    }

    ScopedKey(const ScopedKey&) = delete;
    ScopedKey& operator=(const ScopedKey&) = delete;
    ScopedKey(ScopedKey&&) = delete;
    ScopedKey& operator=(ScopedKey&&) = delete;

    [[nodiscard]] PHKEY put() noexcept { return &key_; }

    [[nodiscard]] HKEY get() const noexcept { return key_; }

private:
    HKEY key_ = nullptr;
};

/// Registry paths are ASCII in practice, and only ever appear inside diagnostics.
[[nodiscard]] std::string describe(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        result.push_back(character < 0x80 ? static_cast<char>(character) : '?');
    }
    return result;
}

/// `RegSetValueEx` and friends want a NUL-terminated string; a `wstring_view`
/// need not be one.
[[nodiscard]] std::wstring terminated(std::wstring_view text) {
    return std::wstring{text};
}

/// A value read back from the registry may or may not include its terminator,
/// and `RegQueryValueEx` reports the stored byte count either way.
void trim_trailing_nuls(std::wstring& text) {
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
}

}  // namespace

Win32Registry::Win32Registry(HKEY root) noexcept : root_(root) {}

Result<std::vector<std::wstring>> Win32Registry::subkeys(std::wstring_view key) const {
    ScopedKey handle;
    const LSTATUS opened = win32().reg_open_key_ex(root_, terminated(key).c_str(), 0, KEY_READ, handle.put());
    if (opened != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(static_cast<DWORD>(opened),
                                                std::format("open the registry key {}", describe(key))));
    }

    std::vector<std::wstring> names;
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, max_key_name_length> buffer{};
        auto length = static_cast<DWORD>(buffer.size());
        const LSTATUS status = win32().reg_enum_key_ex(handle.get(), index, buffer.data(), &length, nullptr,
                                                       nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            return std::unexpected(error_from_win32(
                static_cast<DWORD>(status), std::format("enumerate the subkeys of {}", describe(key))));
        }
        names.emplace_back(buffer.data(), length);
    }
    return names;
}

Result<std::optional<std::wstring>> Win32Registry::read_string(std::wstring_view key,
                                                               std::wstring_view value) const {
    ScopedKey handle;
    const LSTATUS opened = win32().reg_open_key_ex(root_, terminated(key).c_str(), 0, KEY_READ, handle.put());
    if (opened != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(static_cast<DWORD>(opened),
                                                std::format("open the registry key {}", describe(key))));
    }

    const std::wstring value_name = terminated(value);
    DWORD type = 0;
    DWORD bytes = 0;
    const LSTATUS sized =
        win32().reg_query_value_ex(handle.get(), value_name.c_str(), nullptr, &type, nullptr, &bytes);
    if (sized == ERROR_FILE_NOT_FOUND) {
        return std::optional<std::wstring>{};
    }
    if (sized != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(
            static_cast<DWORD>(sized), std::format("read {} from {}", describe(value), describe(key))));
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        return fail(
            ErrorCode::Generic,
            std::format("{} in {} is not a string (registry type {})", describe(value), describe(key), type),
            "the WSL registry layout has changed; please report this with `wsl --version`");
    }

    std::wstring data(bytes / sizeof(wchar_t), L'\0');
    const LSTATUS read = win32().reg_query_value_ex(handle.get(), value_name.c_str(), nullptr, &type,
                                                    reinterpret_cast<LPBYTE>(data.data()), &bytes);
    if (read != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(
            static_cast<DWORD>(read), std::format("read {} from {}", describe(value), describe(key))));
    }
    trim_trailing_nuls(data);
    return std::optional{data};
}

Result<std::optional<std::uint32_t>> Win32Registry::read_dword(std::wstring_view key,
                                                               std::wstring_view value) const {
    ScopedKey handle;
    const LSTATUS opened = win32().reg_open_key_ex(root_, terminated(key).c_str(), 0, KEY_READ, handle.put());
    if (opened != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(static_cast<DWORD>(opened),
                                                std::format("open the registry key {}", describe(key))));
    }

    DWORD type = 0;
    DWORD data = 0;
    DWORD bytes = sizeof(data);
    const LSTATUS read = win32().reg_query_value_ex(handle.get(), terminated(value).c_str(), nullptr, &type,
                                                    reinterpret_cast<LPBYTE>(&data), &bytes);
    if (read == ERROR_FILE_NOT_FOUND) {
        return std::optional<std::uint32_t>{};
    }
    if (read != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(
            static_cast<DWORD>(read), std::format("read {} from {}", describe(value), describe(key))));
    }
    if (type != REG_DWORD) {
        return fail(
            ErrorCode::Generic,
            std::format("{} in {} is not a DWORD (registry type {})", describe(value), describe(key), type),
            "the WSL registry layout has changed; please report this with `wsl --version`");
    }
    return std::optional{static_cast<std::uint32_t>(data)};
}

Status Win32Registry::write_string(std::wstring_view key, std::wstring_view value, std::wstring_view data) {
    ScopedKey handle;
    const LSTATUS opened =
        win32().reg_open_key_ex(root_, terminated(key).c_str(), 0, KEY_SET_VALUE, handle.put());
    if (opened != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(
            static_cast<DWORD>(opened), std::format("open the registry key {} for writing", describe(key))));
    }

    // The stored length includes the terminator: WSL's own reader expects it.
    const std::wstring payload = terminated(data);
    const auto bytes = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
    const LSTATUS written = win32().reg_set_value_ex(handle.get(), terminated(value).c_str(), 0, REG_SZ,
                                                     reinterpret_cast<const BYTE*>(payload.c_str()), bytes);
    if (written != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(
            static_cast<DWORD>(written), std::format("write {} to {}", describe(value), describe(key))));
    }
    return {};
}

}  // namespace wsldisk::platform
