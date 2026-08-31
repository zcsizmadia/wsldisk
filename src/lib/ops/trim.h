#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../interfaces.h"
#include "../model/distro.h"
#include "operation.h"

namespace wsldisk::ops {

/// Discards the guest's unused blocks with `fstrim`.
///
/// The light-touch half of `compact`: it runs inside the distribution and leaves
/// it running, so it is the one reclaim step that is safe to put on a schedule.
/// On a sparse-mode disk it is the whole job; on an ordinary one it is what
/// makes the later compaction able to reclaim anything at all.
///
/// It does not shut the distribution down, does not touch the `.vhdx`, and has
/// nothing to undo. What it changes is which blocks the guest filesystem has
/// told the disk it no longer needs -- there is no previous state to put back.
class TrimOperation final : public IOperation {
public:
    TrimOperation(const IWslHost& host, model::Distro distro,
                  std::chrono::milliseconds timeout = std::chrono::minutes{10});

    [[nodiscard]] Result<Plan> plan() override;
    [[nodiscard]] Result<Report> execute(ProgressSink& progress) override;
    [[nodiscard]] Status verify() override;
    void rollback(ProgressSink& progress) noexcept override;

    /// What `fstrim` said it trimmed, when it said anything.
    ///
    /// Absent when `-v` was rejected, or when the guest printed something this
    /// does not recognise. Treat the number as a report, not a measurement:
    /// see `trimmed_bytes_are_misleading`.
    [[nodiscard]] const std::optional<std::uint64_t>& trimmed_bytes() const noexcept {
        return trimmed_bytes_;
    }

    /// Whether the run needed the plain `fstrim /` spelling.
    ///
    /// busybox rejects some of util-linux's options, and Alpine -- which is the
    /// integration fixture -- is busybox. Exposed so a test can prove the
    /// fallback ran rather than inferring it from the absence of a number.
    [[nodiscard]] bool used_fallback() const noexcept { return used_fallback_; }

private:
    const IWslHost* host_;
    model::Distro distro_;
    std::chrono::milliseconds timeout_;
    std::optional<std::uint64_t> trimmed_bytes_;
    bool used_fallback_ = false;
};

/// The byte count out of `fstrim -v` output, if there is one.
///
/// Both spellings in the wild are handled: util-linux prints
/// `/: 1 TiB (1078939029504 bytes) trimmed on /dev/sdc` and busybox prints
/// `/: 1078939029504 bytes trimmed`. The figure taken is always the one in
/// bytes, never the human-readable one, because rounding a number this
/// misleading twice helps nobody.
[[nodiscard]] std::optional<std::uint64_t> parse_trimmed_bytes(std::string_view output);

/// Why the figure `fstrim` reports must not be shown on its own.
///
/// Measured in spike #1: freeing 1 GiB and running `fstrim /` reported
/// 1,078,939,029,504 bytes -- the whole free extent of the 1 TB default
/// `vhdSize`, three orders of magnitude out. Every place that shows the number
/// says this, so nobody reads it as space reclaimed.
inline constexpr std::string_view trimmed_bytes_are_misleading =
    "that figure is the free extent of the disk, not space reclaimed: "
    "compaction is what shrinks the file";

}  // namespace wsldisk::ops
