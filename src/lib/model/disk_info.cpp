#include "disk_info.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <numeric>
#include <ranges>

namespace wsldisk::model {
namespace {

/// `--exec` does not search PATH, so the guest command is an absolute path.
constexpr std::string_view guest_df = "/bin/df";

/// Columns `df` always ends with, whatever its locale or how the row wrapped:
/// `1B-blocks used available use% mounted-on`.
constexpr std::size_t df_trailing_columns = 5;

[[nodiscard]] std::optional<std::uint64_t> to_number(std::string_view text) {
    std::uint64_t value = 0;
    const auto* const end = text.data() + text.size();
    // The size *is* provided -- `end` is exactly it -- which is what the check
    // asks for; it just cannot see that through the separate variable.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

/// The last line with anything on it. `df` prints a header first and may print
/// a trailing newline.
[[nodiscard]] std::vector<std::string_view> last_row(std::string_view output) {
    std::vector<std::string_view> tokens;
    for (const auto line : std::views::split(output, '\n')) {
        std::string_view text{line.begin(), line.end()};
        // The stream may be CRLF; the carriage return belongs to the line
        // ending, not to the last column. Dropping it here rather than
        // filtering a lone "\r" token keeps it out of the mount point too.
        if (text.ends_with('\r')) {
            text.remove_suffix(1);
        }

        std::vector<std::string_view> row;
        for (const auto piece : std::views::split(text, ' ')) {
            const std::string_view token{piece.begin(), piece.end()};
            if (!token.empty()) {
                row.push_back(token);
            }
        }
        // A row that wrapped leaves the device name alone on its own line, so
        // the *last* non-empty line is the one carrying the numbers.
        if (!row.empty()) {
            tokens = std::move(row);
        }
    }
    return tokens;
}

}  // namespace

std::optional<std::uint64_t> DiskInfo::reclaimable() const noexcept {
    if (!size_on_disk.has_value() || !guest_used.has_value()) {
        return std::nullopt;
    }
    // The host and the guest count different things, and on a compressed volume
    // the guest's number can be the larger of the two. That is not a negative
    // saving; it is no saving.
    if (*guest_used >= *size_on_disk) {
        return std::uint64_t{0};
    }
    return *size_on_disk - *guest_used;
}

std::optional<GuestUsage> parse_df(std::string_view output) {
    const std::vector<std::string_view> row = last_row(output);
    if (row.size() < df_trailing_columns) {
        return std::nullopt;
    }

    // Counted from the right, so a wrapped device name or a long filesystem
    // name cannot shift the columns that matter.
    const auto used = to_number(row[row.size() - 4]);
    const auto available = to_number(row[row.size() - 3]);
    if (!used.has_value() || !available.has_value()) {
        return std::nullopt;
    }
    return GuestUsage{.used = *used, .available = *available};
}

Result<DiskInfo> measure(const Distro& distro, const IFileSystem& filesystem, const IVirtualDisk& disks,
                         const IWslHost& host, const ProbeOptions& options) {
    DiskInfo info;

    if (const auto size = filesystem.file_size(distro.vhdx_path); size.has_value()) {
        info.file_size = *size;
    } else {
        info.notes.push_back(size.error().to_string());
    }

    if (const auto on_disk = filesystem.file_size_on_disk(distro.vhdx_path); on_disk.has_value()) {
        info.size_on_disk = *on_disk;
    } else {
        info.notes.push_back(on_disk.error().to_string());
    }

    if (const auto sparse = filesystem.is_sparse(distro.vhdx_path); sparse.has_value()) {
        info.is_sparse = *sparse;
    } else {
        info.notes.push_back(sparse.error().to_string());
    }

    if (const auto ranges = filesystem.allocated_ranges(distro.vhdx_path); ranges.has_value()) {
        info.allocated_bytes = std::accumulate(
            ranges->begin(), ranges->end(), std::uint64_t{0},
            [](std::uint64_t total, const AllocatedRange& range) { return total + range.length; });
    } else {
        info.notes.push_back(ranges.error().to_string());
    }

    // A running distribution holds its VHDX open, so this is the measurement
    // most likely to be missing on a machine in normal use.
    if (const auto handle = disks.open(distro.vhdx_path); handle.has_value()) {
        if (const auto disk = (*handle)->information(); disk.has_value()) {
            info.virtual_size = disk->virtual_size;
        } else {
            info.notes.push_back(disk.error().to_string());
        }
    } else {
        info.notes.push_back(handle.error().to_string());
    }

    const auto running = host.running();
    if (!running.has_value()) {
        info.notes.push_back(running.error().to_string());
        return info;
    }

    const bool is_running = std::ranges::find(*running, distro.name) != running->end();
    if (!is_running && !options.probe_guest) {
        // Deliberately not an error and deliberately not a probe: starting a
        // distribution to measure it would change the thing being measured.
        info.notes.push_back(
            std::format("{} is not running; pass --probe to start it and read guest usage", distro.name));
        return info;
    }

    const std::vector<std::string> argv{std::string{guest_df}, "-B1", "/"};
    const auto probe = host.run_as_root(distro.name, argv, options.guest_timeout);
    if (!probe.has_value()) {
        info.notes.push_back(probe.error().to_string());
        return info;
    }
    if (!probe->succeeded()) {
        info.notes.push_back(std::format("df exited {} in {}", probe->exit_code, distro.name));
        return info;
    }

    const auto usage = parse_df(probe->standard_output);
    if (!usage.has_value()) {
        info.notes.push_back(std::format("could not read df output from {}", distro.name));
        return info;
    }
    info.guest_used = usage->used;
    info.guest_free = usage->available;
    return info;
}

}  // namespace wsldisk::model
