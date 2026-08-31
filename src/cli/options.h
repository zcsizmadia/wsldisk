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
/// `machine_readable` false leaves `--json` off, for a command whose stdout is
/// not data.
///
/// Only `completion` wants that: its output is a shell script meant to be
/// sourced, so there is nothing for `--json` to mean. Accepting the flag and
/// ignoring it is the failure this whole change is about -- a command that takes
/// an option and does nothing with it is worse than one that refuses it, because
/// the refusal is the only thing that tells the user their expectation is wrong.
void add_global_options(CLI::App& app, GlobalOptions& options, bool machine_readable = true);

}  // namespace wsldisk::cli
