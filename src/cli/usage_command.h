#pragma once

#include <cstddef>
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

/// What `usage` was asked to do.
struct UsageCommandOptions {
    /// The distribution to look inside.
    std::string name;
    /// Show at most this many entries. Zero means all of them.
    std::size_t top = 0;
    /// Also break the guest down by directory, not just by catalogue entry.
    bool by_directory = false;
    /// How deep that breakdown goes. `/var/lib` is depth 2.
    std::size_t depth = 2;
};

/// Reports where the space inside a distribution went.
///
/// `compact` answers "give me the space back"; this answers the question that
/// comes before it. Read-only in the strict sense: it runs `du` and `df` and
/// deletes nothing, ever. `clean` is the command that acts on this.
[[nodiscard]] int run_usage(const Services& services, const UsageCommandOptions& options,
                            const GlobalOptions& global, ILogger& logger, std::ostream& out,
                            std::ostream& err);

/// Adds the `usage` subcommand to `app`.
void add_usage_command(CLI::App& app, GlobalOptions& global, UsageCommandOptions& options);

}  // namespace wsldisk::cli
