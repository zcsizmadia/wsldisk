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

/// The settings every command runs with, or the defaults when there are none.
///
/// Takes an `IFileSystem` rather than reading the environment itself, because
/// this used to sit inline in `app.cpp` beside the real Win32 services -- where
/// no test could reach its failure paths. A branch that cannot be tested is a
/// branch nobody has checked, and this one decides what `compact` does.
///
/// Neither failure stops the command. A missing file is the defaults and not an
/// error; a file that exists and does not parse is a warning and still the
/// defaults, because the user asked to compact a disk, not to have their
/// settings audited.
[[nodiscard]] model::Config load_configuration(const IFileSystem& filesystem, ILogger& logger);

/// Runs `config`, returning the process exit code.
[[nodiscard]] int run_config(const Services& services, const ConfigOptions& options,
                             const GlobalOptions& global, ILogger& logger, const LaunchEditor& launch,
                             std::ostream& out, std::ostream& err);

/// Adds the `config` subcommand and its four verbs to `app`.
void add_config_command(CLI::App& app, GlobalOptions& global, ConfigOptions& options);

}  // namespace wsldisk::cli
