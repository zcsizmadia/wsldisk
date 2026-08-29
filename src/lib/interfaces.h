#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

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
