#pragma once

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "errors.h"
#include "list_command.h"
#include "model/orphans.h"
#include "options.h"

namespace CLI {
class App;
}  // namespace CLI

namespace wsldisk::cli {

class ILogger;

/// What `orphans` was asked to do.
struct OrphansOptions {
    /// Extra directories to search, on top of the defaults.
    std::vector<std::string> scan_dirs;
    /// Delete what was found, after confirming.
    bool remove = false;
    /// Point a distribution at a disk. Both are required together.
    std::string relink_distro;
    std::string relink_path;

    [[nodiscard]] bool relinking() const noexcept { return !relink_distro.empty(); }
};

/// Asks the user to confirm a destructive step.
///
/// A function rather than reading stdin directly, so the tests can answer it
/// and so `--yes` is one place rather than a check inside the delete loop.
using Confirm = std::function<bool(std::string_view question)>;

/// Reads a yes/no answer from `in`. Anything that is not `y` or `yes` is no --
/// a destructive default must be the safe one, and end-of-input (a piped
/// command with nothing to answer with) is not consent.
[[nodiscard]] bool ask(std::istream& in, std::ostream& out, std::string_view question);

/// A `Confirm` that reads the answer from `in`.
///
/// `in` and `out` are captured by reference and must outlive the returned
/// function. In production both are process-lifetime streams.
[[nodiscard]] Confirm console_confirm(std::istream& in, std::ostream& out);

/// Finds the orphaned disks.
[[nodiscard]] Result<std::vector<model::Orphan>> scan_orphans(const Services& services,
                                                              const OrphansOptions& options, ILogger& logger);

/// Deletes the listed disks, after confirming once for the whole set.
///
/// One question rather than one per file: a prompt repeated five times trains
/// the answer rather than the decision.
[[nodiscard]] int delete_orphans(const Services& services, const std::vector<model::Orphan>& orphans,
                                 const GlobalOptions& global, const Confirm& confirm, std::ostream& out,
                                 std::ostream& err);

/// Gathers and renders, returning the process exit code.
[[nodiscard]] int run_orphans(const Services& services, const OrphansOptions& options,
                              const GlobalOptions& global, ILogger& logger, const Confirm& confirm,
                              std::ostream& out, std::ostream& err);

/// Adds the `orphans` subcommand to `app`.
void add_orphans_command(CLI::App& app, GlobalOptions& global, OrphansOptions& options);

}  // namespace wsldisk::cli
