#pragma once

#include <filesystem>
#include <string>

// Forward-declared rather than including CLI11 here: the header is large and
// every translation unit that only wants GlobalOptions would pay for it.
// NOLINTNEXTLINE(readability-identifier-naming) -- CLI11's namespace, not ours.
namespace CLI {
class App;
}  // namespace CLI

namespace wsldisk::cli {

/// Flags every command shares.
///
/// Held in one place so `list --json` and `compact --json` cannot drift apart,
/// and so a new command gets the whole set by construction rather than by
/// remembering.
struct GlobalOptions {
    /// Machine-readable output on stdout. One object per line when a command
    /// reports many things, a single object otherwise.
    bool json = false;
    /// Extra detail. Goes to **stderr**, so `--json` stdout stays parseable
    /// even with `-v` on.
    bool verbose = false;
    /// Assume yes for anything that would otherwise prompt.
    bool assume_yes = false;
    /// Plan and print, change nothing.
    bool dry_run = false;
    /// Append a log to this file as well as writing to stderr.
    std::filesystem::path log_file;
};

/// Adds the shared flags to `app`, writing into `options`.
///
/// Called once per (sub)command so the flags work in either position --
/// `wsldisk --json list` and `wsldisk list --json` both being natural is worth
/// more than the duplication costs.
void add_global_options(CLI::App& app, GlobalOptions& options);

}  // namespace wsldisk::cli
