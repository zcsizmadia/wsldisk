#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>

#include "errors.h"
#include "list_command.h"
#include "options.h"

namespace CLI {
class App;
}  // namespace CLI

namespace wsldisk::cli {

class ILogger;

/// What `config` was asked to do.
struct ConfigOptions {
    /// `path`, `get`, `set`, `edit` or `show`. Empty means `show`.
    std::string action;
    /// The setting for `get` and `set`. Empty for `get` means every setting.
    std::string key;
    /// The value for `set`.
    std::string value;
};

/// Opens a file in the user's editor. A function so a test can answer for it
/// rather than launching notepad.
using LaunchEditor = std::function<Status(const std::filesystem::path& path)>;

/// Opens `file` in the user's editor and waits for it to close.
///
/// The production `LaunchEditor`. A named function rather than a lambda at the
/// call site so it is reachable from a test: everything else about `config` is
/// driven through interfaces, and this was the one line that could only be
/// exercised by launching a real editor.
[[nodiscard]] Status open_in_editor(const IFileSystem& filesystem, const std::filesystem::path& file);

/// The editor command for `config edit`: `%EDITOR%`, or notepad.
///
/// Resolved through the filesystem rather than `getenv` so the choice is
/// testable, and because `%EDITOR%` may itself contain variables.
[[nodiscard]] std::string editor_command(const IFileSystem& filesystem);

/// Runs `config`, returning the process exit code.
[[nodiscard]] int run_config(const Services& services, const ConfigOptions& options,
                             const GlobalOptions& global, ILogger& logger, const LaunchEditor& launch,
                             std::ostream& out, std::ostream& err);

/// Adds the `config` subcommand and its four verbs to `app`.
void add_config_command(CLI::App& app, GlobalOptions& global, ConfigOptions& options);

}  // namespace wsldisk::cli
