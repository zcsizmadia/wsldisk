#include "move.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>
#include <vector>

#include "../model/orphans.h"
#include "../model/size.h"
#include "../model/text.h"

namespace wsldisk::ops {
namespace {

constexpr std::wstring_view extended_prefix = LR"(\\?\)";

/// Named once so the write and the undo cannot disagree about which value they
/// are talking about.
constexpr std::wstring_view base_path_value = L"BasePath";
constexpr std::wstring_view vhd_file_name_value = L"VhdFileName";

/// Headroom on top of the disk's own size before a cross-volume copy.
///
/// A volume with exactly enough free space is a volume with none afterwards, and
/// the copy is not the only thing that needs room -- the filesystem's own
/// metadata grows with it. Refusing at the edge is kinder than failing halfway
/// through a copy the user waited on.
constexpr std::uint64_t free_space_margin = 64ULL * 1024 * 1024;

}  // namespace

MoveOperation::MoveOperation(IRegistry& registry, IFileSystem& filesystem, const IWslHost& host,
                             model::Distro distro, std::filesystem::path destination, MoveOptions options)
    : registry_(&registry),
      filesystem_(&filesystem),
      host_(&host),
      distro_(std::move(distro)),
      destination_(std::move(destination)),
      options_(options) {
    // The file keeps its name: `VhdFileName` records it, and WSL's own
    // conventions depend on it being `ext4.vhdx`.
    target_ = destination_ / std::filesystem::path{distro_.vhdx_path}.filename();
}

std::wstring MoveOperation::intended_base_path() const {
    std::wstring directory = destination_.wstring();
    if (distro_.base_path.starts_with(extended_prefix)) {
        return std::wstring{extended_prefix} + directory;
    }
    return directory;
}

Result<Plan> MoveOperation::plan() {
    if (!distro_.is_wsl2()) {
        return fail(ErrorCode::Preflight,
                    std::format("{} is a WSL1 distribution and has no virtual disk", distro_.name),
                    std::format("convert it with `wsl --set-version {} 2`, then try again", distro_.name));
    }

    if (!filesystem_->exists(distro_.vhdx_path)) {
        return fail(ErrorCode::Preflight,
                    std::format("{}'s disk is not at {}", distro_.name, distro_.vhdx_path.string()),
                    std::format("`wsldisk relink {} <path>` repoints it if the file has already moved",
                                distro_.name));
    }

    if (model::same_path(distro_.vhdx_path, target_)) {
        return fail(ErrorCode::Preflight, std::format("{} is already at {}", distro_.name, target_.string()),
                    "nothing to do; `wsldisk info` shows where it is");
    }

    // Moving onto a file that is already there would destroy it. The user's own
    // previous attempt is the likeliest thing to be sitting at that path.
    if (filesystem_->exists(target_)) {
        return fail(ErrorCode::Preflight, std::format("{} already exists", target_.string()),
                    "move it aside, or choose a different directory");
    }

    // A running distribution has to be refused, and a `running()` that fails is
    // a refusal too: if it *is* running, the smoke test later would execute in
    // the guest that is already booted -- from the old disk -- and pass without
    // testing anything.
    const auto running = host_->running();
    if (!running.has_value()) {
        return fail(
            ErrorCode::Preflight,
            std::format("cannot tell whether {} is running: {}", distro_.name, running.error().to_string()),
            std::format("run `wsl --terminate {}` and try again", distro_.name));
    }
    if (std::ranges::find(*running, distro_.name) != running->end()) {
        return fail(ErrorCode::DistroBusy, std::format("{} is running", distro_.name),
                    std::format("run `wsl --terminate {}` first", distro_.name));
    }

    const auto same = filesystem_->same_volume(distro_.vhdx_path, destination_);
    if (!same.has_value()) {
        return std::unexpected(same.error());
    }
    same_volume_ = *same;

    // What the file actually occupies, not its virtual size: a 1 TiB VHDX
    // holding 12 GiB needs 12 GiB of room, and refusing on the virtual size
    // would refuse every move anyone ever wanted to make.
    const auto occupied = filesystem_->file_size_on_disk(distro_.vhdx_path);
    if (!occupied.has_value()) {
        return std::unexpected(occupied.error());
    }
    size_ = *occupied;

    // Only a copy needs room. A rename moves no bytes -- but `--keep-source`
    // turns a same-volume move into a copy, and that copy needs the space like
    // any other.
    if (!was_renamed()) {
        const auto volume = filesystem_->volume_info(destination_);
        if (!volume.has_value()) {
            return std::unexpected(volume.error());
        }
        if (!volume->supports_vhdx()) {
            return fail(
                ErrorCode::Preflight,
                std::format("{} is {}, which cannot hold a virtual disk", destination_.string(),
                            volume->filesystem_name),
                "FAT and exFAT support neither sparse files nor files over 4 GB; use an NTFS or ReFS volume");
        }
        if (volume->free_bytes < *occupied + free_space_margin) {
            return fail(ErrorCode::Preflight,
                        std::format("{} has {} free and the disk occupies {}", destination_.string(),
                                    format_size(volume->free_bytes), format_size(*occupied)),
                        "free some space on the target volume, or `wsldisk compact` the disk first");
        }
    }

    Plan plan;
    plan.estimate.bytes_freed = std::nullopt;
    plan.steps.push_back(StepPlan{
        .description = was_renamed()
                           ? std::format("move {} to {}", distro_.vhdx_path.string(), target_.string())
                           : std::format("copy {} to {}", distro_.vhdx_path.string(), target_.string()),
        .mutates = true,
        .undo_description = std::format("remove {}", target_.string())});
    plan.steps.push_back(
        StepPlan{.description = std::format("point {} at {}", distro_.name, target_.string()),
                 .mutates = true,
                 .undo_description = std::format("restore {}'s registry entry", distro_.name)});
    plan.steps.push_back(StepPlan{
        .description = std::format("start {} to check the new path works", distro_.name), .mutates = false});

    // A rename left nothing behind, and `--keep-source` asked for it to stay.
    // Either way there is no fourth step, which is why the plan says so rather
    // than the execution quietly skipping one.
    if (!was_renamed() && !options_.keep_source) {
        plan.steps.push_back(
            StepPlan{.description = std::format("delete {}", distro_.vhdx_path.string()), .mutates = true});
    }

    plan.warnings.push_back(
        Warning{.message = std::format("{}'s registry entry will point at the new location", distro_.name),
                .remedy = "it is put back automatically if the distribution fails to start"});
    if (!was_renamed() && !options_.keep_source) {
        plan.warnings.push_back(
            Warning{.message = "the original disk is deleted once the distribution has started from the copy",
                    .remedy = "pass --keep-source to leave it in place"});
    }
    return plan;
}

Status MoveOperation::transfer(ProgressSink& progress) {
    // The directory may not exist yet; making it is part of putting the file
    // there. Not undone: an empty directory the user asked for is not damage,
    // and removing one that turned out to have been theirs already would be.
    if (const Status made = filesystem_->create_directories(destination_); !made.has_value()) {
        return made;
    }

    if (was_renamed()) {
        if (const Status moved = filesystem_->rename(distro_.vhdx_path, target_); !moved.has_value()) {
            return moved;
        }
        // Undone by moving it back, not by deleting it: the file at the target
        // *is* the original.
        const std::filesystem::path source = distro_.vhdx_path;
        const std::filesystem::path target = target_;
        IFileSystem* filesystem = filesystem_;
        auto move_back = [filesystem, source, target]() -> Status {
            return filesystem->rename(target, source);
        };
        undo_.push(std::format("move {} back to {}", target_.string(), source.string()), move_back);
        return {};
    }

    auto report = [&progress](std::uint64_t copied, std::uint64_t bytes) {
        progress.step_progress(DiskProgress{.current = copied, .total = bytes});
        return true;
    };
    if (const Status copied = filesystem_->copy_file_sparse(distro_.vhdx_path, target_, report);
        !copied.has_value()) {
        // The partial file is this operation's to clean up: it knows the copy
        // failed and that half a disk is worth nothing.
        std::ignore = filesystem_->remove(target_);
        return copied;
    }

    const std::filesystem::path target = target_;
    IFileSystem* filesystem = filesystem_;
    auto remove_copy = [filesystem, target]() -> Status { return filesystem->remove(target); };
    undo_.push(std::format("remove {}", target_.string()), remove_copy);
    return {};
}

Status MoveOperation::repoint() {
    const std::wstring key = model::registry_key_for(distro_);
    // Captured before anything changes, so the undo restores what was there
    // rather than what it assumes was there.
    const std::wstring previous_base_path = distro_.base_path;
    const std::wstring previous_vhd_file_name = distro_.vhd_file_name;

    if (const Status written = registry_->write_string(key, base_path_value, intended_base_path());
        !written.has_value()) {
        return written;
    }
    // Named rather than written inline: clang-tidy reports a lambda in argument
    // position as one that must not throw.
    IRegistry* registry = registry_;
    auto restore_base_path = [registry, key, previous_base_path]() -> Status {
        return registry->write_string(key, base_path_value, previous_base_path);
    };
    undo_.push(std::format("restore {}'s BasePath", distro_.name), restore_base_path);

    // Only when the distribution already had one. Adding the value to a legacy
    // entry that never had it would change its layout rather than repair it.
    if (previous_vhd_file_name.empty()) {
        return {};
    }
    const std::wstring name = target_.filename().wstring();
    if (const Status written = registry_->write_string(key, vhd_file_name_value, name);
        !written.has_value()) {
        return written;
    }
    auto restore_vhd_file_name = [registry, key, previous_vhd_file_name]() -> Status {
        return registry->write_string(key, vhd_file_name_value, previous_vhd_file_name);
    };
    undo_.push(std::format("restore {}'s VhdFileName", distro_.name), restore_vhd_file_name);
    return {};
}

Status MoveOperation::prove_it_boots() {
    // `/bin/sh -c :`, not `/bin/true`. POSIX guarantees `/bin/sh`; it does not
    // guarantee `/bin/true`, and NixOS-WSL ships without one -- a missing binary
    // boots the distribution and then fails the exec, which is
    // indistinguishable here from a boot failure.
    const std::vector<std::string> argv{"/bin/sh", "-c", ":"};
    const auto started = host_->run_as_root(distro_.name, argv, std::chrono::seconds{120});
    if (!started.has_value()) {
        return std::unexpected(started.error());
    }
    if (!started->succeeded()) {
        return fail(ErrorCode::Preflight,
                    std::format("{} did not start from {}", distro_.name, target_.string()),
                    "the registry entry has been put back and the original disk is untouched");
    }
    return {};
}

Result<Report> MoveOperation::execute(ProgressSink& progress) {
    Report report;
    std::size_t index = 0;

    const StepPlan transfer_step{
        .description = was_renamed()
                           ? std::format("move {} to {}", distro_.vhdx_path.string(), target_.string())
                           : std::format("copy {} to {}", distro_.vhdx_path.string(), target_.string()),
        .mutates = true};
    progress.step_started(index, transfer_step);
    if (const Status moved = transfer(progress); !moved.has_value()) {
        return std::unexpected(moved.error());
    }
    progress.step_finished(index, transfer_step);
    report.completed.push_back(transfer_step.description);
    ++index;

    const StepPlan repoint_step{.description = std::format("point {} at {}", distro_.name, target_.string()),
                                .mutates = true};
    progress.step_started(index, repoint_step);
    if (const Status pointed = repoint(); !pointed.has_value()) {
        return std::unexpected(pointed.error());
    }
    progress.step_finished(index, repoint_step);
    report.completed.push_back(repoint_step.description);
    ++index;

    const StepPlan smoke{.description = std::format("start {} to check the new path works", distro_.name)};
    progress.step_started(index, smoke);
    if (const Status booted = prove_it_boots(); !booted.has_value()) {
        return std::unexpected(booted.error());
    }
    progress.step_finished(index, smoke);
    report.completed.push_back(smoke.description);
    ++index;

    // Past here nothing is undone, which is why it is last and why it does not
    // happen until the distribution has been seen to boot from the new disk.
    if (!was_renamed() && !options_.keep_source) {
        const StepPlan cleanup{.description = std::format("delete {}", distro_.vhdx_path.string()),
                               .mutates = true};
        progress.step_started(index, cleanup);
        if (const Status removed = filesystem_->remove(distro_.vhdx_path); !removed.has_value()) {
            // The move worked; the leftover did not go away. Saying so is more
            // use than failing an operation that achieved what it set out to.
            progress.message(std::format("{} could not be deleted: {}", distro_.vhdx_path.string(),
                                         removed.error().message));
            progress.message("the move succeeded; the old file can be removed by hand");
        } else {
            progress.step_finished(index, cleanup);
            report.completed.push_back(cleanup.description);
        }
    }

    report.actual.bytes_freed = std::nullopt;
    return report;
}

Status MoveOperation::verify() {
    if (!filesystem_->exists(target_)) {
        return fail(ErrorCode::Partial, std::format("{} is not there after the move", target_.string()),
                    "run `wsldisk info` to see where the distribution points");
    }

    const std::wstring key = model::registry_key_for(distro_);
    const auto written = registry_->read_string(key, base_path_value);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    // An absent value compares unequal, which is the right answer: the intended
    // path is a directory and is never empty.
    if (written->value_or(std::wstring{}) != intended_base_path()) {
        return fail(ErrorCode::Partial, std::format("{}'s BasePath is not what was written", distro_.name),
                    "run `wsldisk info` to see where it points now");
    }
    return {};
}

void MoveOperation::rollback(ProgressSink& progress) noexcept {
    undo_.unwind(progress);
}

}  // namespace wsldisk::ops
