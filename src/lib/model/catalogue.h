#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "../errors.h"

namespace wsldisk::model {

/// One place inside a guest that is worth measuring.
///
/// The catalogue is `data/caches.toml`, embedded into the binary at build time
/// so the tool stays a single file. Adding a package manager is a line of TOML
/// rather than a code change, which is the point of keeping it as data.
struct CacheEntry {
    /// Absolute guest path. A leading `~` stands for a home directory and is
    /// expanded per user, so one entry covers every account on the machine.
    std::string path;

    /// What to call it in the table.
    std::string label;

    /// Whether emptying it loses only things that can be fetched or regenerated.
    ///
    /// `false` does not mean dangerous. It means the tool cannot tell whether
    /// the contents matter -- `/var/lib/docker` holds images someone built --
    /// so it declines to decide, and `clean` will not touch it without being
    /// told twice.
    bool safe = false;

    /// Shown when the entry is big enough to be worth acting on. Usually the
    /// command that empties it properly.
    std::string note;

    /// Whether the path is under a home directory rather than absolute.
    [[nodiscard]] bool is_per_user() const noexcept { return path.starts_with("~/"); }
};

/// The catalogue compiled into this binary.
///
/// Parsed once and returned by reference. A malformed catalogue is a build-time
/// mistake rather than a runtime one -- the file ships inside the executable --
/// so this returns the entries it could read and the tests are what stop a bad
/// edit reaching a release.
[[nodiscard]] const std::vector<CacheEntry>& cache_catalogue();

/// Parses catalogue TOML. Exposed for the tests, which is the only way to feed
/// it something malformed.
///
/// Every `[[cache]]` needs `path`, `label` and `safe`; `note` is optional. An
/// entry missing any of them is a `Usage` error naming which, because the file
/// is hand-edited and "it did not work" is not a useful thing to say about a
/// typo.
[[nodiscard]] Result<std::vector<CacheEntry>> parse_catalogue(std::string_view text);

/// Whether `inner` names a path inside `outer`.
///
/// `~/.cache` contains `~/.cache/pip`, and `/var/log` contains
/// `/var/log/journal`. Both pairs are in the catalogue on purpose -- the user
/// wants to know about the specific one and about the whole -- so the report has
/// to know which totals are already counted inside another and say so rather
/// than presenting the same gigabytes twice.
[[nodiscard]] bool path_contains(std::string_view outer, std::string_view inner);

}  // namespace wsldisk::model
