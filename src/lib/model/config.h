#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../errors.h"
#include "../interfaces.h"

namespace wsldisk::model {

/// The settings `%APPDATA%\wsldisk\config.toml` can carry.
///
/// Every field has a default that matches the command's own default, so a
/// machine with no config file behaves exactly like one with an empty file.
/// That is why a missing file is not an error: there is nothing it could say
/// that the defaults do not already say.
struct Config {
    /// Extra roots for `orphans` to scan, on top of the built-in three.
    std::vector<std::string> scan_dirs;

    /// The default for `compact`'s trim step. `--no-trim` still overrides it.
    bool compact_trim = true;

    /// Whether `compact` starts a distribution again afterwards by default.
    bool compact_restart = false;

    /// How long `compact` waits for the disk after terminating.
    ///
    /// Seconds in the file because that is what a human writes; the operation
    /// takes a duration.
    std::uint32_t unlock_timeout_seconds = 90;

    [[nodiscard]] std::chrono::milliseconds unlock_timeout() const noexcept {
        return std::chrono::seconds{unlock_timeout_seconds};
    }
};

/// The largest `unlock_timeout_seconds` the config will accept.
///
/// The utility VM idles out in about a minute once nothing is running, and if
/// something *is* running it never does -- so past a couple of minutes a longer
/// wait only delays a refusal the user has to act on. An hour is far past useful
/// and still leaves the value obviously a number of seconds.
///
/// Lives here rather than in the parser because `compact` suggests a longer wait
/// in its refusal, and a suggestion the config would then reject is worse than
/// no suggestion at all.
constexpr std::uint32_t max_unlock_timeout_seconds = 3600;

/// Every key `get` and `set` accept, in the order `wsldisk config get` prints
/// them with no argument.
///
/// Exported so the CLI, the tests and the error message for an unknown key all
/// work from one list rather than three that can drift.
[[nodiscard]] std::vector<std::string> config_keys();

/// Parses `config.toml`.
///
/// A malformed file is a `Usage` error carrying the line and column, because
/// the only thing the user can do about it is go and look. An unknown key is
/// *not* an error: a config written by a later version has to stay readable by
/// an earlier one, and refusing to start over a key we do not know would make
/// every upgrade a breaking change.
[[nodiscard]] Result<Config> parse_config(std::string_view text);

/// Renders a config back to TOML.
///
/// `config set` goes value -> struct -> text, never text -> text: string-editing
/// a config file is how comments end up inside string literals and how a
/// half-written value becomes a parse error the user did not cause.
[[nodiscard]] std::string render_config(const Config& config);

/// Reads one key, in the dotted form `compact.trim`.
///
/// Absent when the key is not one this version knows.
[[nodiscard]] std::optional<std::string> get_config_value(const Config& config, std::string_view key);

/// Sets one key from its text form.
///
/// Fails with `Usage` for a key this version does not know, and for a value the
/// key cannot take -- `compact.trim = maybe` is refused here rather than
/// written and rejected on the next run.
[[nodiscard]] Status set_config_value(Config& config, std::string_view key, std::string_view value);

/// Where the config file lives: `%APPDATA%\wsldisk\config.toml`.
[[nodiscard]] Result<std::filesystem::path> config_path(const IFileSystem& filesystem);

/// Loads the config, or the defaults when there is no file.
///
/// A file that is not there is not a failure; a file that is there and will not
/// parse is.
[[nodiscard]] Result<Config> load_config(const IFileSystem& filesystem, const std::filesystem::path& path);

/// The disk-relevant keys of `%USERPROFILE%\.wslconfig`.
///
/// Displayed, never written. `.wslconfig` belongs to WSL and to whatever else
/// the user has editing it, and a tool that rewrites someone else's file to
/// change one line is a tool that eventually loses their comments.
struct WslConfig {
    /// `[wsl2] defaultVhdSize` -- the size new distributions are created with.
    std::optional<std::string> default_vhd_size;
    /// `[wsl2] vhdSize` -- an older spelling some documentation still shows.
    std::optional<std::string> vhd_size;
    /// `[wsl2] swapFile` -- where the swap VHDX lives, which is a disk this
    /// tool will otherwise report as an orphan.
    std::optional<std::string> swap_file;

    [[nodiscard]] bool empty() const noexcept {
        return !default_vhd_size.has_value() && !vhd_size.has_value() && !swap_file.has_value();
    }
};

/// Parses the three keys above out of `.wslconfig`.
///
/// `.wslconfig` is INI, not TOML: bare values, `#` and `;` comments, and no
/// quoting. Parsing it with a TOML parser would reject files WSL itself
/// accepts, so this reads the handful of lines it cares about and ignores
/// everything else -- including sections other than `[wsl2]`.
[[nodiscard]] WslConfig parse_wslconfig(std::string_view text);

/// Where `.wslconfig` lives: `%USERPROFILE%\.wslconfig`.
[[nodiscard]] Result<std::filesystem::path> wslconfig_path(const IFileSystem& filesystem);

}  // namespace wsldisk::model
