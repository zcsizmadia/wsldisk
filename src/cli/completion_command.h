#pragma once

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "errors.h"
#include "options.h"

namespace CLI {
class App;
}  // namespace CLI

namespace wsldisk::cli {

/// What `completion` was asked for.
struct CompletionOptions {
    /// `powershell`, `bash` or `zsh`.
    std::string shell;
};

/// The shells `completion` can emit for, in the order `--help` lists them.
[[nodiscard]] std::vector<std::string> completion_shells();

/// Emits a completion script for `shell`, read out of `app`'s command tree.
///
/// Generated rather than written by hand, and generated from the *real* tree
/// rather than a copy of it: a flag that exists appears in the completions, and
/// one that is renamed is renamed in both. A hand-maintained script is wrong
/// the first time anyone adds an option and does not notice.
///
/// Distribution names are not baked in -- the script asks `wsldisk list --json`
/// at completion time, because the answer changes between one invocation and
/// the next.
[[nodiscard]] std::string generate_completion(const CLI::App& app, std::string_view shell);

/// Writes the script for the named shell, returning the process exit code.
[[nodiscard]] int run_completion(const CompletionOptions& options, const GlobalOptions& global,
                                 std::ostream& out, std::ostream& err);

/// Adds the `completion` subcommand to `app`.
void add_completion_command(CLI::App& app, GlobalOptions& global, CompletionOptions& options);

}  // namespace wsldisk::cli
