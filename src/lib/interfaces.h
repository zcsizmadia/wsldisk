#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "errors.h"

namespace wsldisk {

/// What a volume reports about itself. Sizes are bytes.
struct VolumeInfo {
    /// Filesystem name as Windows reports it: `NTFS`, `ReFS`, `exFAT`, `FAT32`.
    std::string filesystem_name;
    /// Total size of the volume.
    std::uint64_t total_bytes = 0;
    /// Bytes available to the calling user (respects quotas).
    std::uint64_t free_bytes = 0;

    /// Whether the volume supports sparse files and >4 GiB files, i.e. whether a
    /// VHDX may live on it. FAT and exFAT do not qualify.
    [[nodiscard]] bool supports_vhdx() const noexcept {
        return filesystem_name == "NTFS" || filesystem_name == "ReFS";
    }
};

/// Read access to the WSL registry, plus the one write `relink` needs.
///
/// Key paths are relative to a root the implementation holds (`HKEY_CURRENT_USER`
/// in production, a scratch key in the contract tests), so nothing here has to
/// name a hive.
///
/// Everything is UTF-16 because the registry is: distribution names and paths
/// keep their original encoding until the model layer converts them for display.
/// A value that does not exist is `std::nullopt`, which is not the same as a read
/// that failed -- the WSL layout has genuinely optional values (`VhdFileName` is
/// absent on the legacy packaged layout) and the caller has to tell them apart.
class IRegistry {
public:
    IRegistry() = default;
    IRegistry(const IRegistry&) = delete;
    IRegistry& operator=(const IRegistry&) = delete;
    IRegistry(IRegistry&&) = delete;
    IRegistry& operator=(IRegistry&&) = delete;
    virtual ~IRegistry() = default;

    /// Names of the immediate subkeys of `key`, in enumeration order.
    [[nodiscard]] virtual Result<std::vector<std::wstring>> subkeys(std::wstring_view key) const = 0;

    /// A `REG_SZ` (or `REG_EXPAND_SZ`) value, unexpanded.
    [[nodiscard]] virtual Result<std::optional<std::wstring>> read_string(std::wstring_view key,
                                                                          std::wstring_view value) const = 0;

    /// A `REG_DWORD` value.
    [[nodiscard]] virtual Result<std::optional<std::uint32_t>> read_dword(std::wstring_view key,
                                                                          std::wstring_view value) const = 0;

    /// Writes a `REG_SZ`. The only mutation M1 performs, for `orphans --relink`.
    [[nodiscard]] virtual Status write_string(std::wstring_view key, std::wstring_view value,
                                              std::wstring_view data) = 0;
};

/// Host filesystem queries. The only implementation that touches Win32 lives in
/// `platform/`; unit tests substitute an in-memory fake.
class IFileSystem {
public:
    IFileSystem() = default;
    IFileSystem(const IFileSystem&) = delete;
    IFileSystem& operator=(const IFileSystem&) = delete;
    IFileSystem(IFileSystem&&) = delete;
    IFileSystem& operator=(IFileSystem&&) = delete;
    virtual ~IFileSystem() = default;

    /// Whether the path exists (as a file or a directory).
    [[nodiscard]] virtual bool exists(const std::filesystem::path& path) const = 0;

    /// Logical file length, as reported by the directory entry.
    [[nodiscard]] virtual Result<std::uint64_t> file_size(const std::filesystem::path& path) const = 0;

    /// Bytes the file actually occupies on the volume. For a sparse or compressed
    /// file this is smaller than `file_size` -- it is the number that matters when
    /// reporting how much disk a VHDX is really using.
    [[nodiscard]] virtual Result<std::uint64_t> file_size_on_disk(
        const std::filesystem::path& path) const = 0;

    /// Whether the file carries `FILE_ATTRIBUTE_SPARSE_FILE`.
    [[nodiscard]] virtual Result<bool> is_sparse(const std::filesystem::path& path) const = 0;

    /// Filesystem type and free space of the volume holding `path`.
    [[nodiscard]] virtual Result<VolumeInfo> volume_info(const std::filesystem::path& path) const = 0;
};

}  // namespace wsldisk
