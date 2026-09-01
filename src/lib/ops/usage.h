#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../interfaces.h"
#include "../model/catalogue.h"
#include "../model/distro.h"

namespace wsldisk::ops {

/// One measured place inside the guest.
struct UsageEntry {
    /// The path as it was measured, with `~` already expanded.
    std::string path;
    std::string label;
    std::uint64_t bytes = 0;
    /// Whether emptying it loses only what can be fetched again.
    bool safe = false;
    std::string note;
    /// Whether another entry in the report lives inside this one.
    ///
    /// `~/.cache` contains `~/.cache/pip`; both are worth reporting and only one
    /// of them can be added to a total. The flag is what lets the table say so
    /// instead of counting the same bytes twice.
    bool contains_others = false;
};

/// What `usage` found.
struct UsageReport {
    /// The distribution it looked in.
    std::string distribution;
    /// `df` on `/` inside the guest.
    std::uint64_t guest_used = 0;
    std::uint64_t guest_free = 0;
    /// Every catalogue entry that exists and is not empty, biggest first.
    std::vector<UsageEntry> entries;
    /// The entries that are not inside another one, added up. What the user can
    /// meaningfully compare against `guest_used`.
    std::uint64_t counted = 0;
    /// Anything that could not be measured, in the user's words rather than
    /// `du`'s.
    std::vector<std::string> notes;
};

/// How `usage` should go about it.
struct UsageOptions {
    /// Show at most this many entries. Zero means all of them.
    std::size_t top = 0;
    /// How long to give the whole measurement.
    ///
    /// `du` over a large filesystem is slow, and the alternative to a timeout is
    /// a command that looks like it has hung.
    std::chrono::milliseconds timeout{std::chrono::minutes{5}};
};

/// Measures where the space inside a distribution went.
///
/// Read-only, entirely: it runs `du` and `df` and nothing else. `clean` is the
/// command that acts on this, and it reads the same catalogue so the two cannot
/// disagree about what counts as a cache.
///
/// Not an `IOperation`. There is no plan, nothing to undo and no point of no
/// return -- wrapping a measurement in the mutation lifecycle would be
/// ceremony that implies a danger this does not have.
class UsageOperation {
public:
    UsageOperation(const IWslHost& host, model::Distro distro, UsageOptions options = {});

    /// Runs the measurement. `progress` is told which path is being measured,
    /// because `du` on a large filesystem is slow enough that silence reads as
    /// a hang.
    [[nodiscard]] Result<UsageReport> measure(const std::function<void(std::string_view)>& progress);

private:
    /// `df` on `/`, into `report`. A `df` that cannot be parsed is a note rather
    /// than a failure: the entries are still worth reporting without a total to
    /// compare them against.
    [[nodiscard]] Status read_totals(UsageReport& report);

    /// The home directories to expand `~` into, noting it in `report` when the
    /// guest's users could not be listed.
    [[nodiscard]] std::vector<std::string> homes_of(UsageReport& report);

    /// The paths one catalogue entry stands for: one, or one per home.
    [[nodiscard]] static std::vector<std::string> paths_for(const model::CacheEntry& entry,
                                                            const std::vector<std::string>& homes);

    /// `du` on one path. Nothing when the path is not there or is empty, which
    /// is the normal case rather than a failure.
    [[nodiscard]] Result<std::optional<UsageEntry>> measure_path(const model::CacheEntry& entry,
                                                                 const std::string& path, bool per_user);

    const IWslHost* host_;
    model::Distro distro_;
    UsageOptions options_;
};

/// Turns one `du -sb` line into a byte count.
///
/// `du` prints `<bytes>\t<path>`. Exposed for the tests: the parsing is where
/// this can go quietly wrong, and a guest that answers something unexpected
/// should produce a note rather than a number that is silently zero.
[[nodiscard]] std::optional<std::uint64_t> parse_du_line(std::string_view line);

/// The home directories to expand `~` into, from `getent passwd` output.
///
/// Real users only: the system accounts all point at `/`, `/nonexistent` or
/// `/usr/sbin/nologin`, and measuring `/` once per system account would take
/// minutes to report the same number a dozen times.
[[nodiscard]] std::vector<std::string> home_directories(std::string_view passwd);

}  // namespace wsldisk::ops
