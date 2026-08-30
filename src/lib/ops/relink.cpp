#include "relink.h"

#include <format>
#include <utility>

#include "../model/orphans.h"
#include "../model/text.h"

namespace wsldisk::ops {
namespace {

constexpr std::wstring_view extended_prefix = LR"(\\?\)";

/// The value names this writes back. Named once so the write and the undo
/// cannot disagree about which one they are talking about.
constexpr std::wstring_view base_path_value = L"BasePath";
constexpr std::wstring_view vhd_file_name_value = L"VhdFileName";

}  // namespace

RelinkOperation::RelinkOperation(IRegistry& registry, const IFileSystem& filesystem, const IWslHost& host,
                                 model::Distro distro, std::filesystem::path target)
    : registry_(&registry),
      filesystem_(&filesystem),
      host_(&host),
      distro_(std::move(distro)),
      target_(std::move(target)) {}

std::wstring RelinkOperation::intended_base_path() const {
    std::wstring directory = target_.parent_path().wstring();
    // Whichever form the distribution already used. Normalising it would be an
    // unrequested change to a value Docker Desktop, not this tool, owns.
    if (distro_.base_path.starts_with(extended_prefix)) {
        return std::wstring{extended_prefix} + directory;
    }
    return directory;
}

Result<Plan> RelinkOperation::plan() {
    if (!distro_.is_wsl2()) {
        return fail(ErrorCode::Preflight,
                    std::format("{} is a WSL1 distribution and has no virtual disk", distro_.name),
                    std::format("convert it with `wsl --set-version {} 2`, then try again", distro_.name));
    }

    if (!filesystem_->exists(target_)) {
        return fail(ErrorCode::Preflight, std::format("{} does not exist", target_.string()),
                    "check the path, or run `wsldisk orphans` to see what was found");
    }

    // Pointing a distribution at a disk it already uses is not an error worth
    // failing on, but it is worth saying: the user probably meant a different
    // file, and doing nothing quietly would look like success.
    if (model::same_path(distro_.vhdx_path, target_)) {
        return fail(ErrorCode::Preflight,
                    std::format("{} already points at {}", distro_.name, target_.string()),
                    "nothing to do; `wsldisk info` shows where it points");
    }

    const auto running = host_->running();
    if (running.has_value() && std::ranges::find(*running, distro_.name) != running->end()) {
        return fail(ErrorCode::DistroBusy, std::format("{} is running", distro_.name),
                    std::format("run `wsl --terminate {}` first", distro_.name));
    }

    Plan plan;
    plan.steps.push_back(
        StepPlan{.description = std::format("point {} at {}", distro_.name, target_.string()),
                 .mutates = true,
                 .undo_description = std::format("restore {}'s registry entry", distro_.name)});
    plan.steps.push_back(StepPlan{
        .description = std::format("start {} to check the new path works", distro_.name), .mutates = false});
    plan.warnings.push_back(
        Warning{.message = "this rewrites the WSL registry entry for the distribution",
                .remedy = "it is put back automatically if the distribution fails to start"});
    return plan;
}

Result<Report> RelinkOperation::execute(ProgressSink& progress) {
    const std::wstring key = model::registry_key_for(distro_);
    const StepPlan write{.description = std::format("point {} at {}", distro_.name, target_.string()),
                         .mutates = true};
    progress.step_started(0, write);

    // Captured before anything changes, so the undo restores exactly what was
    // there rather than what it assumes was there.
    const std::wstring previous_base_path = distro_.base_path;
    const std::wstring previous_vhd_file_name = distro_.vhd_file_name;
    const bool had_vhd_file_name = !previous_vhd_file_name.empty();

    if (const Status written = registry_->write_string(key, base_path_value, intended_base_path());
        !written.has_value()) {
        return std::unexpected(written.error());
    }
    // Named rather than written inline: clang-tidy's bugprone-exception-escape
    // reports a lambda in argument position as one that must not throw.
    auto restore_base_path = [this, key, previous_base_path]() -> Status {
        return registry_->write_string(key, base_path_value, previous_base_path);
    };
    undo_.push(std::format("restore {}'s BasePath", distro_.name), restore_base_path);

    // Only written when the distribution already had one. Adding the value to a
    // legacy entry that never had it would change its layout, not repair it.
    if (had_vhd_file_name) {
        const std::wstring name = target_.filename().wstring();
        if (const Status written = registry_->write_string(key, vhd_file_name_value, name);
            !written.has_value()) {
            return std::unexpected(written.error());
        }
        auto restore_vhd_file_name = [this, key, previous_vhd_file_name]() -> Status {
            return registry_->write_string(key, vhd_file_name_value, previous_vhd_file_name);
        };
        undo_.push(std::format("restore {}'s VhdFileName", distro_.name), restore_vhd_file_name);
    }
    progress.step_finished(0, write);

    const StepPlan smoke{.description = std::format("start {} to check the new path works", distro_.name)};
    progress.step_started(1, smoke);

    // The smoke test. `/bin/true` does nothing in the guest, which is the
    // point: what is being tested is that the distribution boots from the disk
    // the registry now names.
    const std::vector<std::string> argv{"/bin/true"};
    const auto started = host_->run_as_root(distro_.name, argv, std::chrono::seconds{60});
    if (!started.has_value()) {
        return std::unexpected(started.error());
    }
    if (!started->succeeded()) {
        return fail(ErrorCode::Preflight,
                    std::format("{} did not start from {}", distro_.name, target_.string()),
                    "the registry entry has been put back; check the path names the right disk");
    }
    progress.step_finished(1, smoke);

    Report report;
    report.completed.push_back(write.description);
    report.completed.push_back(smoke.description);
    return report;
}

Status RelinkOperation::verify() {
    const std::wstring key = model::registry_key_for(distro_);
    const auto written = registry_->read_string(key, base_path_value);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    // An absent value compares unequal, which is the right answer:
    // `intended_base_path` is a directory and is never empty.
    if (written->value_or(std::wstring{}) != intended_base_path()) {
        return fail(ErrorCode::Partial, std::format("{}'s BasePath is not what was written", distro_.name),
                    "run `wsldisk info` to see where it points now");
    }
    return {};
}

void RelinkOperation::rollback(ProgressSink& progress) noexcept {
    undo_.unwind(progress);
}

}  // namespace wsldisk::ops
