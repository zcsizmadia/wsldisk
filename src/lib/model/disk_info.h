#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../errors.h"
#include "../interfaces.h"
#include "distro.h"

namespace wsldisk::model {

/// What one distribution's disk costs, from every angle that differs.
///
/// Every field is optional because every one of them can be unmeasurable on a
/// machine where the command still has something useful to say: a running
/// distribution holds its VHDX open, a guest probe needs the distribution up.
/// A measurement that fails leaves the field unset and adds a note; it never
/// fails the command, because `list` printing most of a row beats printing
/// nothing.
struct DiskInfo {
    /// The maximum the disk may grow to. `GetVirtualDiskInformation`, so absent
    /// while the utility VM holds the file.
    std::optional<std::uint64_t> virtual_size;
    /// Logical length of the VHDX file.
    std::optional<std::uint64_t> file_size;
    /// What the volume is actually charged for it -- smaller than `file_size`
    /// when the file is sparse, which WSL's disks usually are.
    std::optional<std::uint64_t> size_on_disk;
    /// Summed `FSCTL_QUERY_ALLOCATED_RANGES`. Spike #4 found the `Flags` value
    /// cannot identify sparseness, so this is where the number comes from.
    std::optional<std::uint64_t> allocated_bytes;
    std::optional<bool> is_sparse;

    /// What the guest filesystem reports. Only measured when the distribution
    /// is already running, or when the caller explicitly asked -- nothing here
    /// starts a distribution in order to measure it.
    std::optional<std::uint64_t> guest_used;
    std::optional<std::uint64_t> guest_free;

    /// Why a field is missing. One line per failed measurement, meant to be
    /// shown next to the row rather than swallowed.
    std::vector<std::string> notes;

    /// The headline number: what compaction could plausibly give back.
    ///
    /// `size_on_disk - guest_used`, and unknown unless both are. Clamped at
    /// zero: the two numbers come from different layers and the guest's can
    /// exceed the host's on a compressed volume, which is not a negative
    /// saving.
    [[nodiscard]] std::optional<std::uint64_t> reclaimable() const noexcept;
};

/// How much work `measure` is allowed to do.
struct ProbeOptions {
    /// Ask the guest even when the distribution is not running, which starts it.
    /// Off by default: measuring must not change what is running.
    bool probe_guest = false;
    /// Budget for the guest command. It is a `df`, so it either answers at once
    /// or something is wrong.
    std::chrono::milliseconds guest_timeout{30'000};
};

/// Measures one distribution's disk.
///
/// Cannot fail, and deliberately returns no `Result`: every individual
/// measurement that goes wrong becomes an unset field and a note, so there is no
/// outcome left for an error to describe. An earlier version returned
/// `Result<DiskInfo>` and gave every caller a branch that could not be reached.
[[nodiscard]] DiskInfo measure(const Distro& distro, const IFileSystem& filesystem, const IVirtualDisk& disks,
                               const IWslHost& host, const ProbeOptions& options);

/// Used and free bytes from `df -B1` output.
///
/// Parsed by column position counted **from the right**, never by header text.
/// The headers are localized, and a long device name makes `df` wrap the row
/// onto two lines -- but the last five columns are always
/// `1B-blocks used available use% mounted-on`, and the mount point being `/`
/// has no spaces in it. Returns nothing when the output is not that shape.
struct GuestUsage {
    std::uint64_t used = 0;
    std::uint64_t available = 0;
};

[[nodiscard]] std::optional<GuestUsage> parse_df(std::string_view output);

}  // namespace wsldisk::model
