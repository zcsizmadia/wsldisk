#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../errors.h"
#include "../interfaces.h"
#include "distro.h"

namespace wsldisk::model {

/// A `.vhdx` on disk that no registry entry resolves to.
struct Orphan {
    std::filesystem::path path;
    /// What it is costing, so `orphans` can say what deleting would recover.
    /// Unknown when the file could not be measured.
    std::optional<std::uint64_t> size_on_disk;
};

/// A path reduced to a form two spellings of the same file share.
///
/// The extended-length prefix is stripped, trailing separators dropped and the
/// whole thing lower-cased. All three matter: `BasePath` is stored with the
/// `\\?\` prefix on some distributions and without on others *on the same
/// machine* (spike #4), Windows paths are case-insensitive, and a directory
/// listing and a registry value disagree about trailing separators often enough
/// to matter. Without this a distribution's own disk looks orphaned.
[[nodiscard]] std::wstring canonical_path(std::wstring_view path);

/// Whether two paths name the same file.
[[nodiscard]] bool same_path(const std::filesystem::path& left, const std::filesystem::path& right);

/// Expands one scan pattern into the directories it names.
///
/// A pattern may contain a single `*` component: `...\wsl\*` is every
/// subdirectory of `wsl`, and `...\Packages\*\LocalState` is the `LocalState`
/// inside each package. That is the shape the two real WSL layouts take, and
/// supporting exactly it beats a general glob nobody needs.
[[nodiscard]] std::vector<std::filesystem::path> expand_scan_pattern(const IFileSystem& filesystem,
                                                                     const std::filesystem::path& pattern);

/// The directories WSL and Docker Desktop actually put disks in.
///
/// Both layouts exist in the wild on the same machine (spike #4), so all of
/// them are searched rather than guessing which one this machine uses. Returns
/// the patterns unexpanded; `%LOCALAPPDATA%` is resolved through the
/// filesystem so a test can point it somewhere else.
[[nodiscard]] Result<std::vector<std::filesystem::path>> default_scan_patterns(const IFileSystem& filesystem);

/// Finds every `.vhdx` under `patterns` that no distribution claims.
///
/// A directory that cannot be read is skipped rather than failing the scan: the
/// point is to find what is there, and one unreadable directory should not hide
/// the rest.
[[nodiscard]] std::vector<Orphan> find_orphans(const IFileSystem& filesystem, const DistroList& distros,
                                               const std::vector<std::filesystem::path>& patterns,
                                               std::vector<std::string>& warnings);

}  // namespace wsldisk::model
