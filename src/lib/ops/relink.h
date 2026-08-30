#pragma once

#include <filesystem>
#include <string>

#include "../interfaces.h"
#include "../model/distro.h"
#include "operation.h"

namespace wsldisk::ops {

/// Points a distribution's registry entry at a disk that has moved.
///
/// The mutating half of `orphans`. It rewrites `BasePath` (and `VhdFileName`
/// when the distribution has one), then starts the distribution as a smoke
/// test: a registry entry that parses but does not boot is worse than one that
/// is obviously wrong, because the user would find out later and from WSL
/// rather than from here.
///
/// If the smoke test fails the registry goes back exactly as it was. That is
/// the whole reason this is an `IOperation` rather than two registry writes.
class RelinkOperation final : public IOperation {
public:
    RelinkOperation(IRegistry& registry, const IFileSystem& filesystem, const IWslHost& host,
                    model::Distro distro, std::filesystem::path target);

    [[nodiscard]] Result<Plan> plan() override;
    [[nodiscard]] Result<Report> execute(ProgressSink& progress) override;
    [[nodiscard]] Status verify() override;
    void rollback(ProgressSink& progress) noexcept override;

    /// What `BasePath` will be written as: the directory holding `target`, in
    /// whichever prefix form the distribution already used.
    ///
    /// The form is preserved rather than normalised because it varies per
    /// distribution on one machine (spike #4), and rewriting Docker Desktop's
    /// extended-length path as a bare one is a change nobody asked for to a
    /// value something else owns.
    [[nodiscard]] std::wstring intended_base_path() const;

private:
    IRegistry* registry_;
    const IFileSystem* filesystem_;
    const IWslHost* host_;
    model::Distro distro_;
    std::filesystem::path target_;
    UndoStack undo_;
};

}  // namespace wsldisk::ops
