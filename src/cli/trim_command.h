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

/// What `trim` was asked to do.
struct TrimOptions {
    std::string name;
};

/// Trims one distribution, returning the process exit code.
[[nodiscard]] int run_trim(const Services& services, const TrimOptions& options, const GlobalOptions& global,
                           ILogger& logger, std::ostream& out, std::ostream& err);

/// Adds the `trim` subcommand to `app`.
void add_trim_command(CLI::App& app, GlobalOptions& global, TrimOptions& options);

}  // namespace wsldisk::cli
