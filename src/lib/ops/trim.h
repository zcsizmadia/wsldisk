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

/// What one `fstrim` run produced.
struct TrimResult {
    /// What it said it trimmed, when it said anything. Absent when `-v` was
    /// rejected, or when the guest printed something unrecognised. Treat it as
    /// a report, not a measurement: see `trimmed_bytes_are_misleading`.
    std::optional<std::uint64_t> trimmed_bytes;
    /// Whether the run needed the plain `fstrim /` spelling.
    ///
    /// busybox rejects some of util-linux's options, and Alpine -- the
    /// integration fixture -- is busybox. Reported so a test can prove the
    /// fallback ran rather than inferring it from the absence of a number.
    bool used_fallback = false;
};

/// Runs `fstrim` in one distribution, with the `-v` fallback.
///
/// Shared by `trim` and by `compact`'s first step rather than written twice:
/// which spellings the guest takes is a fact about WSL, and two copies of it
/// would eventually disagree.
///
/// `-av` is never used. busybox rejects `-a` outright (spike #1), so this is
/// `/sbin/fstrim -v /`, retried as plain `/sbin/fstrim /` when the guest
/// refuses the option. A failure that is not about an option is not retried:
/// retrying would only hide the reason.
[[nodiscard]] Result<TrimResult> run_fstrim(const IWslHost& host, const std::string& distro,
                                            std::chrono::milliseconds timeout, ProgressSink& progress);

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
    [[nodiscard]] const std::optional<std::uint64_t>& trimmed_bytes() const noexcept {
        return result_.trimmed_bytes;
    }

    /// Whether the run needed the plain `fstrim /` spelling.
    [[nodiscard]] bool used_fallback() const noexcept { return result_.used_fallback; }

private:
    const IWslHost* host_;
    model::Distro distro_;
    std::chrono::milliseconds timeout_;
    TrimResult result_;
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
