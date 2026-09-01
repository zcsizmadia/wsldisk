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

/// One directory inside the guest, from the `--by-directory` breakdown.
struct UsageDirectory {
    /// The guest path. The user's own directory names, which is why they go to
    /// stdout and never into a fixture.
    std::string path;
    std::uint64_t bytes = 0;
    /// How far below `/` it sits: `/var` is 1, `/var/lib` is 2.
    std::size_t depth = 0;
    /// How much of this directory the catalogue already accounted for.
    ///
    /// A label on its own is not enough and was actively misleading: `/home` at
    /// 3.5 GiB containing a 36-byte `~/.cache` was reported as "already shown as
    /// user cache", which claims the whole directory was covered when almost
    /// none of it was. The number says what the label cannot.
    ///
    /// Nested catalogue entries are counted once, the same way `counted` is.
    std::uint64_t attributed_bytes = 0;

    /// The largest catalogue entry inside, for context. Empty when nothing in
    /// the catalogue explains any of it.
    std::string attributed_to;
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
    /// The largest directories, biggest first. Empty unless `--by-directory`
    /// asked for them.
    std::vector<UsageDirectory> directories;
    /// Anything that could not be measured, in the user's words rather than
    /// `du`'s.
    std::vector<std::string> notes;
};

/// How `usage` should go about it.
struct UsageOptions {
    /// Show at most this many entries. Zero means all of them.
    std::size_t top = 0;

    /// Also break the whole guest down by directory.
    ///
    /// The catalogue answers "how much of this is cache". This answers "and the
    /// rest?", which is the question that follows and the one the catalogue
    /// cannot answer for a directory nobody has written an entry for.
    bool by_directory = false;

    /// How far down that breakdown goes. `/var/lib` is depth 2.
    ///
    /// Two by default: deep enough to separate `/var/lib/docker` from `/var/log`
    /// and shallow enough that the answer is a page rather than a filesystem.
    std::size_t depth = 2;
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

    /// One `du` over the whole guest, filling in `report.directories`.
    ///
    /// One call rather than one per directory: `du` walks the tree once and
    /// reports every level on the way, and asking it repeatedly would re-walk
    /// the same filesystem for every answer.
    [[nodiscard]] Status measure_directories(UsageReport& report,
                                             const std::function<void(std::string_view)>& progress);

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

/// Every `<bytes>\t<path>` line in a `du` listing, in the order they appeared.
///
/// Lines that are not measurements -- permission warnings, a shell complaining
/// -- are dropped rather than guessed at. Exposed for the tests.
[[nodiscard]] std::vector<UsageDirectory> parse_du_listing(std::string_view text);

/// How far below `/` a path sits. `/` is 0, `/var` is 1, `/var/lib` is 2.
[[nodiscard]] std::size_t path_depth(std::string_view path);

/// The home directories to expand `~` into, from `getent passwd` output.
///
/// Real users only: the system accounts all point at `/`, `/nonexistent` or
/// `/usr/sbin/nologin`, and measuring `/` once per system account would take
/// minutes to report the same number a dozen times.
[[nodiscard]] std::vector<std::string> home_directories(std::string_view passwd);

}  // namespace wsldisk::ops
