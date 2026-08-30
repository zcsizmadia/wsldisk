#pragma once

#include <windows.h>
#include <virtdisk.h>

#include "../interfaces.h"

namespace wsldisk::platform {

/// `IVirtualDisk` on top of the Virtual Disk Service.
///
/// Opens with `OPEN_VIRTUAL_DISK_VERSION_2` and `VIRTUAL_DISK_ACCESS_NONE`,
/// measured to compact without administrator rights. V2 accepts that mask and
/// no other: `VIRTUAL_DISK_ACCESS_METAOPS` with V2 parameters fails to open
/// with `ERROR_INVALID_PARAMETER`. V1 with `METAOPS` compacts too, but V1 also
/// accepts masks that open and then fail at the compaction, so the V2 shape is
/// the one that fails early (PLAN.md D10, docs/RESEARCH.md). A contract test
/// pins the behaviour this relies on.
class Win32VirtualDisk final : public IVirtualDisk {
public:
    [[nodiscard]] Result<std::unique_ptr<IVirtualDiskHandle>> open(
        const std::filesystem::path& path) const override;

    [[nodiscard]] Status create(const std::filesystem::path& path, std::uint64_t maximum_size) const override;
};

/// The VHDX vendor GUID, exposed so contract tests can build a storage type
/// without duplicating the literal.
[[nodiscard]] VIRTUAL_STORAGE_TYPE vhdx_storage_type() noexcept;

}  // namespace wsldisk::platform
