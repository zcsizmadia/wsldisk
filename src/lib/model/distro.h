#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../errors.h"
#include "../interfaces.h"

namespace wsldisk::model {

/// One registered WSL distribution, as the registry describes it.
///
/// Text is UTF-8 -- this is the layer that converts, so nothing above it has to
/// think about UTF-16 -- with one deliberate exception: `base_path` keeps the
/// wide string exactly as stored, prefix and all.
struct Distro {
    /// `DistributionName`. The name every command takes and `wsl.exe` answers to.
    std::string name;
    /// The `{GUID}` subkey this came from, which is what identifies it when two
    /// distributions share a name (WSL allows it; the registry does not care).
    std::string guid;
    /// 1 or 2. WSL1 distributions enumerate normally and are refused later, by
    /// the shared preflight rather than here (PLAN.md D8).
    std::uint32_t version = 2;

    /// `BasePath` exactly as stored, *including* any `\\?\` prefix.
    ///
    /// The prefix form varies per distribution on one machine (spike #4), so
    /// `move` and `relink` have to write back whichever form they found. Storing
    /// a normalised copy and reconstructing the prefix later would be guesswork.
    std::wstring base_path;

    /// Where the disk actually is: `base_path` with any extended-length prefix
    /// stripped, joined with `VhdFileName` or `ext4.vhdx`.
    std::filesystem::path vhdx_path;

    std::uint32_t default_uid = 0;
    /// `Flags` was 15 on every distribution measured, so it identifies nothing
    /// on its own. Carried because `info` reports it, not because it decides
    /// anything.
    std::uint32_t flags = 0;
    std::uint32_t state = 0;

    /// Whether `DefaultDistribution` at the root points here.
    bool is_default = false;
    /// `Modern=1`: the layout `wsl --install` writes today. Absent on the MSIX
    /// packaged layout.
    bool modern = false;

    /// `Flavor` and `OsVersion`, recorded at import on modern layouts and absent
    /// on older ones.
    std::string flavor;
    std::string os_version;

    [[nodiscard]] bool is_wsl2() const noexcept { return version == 2; }
};

/// What enumeration found, plus what it had to skip.
///
/// A malformed key is a warning rather than a failure: one unusable registry
/// entry must not stop `list` reporting the distributions that are fine, and
/// `orphans` exists precisely to find such things.
struct DistroList {
    std::vector<Distro> distros;
    std::vector<std::string> warnings;

    /// The distribution `DefaultDistribution` points at, if it is registered.
    /// Absent when the value is missing or dangling -- both of which happen.
    [[nodiscard]] const Distro* default_distro() const noexcept;

    /// Case-insensitive, because `wsl.exe` matches names that way.
    [[nodiscard]] const Distro* find(std::string_view name) const noexcept;
};

/// Where WSL records its distributions, relative to `HKEY_CURRENT_USER`.
///
/// Exported so the canned hives in the tests are built at the same path the
/// code reads. They were not, and every unit test passed while `list` on a real
/// machine could not find the key at all.
[[nodiscard]] std::wstring_view lxss_key();

/// Reads every distribution out of the `Lxss` key.
///
/// Fails only when the key itself cannot be read. Anything wrong with an
/// individual subkey becomes a warning.
[[nodiscard]] Result<DistroList> enumerate(const IRegistry& registry);

/// Strips a `\\?\` or `\\?\UNC\` prefix, leaving a path the rest of Windows and
/// `std::filesystem` accept. Exposed for tests and for `orphans`.
[[nodiscard]] std::wstring strip_extended_prefix(std::wstring_view path);

/// Joins a `BasePath` and a `VhdFileName` into the disk's location. Exposed
/// because `orphans` builds the same path for keys that failed to enumerate.
[[nodiscard]] std::filesystem::path vhdx_path_for(std::wstring_view base_path,
                                                  std::wstring_view vhd_file_name);

}  // namespace wsldisk::model
