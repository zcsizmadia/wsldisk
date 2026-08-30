#include "orphans.h"

#include <algorithm>
#include <format>

#include "text.h"

namespace wsldisk::model {
namespace {

/// The one wildcard component a scan pattern may contain.
constexpr std::wstring_view wildcard = L"*";

/// What a disk is called when nothing says otherwise, and the extension every
/// scan looks for.
constexpr std::wstring_view vhdx_pattern = L"*.vhdx";

[[nodiscard]] wchar_t lower(wchar_t character) {
    return (character >= L'A' && character <= L'Z') ? static_cast<wchar_t>(character - L'A' + L'a')
                                                    : character;
}

/// Whether one directory entry is a disk worth reporting.
///
/// `seen` grows as it goes: a directory can be reached by more than one
/// pattern, and the same file must not be reported twice.
[[nodiscard]] bool is_unclaimed_disk(const DirectoryEntry& entry, const std::vector<std::wstring>& claimed,
                                     std::vector<std::wstring>& seen) {
    if (entry.is_directory) {
        return false;
    }
    // The filesystem matches `*.vhdx` against the 8.3 short name as well, so
    // `disk.vhdx.bak` can come back. Re-check the extension we actually meant.
    if (canonical_path(entry.path.extension().wstring()) != L".vhdx") {
        return false;
    }

    const std::wstring canonical = canonical_path(entry.path.wstring());
    if (std::ranges::find(claimed, canonical) != claimed.end()) {
        return false;
    }
    if (std::ranges::find(seen, canonical) != seen.end()) {
        return false;
    }
    seen.push_back(canonical);
    return true;
}

/// Appends every disk in `entries` that nothing claims, measured.
void collect_unclaimed(const IFileSystem& filesystem, const std::vector<DirectoryEntry>& entries,
                       const std::vector<std::wstring>& claimed, std::vector<std::wstring>& seen,
                       std::vector<Orphan>& orphans) {
    for (const DirectoryEntry& entry : entries) {
        if (!is_unclaimed_disk(entry, claimed, seen)) {
            continue;
        }
        Orphan orphan{.path = entry.path};
        // Left unset when it cannot be measured: a file that went away between
        // the listing and the query is still worth reporting, without a size.
        if (const auto size = filesystem.file_size_on_disk(entry.path); size.has_value()) {
            orphan.size_on_disk = *size;
        }
        orphans.push_back(std::move(orphan));
    }
}

}  // namespace

std::wstring canonical_path(std::wstring_view path) {
    std::wstring result = strip_extended_prefix(path);

    // A trailing separator is not part of the name. A directory listing and a
    // registry value disagree about it often enough to matter.
    while (result.size() > 1 && (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }

    for (wchar_t& character : result) {
        // Forward slashes are legal in Win32 paths and appear in
        // hand-edited registry values.
        if (character == L'/') {
            character = L'\\';
        }
        character = lower(character);
    }
    return result;
}

bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
    return canonical_path(left.wstring()) == canonical_path(right.wstring());
}

std::vector<std::filesystem::path> expand_scan_pattern(const IFileSystem& filesystem,
                                                       const std::filesystem::path& pattern) {
    // Split at the wildcard component, if there is one.
    std::filesystem::path before;
    std::filesystem::path after;
    bool seen_wildcard = false;
    for (const std::filesystem::path& part : pattern) {
        if (!seen_wildcard && part.wstring() == wildcard) {
            seen_wildcard = true;
            continue;
        }
        (seen_wildcard ? after : before) /= part;
    }

    if (!seen_wildcard) {
        return {pattern};
    }

    const auto entries = filesystem.list_directory(before, L"*");
    if (!entries.has_value()) {
        return {};
    }

    std::vector<std::filesystem::path> directories;
    for (const DirectoryEntry& entry : *entries) {
        if (!entry.is_directory) {
            continue;
        }
        directories.push_back(after.empty() ? entry.path : entry.path / after);
    }
    return directories;
}

Result<std::vector<std::filesystem::path>> default_scan_patterns(const IFileSystem& filesystem) {
    const auto local = filesystem.expand_environment(LR"(%LOCALAPPDATA%)");
    if (!local.has_value()) {
        return std::unexpected(local.error());
    }

    return std::vector<std::filesystem::path>{
        *local / LR"(wsl\*)",                  // what `wsl --install` writes today
        *local / LR"(Packages\*\LocalState)",  // the legacy MSIX layout
        *local / LR"(Docker\wsl\*)",           // Docker Desktop
    };
}

std::vector<Orphan> find_orphans(const IFileSystem& filesystem, const DistroList& distros,
                                 const std::vector<std::filesystem::path>& patterns,
                                 std::vector<std::string>& warnings) {
    // Every disk a distribution claims, in the form paths are compared in.
    std::vector<std::wstring> claimed;
    claimed.reserve(distros.distros.size());
    for (const Distro& distro : distros.distros) {
        claimed.push_back(canonical_path(distro.vhdx_path.wstring()));
    }

    // A directory can be reached by more than one pattern, and the same file
    // must not be reported twice.
    std::vector<std::wstring> seen;
    std::vector<Orphan> orphans;

    for (const std::filesystem::path& pattern : patterns) {
        for (const std::filesystem::path& directory : expand_scan_pattern(filesystem, pattern)) {
            const auto entries = filesystem.list_directory(directory, vhdx_pattern);
            if (!entries.has_value()) {
                // Skipped rather than fatal: the point is to find what is
                // there, and one unreadable directory must not hide the rest.
                warnings.push_back(entries.error().to_string());
                continue;
            }
            collect_unclaimed(filesystem, *entries, claimed, seen, orphans);
        }
    }
    return orphans;
}

}  // namespace wsldisk::model
