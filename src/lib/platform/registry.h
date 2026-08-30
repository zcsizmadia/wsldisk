#pragma once

#include <windows.h>

#include "../interfaces.h"

namespace wsldisk::platform {

/// `IRegistry` on top of the real Win32 registry API.
///
/// Every call goes through the `Win32Api` table (`win32_api.h`) so tests can
/// force each failure branch, and each operation opens and closes its own key
/// rather than caching handles: the WSL layout has a handful of subkeys, and a
/// stale handle across a `wsl --import` would be a subtle bug for no measurable
/// gain.
class Win32Registry final : public IRegistry {
public:
    /// `root` is the hive the relative key paths are resolved against.
    /// Production uses `HKEY_CURRENT_USER`; contract tests pass the same but
    /// confine themselves to a scratch subtree.
    explicit Win32Registry(HKEY root = HKEY_CURRENT_USER) noexcept;

    [[nodiscard]] Result<std::vector<std::wstring>> subkeys(std::wstring_view key) const override;

    [[nodiscard]] Result<std::optional<std::wstring>> read_string(std::wstring_view key,
                                                                  std::wstring_view value) const override;

    [[nodiscard]] Result<std::optional<std::uint32_t>> read_dword(std::wstring_view key,
                                                                  std::wstring_view value) const override;

    [[nodiscard]] Status write_string(std::wstring_view key, std::wstring_view value,
                                      std::wstring_view data) override;

private:
    HKEY root_;
};

}  // namespace wsldisk::platform
