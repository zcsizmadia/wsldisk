#pragma once

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

/// What `compact` was asked to do.
struct CompactCommandOptions {
    /// The distribution to compact. Empty with `--all` or `--file`.
    std::string name;
    /// Every WSL2 distribution.
    bool all = false;
    /// A loose `.vhdx` instead of a distribution's.
    std::string file;
    /// Skip the `fstrim` step.
    bool no_trim = false;
    /// Permit `wsl --shutdown` when something else holds the disk (D9).
    bool shutdown = false;
    /// Start each distribution again afterwards if it was running.
    bool restart = false;

    /// Whether exactly one target was named. `--all`, `--file` and a bare name
    /// are mutually exclusive, and none of them is not a request.
    [[nodiscard]] bool targets_one_thing() const noexcept;
};

/// Compacts what was asked for, returning the process exit code.
[[nodiscard]] int run_compact(const Services& services, const CompactCommandOptions& options,
                              const GlobalOptions& global, ILogger& logger, std::ostream& out,
                              std::ostream& err);

/// Adds the `compact` subcommand to `app`.
void add_compact_command(CLI::App& app, GlobalOptions& global, CompactCommandOptions& options);

}  // namespace wsldisk::cli
