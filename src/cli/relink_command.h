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

/// What `relink` was asked to do.
struct RelinkOptions {
    /// The distribution whose registry entry gets rewritten.
    std::string name;
    /// The `.vhdx` it should point at.
    std::string path;
};

/// Points a distribution at a disk that has already been moved.
///
/// Shared with `orphans --relink`, which is the same operation reached from the
/// other direction: `orphans` finds a disk and asks who should own it, `relink`
/// knows who owns it and where it went. Two doors, one room -- a second copy of
/// this would be a second place for the rollback semantics to drift.
[[nodiscard]] int run_relink(const Services& services, const RelinkOptions& options,
                             const GlobalOptions& global, ILogger& logger, std::ostream& out,
                             std::ostream& err);

/// Adds the `relink` subcommand to `app`.
void add_relink_command(CLI::App& app, GlobalOptions& global, RelinkOptions& options);

}  // namespace wsldisk::cli
