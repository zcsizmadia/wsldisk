#include "config.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>
#include <toml++/toml.hpp>

namespace wsldisk::model {
namespace {

constexpr std::string_view scan_dirs_key = "scan.dirs";
constexpr std::string_view compact_trim_key = "compact.trim";
constexpr std::string_view compact_restart_key = "compact.restart";
constexpr std::string_view unlock_timeout_key = "wsl.unlock_timeout_seconds";

/// The largest wait that is still a wait rather than a hang.
///
/// The handle is never released on a timer (D9), so a long timeout only delays
/// a refusal the user has to act on. An hour is far past useful and still
/// leaves the value obviously a number of seconds.
constexpr std::uint32_t max_unlock_timeout_seconds = 3600;

/// `true`/`false`, and nothing else.
///
/// Not `1`/`yes`/`on`: TOML has one spelling for a boolean, and accepting more
/// here would mean `config set` writes a file that `config get` reads back
/// differently.
[[nodiscard]] std::optional<bool> parse_bool(std::string_view text) {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> parse_seconds(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > max_unlock_timeout_seconds) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

/// Renders a string as a TOML basic string, escaping what has to be escaped.
///
/// Windows paths are the reason this exists: `D:\WSL` in a bare string would
/// read `\W` as an escape and fail to parse on the way back in.
///
/// Not called `quoted`: `<sstream>` drags in `std::quoted`, and an unqualified
/// call with a `std::string` argument finds it by ADL and prefers it, because
/// `std::string` binds to its `const std::string&` exactly where this one needs
/// a conversion to `string_view`. MSVC and clang disagreed about which was
/// picked, which is how this was found.
[[nodiscard]] std::string as_toml_string(std::string_view text) {
    std::string result = "\"";
    for (const char character : text) {
        if (character == '\\' || character == '"') {
            result += '\\';
        }
        result += character;
    }
    result += '"';
    return result;
}

/// Trims spaces and tabs from both ends.
[[nodiscard]] std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

/// What to call a setting's type in an error the user reads.
template <typename T>
[[nodiscard]] constexpr std::string_view type_name() {
    if constexpr (std::is_same_v<T, bool>) {
        return "true or false";
    } else if constexpr (std::is_integral_v<T>) {
        return "whole number";
    } else {
        return "string";
    }
}

/// Reads one setting, refusing a key that is there with the wrong type.
///
/// `at_path(...).value_or(default)` silently returned the default when the value
/// had the wrong type or was out of range, so `trim = "false"` -- a string --
/// left trimming on, and `config get compact.trim` then said `true` while the
/// file plainly said otherwise. An out-of-range `unlock_timeout_seconds` *was*
/// refused, so the file was strict about one mistake and silent about the
/// neighbouring one.
///
/// Absent keys keep the default, which is not a mistake: the defaults are a
/// complete configuration.
template <typename T>
[[nodiscard]] Status read_into(const toml::table& table, std::string_view key, T& target) {
    const auto node = table.at_path(key);
    if (!node) {
        return {};
    }
    const std::optional<T> value = node.value<T>();
    if (!value.has_value()) {
        return fail(ErrorCode::Usage, std::format("{} is not a {}", key, type_name<T>()),
                    "check the value's type, or delete the line to use the default");
    }
    target = *value;
    return {};
}

}  // namespace

std::vector<std::string> config_keys() {
    return {std::string{scan_dirs_key}, std::string{compact_trim_key}, std::string{compact_restart_key},
            std::string{unlock_timeout_key}};
}

Result<Config> parse_config(std::string_view text) {
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& error) {
        // The position is the whole value of the message: "invalid config" sends
        // the user to read the file top to bottom.
        return fail(
            ErrorCode::Usage,
            std::format("config.toml is not valid TOML at line {}, column {}: {}", error.source().begin.line,
                        error.source().begin.column, std::string{error.description()}),
            "fix that line, or delete the file to start again from the defaults");
    }

    Config config;
    if (const auto* dirs = table.at_path(scan_dirs_key).as_array(); dirs != nullptr) {
        for (const auto& entry : *dirs) {
            if (const auto value = entry.value<std::string>(); value.has_value()) {
                config.scan_dirs.push_back(*value);
            }
        }
    }
    if (const Status read = read_into(table, compact_trim_key, config.compact_trim); !read.has_value()) {
        return std::unexpected(read.error());
    }
    if (const Status read = read_into(table, compact_restart_key, config.compact_restart);
        !read.has_value()) {
        return std::unexpected(read.error());
    }
    if (const Status read = read_into(table, unlock_timeout_key, config.unlock_timeout_seconds);
        !read.has_value()) {
        return std::unexpected(read.error());
    }
    if (config.unlock_timeout_seconds > max_unlock_timeout_seconds) {
        return fail(ErrorCode::Usage,
                    std::format("{} is {}, which is more than the {} second maximum", unlock_timeout_key,
                                config.unlock_timeout_seconds, max_unlock_timeout_seconds),
                    "the disk is never released on a timer, so a long wait only delays a "
                    "refusal; use a value under an hour");
    }
    return config;
}

std::string render_config(const Config& config) {
    std::ostringstream out;
    out << "# wsldisk configuration. Every value here is a default that the\n"
           "# matching command-line flag still overrides.\n\n";

    out << "[scan]\n";
    out << "# Extra roots for `wsldisk orphans` to search, on top of the built-in three.\n";
    out << "dirs = [";
    for (std::size_t index = 0; index < config.scan_dirs.size(); ++index) {
        out << (index == 0 ? "" : ", ") << as_toml_string(config.scan_dirs[index]);
    }
    out << "]\n\n";

    out << "[compact]\n";
    out << "# Run fstrim before compacting. `--no-trim` overrides this.\n";
    out << "trim = " << (config.compact_trim ? "true" : "false") << '\n';
    out << "# Start the distribution again afterwards if it was running.\n";
    out << "restart = " << (config.compact_restart ? "true" : "false") << "\n\n";

    out << "[wsl]\n";
    out << "# How long `compact` waits for the disk after terminating.\n";
    out << "unlock_timeout_seconds = " << config.unlock_timeout_seconds << '\n';
    return out.str();
}

std::optional<std::string> get_config_value(const Config& config, std::string_view key) {
    if (key == scan_dirs_key) {
        std::string joined;
        for (const std::string& dir : config.scan_dirs) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += dir;
        }
        return joined;
    }
    if (key == compact_trim_key) {
        return config.compact_trim ? "true" : "false";
    }
    if (key == compact_restart_key) {
        return config.compact_restart ? "true" : "false";
    }
    if (key == unlock_timeout_key) {
        return std::to_string(config.unlock_timeout_seconds);
    }
    return std::nullopt;
}

Status set_config_value(Config& config, std::string_view key, std::string_view value) {
    if (key == scan_dirs_key) {
        // Semicolon-separated, because a Windows path can contain a comma but
        // never a semicolon: `PATH` has used the same separator for the same
        // reason for thirty years.
        config.scan_dirs.clear();
        for (std::size_t start = 0; start <= value.size();) {
            const std::size_t end = std::min(value.find(';', start), value.size());
            if (const std::string_view part = trimmed(value.substr(start, end - start)); !part.empty()) {
                config.scan_dirs.emplace_back(part);
            }
            start = end + 1;
        }
        return {};
    }

    if (key == compact_trim_key || key == compact_restart_key) {
        const auto parsed = parse_bool(value);
        if (!parsed.has_value()) {
            return fail(ErrorCode::Usage, std::format("{} takes true or false, not `{}`", key, value),
                        "write `true` or `false`");
        }
        (key == compact_trim_key ? config.compact_trim : config.compact_restart) = *parsed;
        return {};
    }

    if (key == unlock_timeout_key) {
        const auto parsed = parse_seconds(value);
        if (!parsed.has_value()) {
            return fail(ErrorCode::Usage,
                        std::format("{} takes a whole number of seconds up to {}, not `{}`", key,
                                    max_unlock_timeout_seconds, value),
                        "the disk is never released on a timer, so a long wait only delays a "
                        "refusal; use a value under an hour");
        }
        config.unlock_timeout_seconds = *parsed;
        return {};
    }

    std::string known;
    for (const std::string& candidate : config_keys()) {
        if (!known.empty()) {
            known += ", ";
        }
        known += candidate;
    }
    return fail(ErrorCode::Usage, std::format("there is no setting called `{}`", key),
                std::format("the settings are: {}", known));
}

Result<std::filesystem::path> config_path(const IFileSystem& filesystem) {
    const auto appdata = filesystem.expand_environment(LR"(%APPDATA%)");
    if (!appdata.has_value()) {
        return std::unexpected(appdata.error());
    }
    return *appdata / L"wsldisk" / L"config.toml";
}

Result<Config> load_config(const IFileSystem& filesystem, const std::filesystem::path& path) {
    if (!filesystem.exists(path)) {
        // Not an error: the defaults are a complete configuration, and asking
        // the user to create a file that would say exactly what is already true
        // would be ceremony.
        return Config{};
    }

    const auto text = filesystem.read_text_file(path);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return parse_config(*text);
}

WslConfig parse_wslconfig(std::string_view text) {
    WslConfig config;
    bool in_wsl2 = false;

    for (std::size_t start = 0; start <= text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = trimmed(text.substr(start, end - start));
        start = end + 1;

        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[') {
            // Anything outside [wsl2] is someone else's setting.
            in_wsl2 = line == "[wsl2]";
            continue;
        }
        if (!in_wsl2) {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view name = trimmed(line.substr(0, equals));
        const std::string_view value = trimmed(line.substr(equals + 1));
        if (name == "defaultVhdSize") {
            config.default_vhd_size = std::string{value};
        } else if (name == "vhdSize") {
            config.vhd_size = std::string{value};
        } else if (name == "swapFile") {
            config.swap_file = std::string{value};
        }
    }
    return config;
}

Result<std::filesystem::path> wslconfig_path(const IFileSystem& filesystem) {
    const auto profile = filesystem.expand_environment(LR"(%USERPROFILE%)");
    if (!profile.has_value()) {
        return std::unexpected(profile.error());
    }
    return *profile / L".wslconfig";
}

}  // namespace wsldisk::model
