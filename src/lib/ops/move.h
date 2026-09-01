#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "../interfaces.h"
#include "../model/distro.h"
#include "operation.h"

namespace wsldisk::ops {

/// How `move` should go about it.
struct MoveOptions {
    /// Leave the original file where it was.
    ///
    /// Off by default -- a move that leaves both copies has not moved anything
    /// and quietly costs the space twice. On for someone who wants the old disk
    /// kept until they are satisfied, which is a reasonable thing to want of an
    /// operation this size.
    bool keep_source = false;
};

/// Relocates a distribution's virtual disk to another directory or drive.
///
/// The alternative WSL offers is `wsl --export` followed by `wsl --import`,
/// which is slow, loses the default user and the flags, and hands the
/// distribution a new GUID. Moving the file and repointing `BasePath` keeps all
/// of it (decision D4).
///
/// The order is the whole safety argument. The source file is the only copy of
/// the user's data until the new one has been proved to boot, so nothing deletes
/// it before then, and every step before that point can be undone:
///
/// 1. copy (or rename, on one volume) -- undo: delete the copy
/// 2. repoint the registry -- undo: write the old values back
/// 3. start the distribution and run a command -- reads only
/// 4. delete the source -- the point of no return, and last
///
/// A failure at step 3 therefore leaves a working distribution pointing at the
/// disk it started with, not two halves of one.
class MoveOperation final : public IOperation {
public:
    /// `destination` is the directory to move the disk into; the file keeps its
    /// name, because `VhdFileName` and WSL's own conventions both depend on it.
    MoveOperation(IRegistry& registry, IFileSystem& filesystem, const IWslHost& host, model::Distro distro,
                  std::filesystem::path destination, MoveOptions options = {});

    [[nodiscard]] Result<Plan> plan() override;
    [[nodiscard]] Result<Report> execute(ProgressSink& progress) override;
    [[nodiscard]] Status verify() override;
    void rollback(ProgressSink& progress) noexcept override;

    /// Where the disk will end up.
    [[nodiscard]] const std::filesystem::path& target() const noexcept { return target_; }

    /// What `BasePath` will be written as, in whichever prefix form the
    /// distribution already used.
    ///
    /// Preserved rather than normalised because the form varies per distribution
    /// on one machine (spike #4), and rewriting Docker Desktop's
    /// extended-length path as a bare one is an unrequested change to a value
    /// something else owns.
    [[nodiscard]] std::wstring intended_base_path() const;

    /// What the disk occupied before the move.
    ///
    /// Zero until `plan()` has succeeded; every path that reports a move has
    /// been through it, so there is no "unknown" case for a caller to handle.
    [[nodiscard]] std::uint64_t size_on_disk() const noexcept { return size_; }

    /// Whether the disk was moved by renaming it rather than copying it.
    ///
    /// Known after `plan()`. A rename is instant and moves no bytes, so it is
    /// the route whenever the volumes match -- *except* under `--keep-source`,
    /// which asks for the original to stay: a rename cannot leave it behind, so
    /// wanting two files means copying even when one volume would have done.
    [[nodiscard]] bool was_renamed() const noexcept { return same_volume_ && !options_.keep_source; }

    /// Whether both paths are on one volume, which is a fact about the machine
    /// rather than a statement about what the move did.
    [[nodiscard]] bool is_same_volume() const noexcept { return same_volume_; }

private:
    /// Moves the file, by whichever route the volumes allow.
    [[nodiscard]] Status transfer(ProgressSink& progress);

    /// Rewrites `BasePath` and, where the distribution has one, `VhdFileName`.
    [[nodiscard]] Status repoint();

    /// Starts the distribution and runs a command in it.
    [[nodiscard]] Status prove_it_boots();

    IRegistry* registry_;
    IFileSystem* filesystem_;
    const IWslHost* host_;
    model::Distro distro_;
    /// The directory the user named.
    std::filesystem::path destination_;
    /// The file inside it.
    std::filesystem::path target_;
    MoveOptions options_;
    std::uint64_t size_ = 0;
    bool same_volume_ = false;
    UndoStack undo_;
};

}  // namespace wsldisk::ops
