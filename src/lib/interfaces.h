#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/// What `GetVirtualDiskInformation` reports about a VHDX. Sizes are bytes.
struct VirtualDiskInfo {
    /// The maximum the disk can grow to -- what `shrink` and `grow` change.
    std::uint64_t virtual_size = 0;
    /// What the disk currently occupies, as the VHDX format sees it. This is not
    /// the same as the file's size on the host volume, which is what
    /// `IFileSystem::file_size_on_disk` reports and what users actually notice.
    std::uint64_t physical_size = 0;
    std::uint32_t block_size = 0;
    std::uint32_t sector_size = 0;
    /// Set for a differencing disk. Empty otherwise, which is every WSL disk
    /// unless someone has built a chain by hand.
    std::wstring parent_path;
};

/// Progress of a long-running virtual-disk operation, as a fraction.
struct DiskProgress {
    std::uint64_t current = 0;
    std::uint64_t total = 0;
};

/// Called periodically while a compaction runs. Returning false asks the
/// operation to stop at the next opportunity.
using ProgressCallback = std::function<bool(const DiskProgress&)>;

/// An open handle to a virtual disk. Closing is the destructor's job, so an
/// operation cannot leak one down an error path.
class IVirtualDiskHandle {
public:
    IVirtualDiskHandle() = default;
    IVirtualDiskHandle(const IVirtualDiskHandle&) = delete;
    IVirtualDiskHandle& operator=(const IVirtualDiskHandle&) = delete;
    IVirtualDiskHandle(IVirtualDiskHandle&&) = delete;
    IVirtualDiskHandle& operator=(IVirtualDiskHandle&&) = delete;
    virtual ~IVirtualDiskHandle() = default;

    [[nodiscard]] virtual Result<VirtualDiskInfo> information() const = 0;

    /// Compacts in place, reporting progress until it finishes. The disk must
    /// not be attached, and nothing here needs administrator rights.
    [[nodiscard]] virtual Status compact(const ProgressCallback& progress) = 0;
};

/// Opens virtual disks.
///
/// The parameter shape is not a caller's choice. The implementation hard-codes
/// `OPEN_VIRTUAL_DISK_VERSION_2` with `VIRTUAL_DISK_ACCESS_NONE`, which is the
/// only mask V2 accepts and which compacts unelevated (PLAN.md D10).
class IVirtualDisk {
public:
    IVirtualDisk() = default;
    IVirtualDisk(const IVirtualDisk&) = delete;
    IVirtualDisk& operator=(const IVirtualDisk&) = delete;
    IVirtualDisk(IVirtualDisk&&) = delete;
    IVirtualDisk& operator=(IVirtualDisk&&) = delete;
    virtual ~IVirtualDisk() = default;

    /// Opens an existing VHDX for metadata and compaction.
    [[nodiscard]] virtual Result<std::unique_ptr<IVirtualDiskHandle>> open(
        const std::filesystem::path& path) const = 0;

    /// Creates a fixed-maximum dynamic VHDX. Only the tests need this -- it is
    /// how contract tests get a real disk without shelling out to `diskpart`.
    [[nodiscard]] virtual Status create(const std::filesystem::path& path,
                                        std::uint64_t maximum_size) const = 0;
};

/// What running one `wsl.exe` command produced.
struct WslCommandResult {
    /// The process exit code. This is the only success signal that can be
    /// trusted; the streams carry noise even on a clean run.
    int exit_code = 0;
    /// Standard output, decoded to UTF-8.
    std::string standard_output;
    /// Standard error, decoded to UTF-8. On a successful `--exec` this still
    /// holds one `Failed to translate '<path>'` line per Windows PATH entry.
    std::string standard_error;

    [[nodiscard]] bool succeeded() const noexcept { return exit_code == 0; }
};

/// Runs `wsl.exe` and reports what it printed.
///
/// This is a process wrapper and nothing else. `wslapi.dll` is unusable from an
/// unpackaged process (spike #3), and everything the registry can answer --
/// which distributions exist, where their disks are, which is default -- goes
/// through `IRegistry` instead. Only facts that require a running WSL come from
/// here.
///
/// Nothing in here parses prose. `--quiet` output is names only, and every other
/// question is answered by an exit code, so a localized Windows does not change
/// what the tool understands.
class IWslHost {
public:
    IWslHost() = default;
    IWslHost(const IWslHost&) = delete;
    IWslHost& operator=(const IWslHost&) = delete;
    IWslHost(IWslHost&&) = delete;
    IWslHost& operator=(IWslHost&&) = delete;
    virtual ~IWslHost() = default;

    /// Names of the distributions that are currently running.
    [[nodiscard]] virtual Result<std::vector<std::string>> running() const = 0;

    /// Stops one distribution.
    [[nodiscard]] virtual Status terminate(std::string_view name) const = 0;

    /// Stops every distribution and the utility VM, which is what releases the
    /// lock on a VHDX so it can be compacted.
    [[nodiscard]] virtual Status shutdown() const = 0;

    /// Runs a command in the guest as root and captures both streams.
    ///
    /// `--exec` does not search PATH, so `argv[0]` must be an absolute guest
    /// path; a relative one is refused here rather than surfacing as WSL's own
    /// `execvpe` error. The guest's output is UTF-8, unlike `wsl.exe`'s own.
    [[nodiscard]] virtual Result<WslCommandResult> run_as_root(std::string_view name,
                                                               std::span<const std::string> argv,
                                                               std::chrono::milliseconds timeout) const = 0;

    /// Attaches a VHDX to the utility VM without mounting a filesystem. Needed
    /// by `shrink` in M2; the process wrapper is the same, so it lives here now.
    [[nodiscard]] virtual Status mount_bare(const std::filesystem::path& vhdx) const = 0;

    /// Detaches a VHDX attached by `mount_bare`.
    [[nodiscard]] virtual Status unmount(const std::filesystem::path& vhdx) const = 0;
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
