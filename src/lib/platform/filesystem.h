#pragma once

#include "../interfaces.h"

namespace wsldisk::platform {

/// `IFileSystem` on top of the real Win32 API.
///
/// Every call goes through the `Win32Api` table (`win32_api.h`) so tests can
/// force each failure branch. The class holds no state and is safe to copy.
class Win32FileSystem final : public IFileSystem {
public:
    [[nodiscard]] bool exists(const std::filesystem::path& path) const override;
    [[nodiscard]] Result<std::uint64_t> file_size(const std::filesystem::path& path) const override;
    [[nodiscard]] Result<std::uint64_t> file_size_on_disk(const std::filesystem::path& path) const override;
    [[nodiscard]] Result<bool> is_sparse(const std::filesystem::path& path) const override;
    [[nodiscard]] Result<VolumeInfo> volume_info(const std::filesystem::path& path) const override;
    [[nodiscard]] Result<std::vector<DirectoryEntry>> list_directory(
        const std::filesystem::path& directory, std::wstring_view pattern) const override;
    [[nodiscard]] Result<std::vector<AllocatedRange>> allocated_ranges(
        const std::filesystem::path& path) const override;
    [[nodiscard]] Result<bool> is_locked(const std::filesystem::path& path) const override;
    [[nodiscard]] Status remove(const std::filesystem::path& path) override;
    [[nodiscard]] Result<std::filesystem::path> expand_environment(
        const std::filesystem::path& path) const override;
};

}  // namespace wsldisk::platform
