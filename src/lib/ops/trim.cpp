#include "trim.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace wsldisk::ops {
namespace {

/// Where `fstrim` is. `wsl --exec` does not search PATH, so this has to be a
/// full path; `/sbin` is the one that works on both merged-usr distributions
/// and the busybox layout the test fixture uses.
constexpr std::string_view fstrim = "/sbin/fstrim";

/// The mountpoint. Always `/`: `-a` would cover every mounted filesystem, but
/// busybox rejects it outright (spike #1), and inside a distribution `/` is the
/// virtual disk that `compact` will later shrink.
constexpr std::string_view root = "/";

/// Whether the guest refused an option rather than failing to trim.
///
/// Two spellings, both measured rather than guessed: busybox prints
/// `fstrim: unrecognized option: a` (spike #1), and getopt prints
/// `invalid option`. A refusal is worth retrying without `-v`; anything else is
/// a real failure and retrying would only hide it.
[[nodiscard]] bool rejected_an_option(std::string_view text) {
    return text.find("unrecognized option") != std::string_view::npos ||
           text.find("invalid option") != std::string_view::npos;
}

/// The run of digits ending at `end`, as a number.
///
/// Returns nothing when there are no digits, or when they do not fit -- a
/// count that overflows 64 bits is not a number this is willing to report.
[[nodiscard]] std::optional<std::uint64_t> digits_before(std::string_view text, std::size_t end) {
    std::size_t begin = end;
    while (begin > 0 && (std::isdigit(static_cast<unsigned char>(text[begin - 1])) != 0)) {
        --begin;
    }
    if (begin == end) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (std::size_t index = begin; index < end; ++index) {
        const auto digit = static_cast<std::uint64_t>(text[index] - '0');
        constexpr std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
        if (value > (limit - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

}  // namespace

std::optional<std::uint64_t> parse_trimmed_bytes(std::string_view output) {
    // Anchored on the word rather than on a column: the two spellings put the
    // number in different places, but both put it immediately before `bytes`.
    const std::size_t at = output.rfind("bytes");
    if (at == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t end = at;
    while (end > 0 && output[end - 1] == ' ') {
        --end;
    }
    return digits_before(output, end);
}

TrimOperation::TrimOperation(const IWslHost& host, model::Distro distro, std::chrono::milliseconds timeout)
    : host_(&host), distro_(std::move(distro)), timeout_(timeout) {}

Result<Plan> TrimOperation::plan() {
    if (!distro_.is_wsl2()) {
        return fail(ErrorCode::Preflight,
                    std::format("{} is a WSL1 distribution and has no virtual disk", distro_.name),
                    std::format("convert it with `wsl --set-version {} 2`, then try again", distro_.name));
    }

    Plan plan;
    plan.steps.push_back(StepPlan{.description = std::format("run {} {} in {}", fstrim, root, distro_.name),
                                  // Nothing to put back: what changes is which blocks the guest
                                  // has told the disk it no longer needs.
                                  .mutates = true});
    plan.warnings.push_back(
        Warning{.message = "this starts the distribution if it is stopped, and leaves it running",
                .remedy = "run `wsl --terminate " + distro_.name + "` afterwards if that matters"});
    return plan;
}

Result<Report> TrimOperation::execute(ProgressSink& progress) {
    const StepPlan step{.description = std::format("run {} {} in {}", fstrim, root, distro_.name),
                        .mutates = true};
    progress.step_started(0, step);

    // `-v` first, because the byte count is the only thing fstrim says about
    // what it did. `-av` is never used: busybox rejects `-a` (spike #1).
    const std::vector<std::string> verbose{std::string{fstrim}, "-v", std::string{root}};
    auto result = host_->run_as_root(distro_.name, verbose, timeout_);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }

    if (!result->succeeded() && rejected_an_option(result->standard_error)) {
        progress.message("fstrim does not take -v here; retrying without it");
        used_fallback_ = true;
        const std::vector<std::string> plain{std::string{fstrim}, std::string{root}};
        result = host_->run_as_root(distro_.name, plain, timeout_);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
    }

    if (!result->succeeded()) {
        // The guest's own words, because "fstrim failed" on its own tells the
        // user nothing they can act on.
        std::string detail =
            result->standard_error.empty() ? result->standard_output : result->standard_error;
        std::erase(detail, '\n');
        return fail(ErrorCode::Generic, std::format("fstrim failed in {}: {}", distro_.name, detail),
                    "check the distribution starts (`wsl -d " + distro_.name +
                        " -- true`) and that /sbin/fstrim exists in it");
    }

    trimmed_bytes_ = parse_trimmed_bytes(result->standard_output);
    progress.step_finished(0, step);

    Report report;
    report.completed.push_back(step.description);
    // Deliberately not reported as `bytes_freed`: the figure is the free extent
    // fstrim was offered, not space reclaimed, and an estimate that says
    // otherwise would be a lie in a struct field.
    return report;
}

Status TrimOperation::verify() {
    // Nothing to check. `fstrim` exiting zero is the whole claim, and re-reading
    // the disk would only measure what compaction has not done yet.
    return {};
}

void TrimOperation::rollback(ProgressSink& /*progress*/) noexcept {
    // Nothing to undo: no previous state was replaced.
}

}  // namespace wsldisk::ops
