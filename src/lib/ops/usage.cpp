#include "usage.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <set>
#include <utility>

#include "../model/disk_info.h"

namespace wsldisk::ops {
namespace {

constexpr std::string_view guest_du = "/usr/bin/du";
constexpr std::string_view guest_getent = "/usr/bin/getent";
constexpr std::string_view guest_df = "/bin/df";

/// Home directories that are not really home directories.
///
/// Every system account on a Debian-derived guest has one of these, and `du -sb`
/// on `/` is a measurement of the whole filesystem that takes as long as the
/// filesystem is big -- once per account.
[[nodiscard]] bool is_a_real_home(std::string_view home) {
    return home.starts_with("/home/") || home.starts_with("/root");
}

[[nodiscard]] std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    for (const auto line : std::views::split(text, '\n')) {
        std::string_view value{line.begin(), line.end()};
        if (value.ends_with('\r')) {
            value.remove_suffix(1);
        }
        if (!value.empty()) {
            lines.push_back(value);
        }
    }
    return lines;
}

}  // namespace

std::optional<std::uint64_t> parse_du_line(std::string_view line) {
    // `du -sb` prints `<bytes>\t<path>`. Anything else -- a permission warning,
    // a shell complaining the binary is missing -- is not a measurement.
    const std::size_t tab = line.find('\t');
    if (tab == std::string_view::npos || tab == 0) {
        return std::nullopt;
    }
    const std::string_view digits = line.substr(0, tab);
    std::uint64_t value = 0;
    const auto* const end = digits.data() + digits.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto parsed = std::from_chars(digits.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string> home_directories(std::string_view passwd) {
    std::set<std::string> homes;
    for (const std::string_view line : lines_of(passwd)) {
        // name:password:uid:gid:gecos:home:shell -- the home is field six.
        std::vector<std::string_view> fields;
        for (const auto field : std::views::split(line, ':')) {
            fields.emplace_back(field.begin(), field.end());
        }
        constexpr std::size_t home_field = 5;
        if (fields.size() <= home_field) {
            continue;
        }
        const std::string_view home = fields[home_field];
        if (home.empty() || !is_a_real_home(home)) {
            continue;
        }
        homes.emplace(home);
    }
    return {homes.begin(), homes.end()};
}

namespace {

/// Marks which entries contain others, and adds up the ones that do not sit
/// inside another.
///
/// Separate from the measuring because it is a different question: `du` says how
/// big each place is, and this says which of those numbers may be added together.
void account_for_nesting(UsageReport& report) {
    for (UsageEntry& outer : report.entries) {
        outer.contains_others = std::ranges::any_of(report.entries, [&outer](const UsageEntry& inner) {
            return model::path_contains(outer.path, inner.path);
        });
    }
    for (const UsageEntry& entry : report.entries) {
        const bool inside_another = std::ranges::any_of(report.entries, [&entry](const UsageEntry& outer) {
            return model::path_contains(outer.path, entry.path);
        });
        if (!inside_another) {
            report.counted += entry.bytes;
        }
    }
}

}  // namespace

UsageOperation::UsageOperation(const IWslHost& host, model::Distro distro, UsageOptions options)
    : host_(&host), distro_(std::move(distro)), options_(options) {}

Status UsageOperation::read_totals(UsageReport& report) {
    const std::vector<std::string> argv{std::string{guest_df}, "-B1", "/"};
    const auto df = host_->run_as_root(distro_.name, argv, options_.timeout);
    if (!df.has_value()) {
        return std::unexpected(df.error());
    }
    const auto usage = model::parse_df(df->standard_output);
    if (!usage.has_value()) {
        report.notes.emplace_back("could not read df output; the totals are unknown");
        return {};
    }
    report.guest_used = usage->used;
    report.guest_free = usage->available;
    return {};
}

std::vector<std::string> UsageOperation::homes_of(UsageReport& report) {
    const std::vector<std::string> argv{std::string{guest_getent}, "passwd"};
    const auto passwd = host_->run_as_root(distro_.name, argv, options_.timeout);
    if (passwd.has_value() && passwd->succeeded()) {
        std::vector<std::string> homes = home_directories(passwd->standard_output);
        if (!homes.empty()) {
            return homes;
        }
    }
    // Better than nothing, and true on the great majority of guests -- but the
    // report says so rather than implying it looked everywhere.
    report.notes.emplace_back(
        "could not list the guest's users, so only /root was checked for per-user caches");
    return {"/root"};
}

Result<std::optional<UsageEntry>> UsageOperation::measure_path(const model::CacheEntry& entry,
                                                               const std::string& path, bool per_user) {
    // `-s` for a single total, `-b` for bytes, `-x` so a bind mount or a Windows
    // drive under /mnt is not counted as part of this disk.
    const std::vector<std::string> argv{std::string{guest_du}, "-sbx", path};
    const auto measured = host_->run_as_root(distro_.name, argv, options_.timeout);
    if (!measured.has_value()) {
        return std::unexpected(measured.error());
    }
    // A path that is not there is the normal case -- no guest has every package
    // manager -- and is silently nothing rather than a note.
    const auto bytes = parse_du_line(measured->standard_output);
    if (!bytes.has_value() || *bytes == 0) {
        return std::nullopt;
    }

    UsageEntry found;
    found.path = path;
    // The label is the same for every home, so say whose it is.
    found.label = per_user ? std::format("{} ({})", entry.label, path) : entry.label;
    found.bytes = *bytes;
    found.safe = entry.safe;
    found.note = entry.note;
    return found;
}

Result<UsageReport> UsageOperation::measure(const std::function<void(std::string_view)>& progress) {
    if (!distro_.is_wsl2()) {
        return fail(ErrorCode::Preflight,
                    std::format("{} is a WSL1 distribution and has no virtual disk", distro_.name),
                    std::format("convert it with `wsl --set-version {} 2`, then try again", distro_.name));
    }

    UsageReport report;
    report.distribution = distro_.name;

    // `df` first, so the totals the entries are compared against come from the
    // same moment as the entries themselves.
    progress("reading the filesystem totals");
    if (const Status totals = read_totals(report); !totals.has_value()) {
        return std::unexpected(totals.error());
    }

    // Home directories, so `~/.cache` covers every account rather than assuming
    // there is one user named after the distribution.
    const std::vector<std::string> homes = homes_of(report);

    for (const model::CacheEntry& entry : model::cache_catalogue()) {
        const std::vector<std::string> paths = paths_for(entry, homes);
        for (const std::string& path : paths) {
            progress(path);
            auto found = measure_path(entry, path, paths.size() > 1);
            if (!found.has_value()) {
                return std::unexpected(found.error());
            }
            if (found->has_value()) {
                report.entries.push_back(std::move(**found));
            }
        }
    }

    // Biggest first: the answer to "where did it go" is the top of this list,
    // and a user reading a table of thirty rows stops after five.
    std::ranges::sort(report.entries, [](const UsageEntry& left, const UsageEntry& right) {
        return left.bytes > right.bytes;
    });
    account_for_nesting(report);

    if (options_.top != 0 && report.entries.size() > options_.top) {
        report.entries.resize(options_.top);
    }
    return report;
}

std::vector<std::string> UsageOperation::paths_for(const model::CacheEntry& entry,
                                                   const std::vector<std::string>& homes) {
    if (!entry.is_per_user()) {
        return {entry.path};
    }
    std::vector<std::string> paths;
    paths.reserve(homes.size());
    for (const std::string& home : homes) {
        paths.push_back(home + entry.path.substr(1));
    }
    return paths;
}

}  // namespace wsldisk::ops
