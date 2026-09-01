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

/// What `move` was asked to do.
struct MoveOptions {
    /// The distribution whose disk gets relocated.
    std::string name;
    /// The directory to move it into. The file keeps its name.
    std::string destination;
    /// Leave the original where it was.
    bool keep_source = false;
};

/// Relocates a distribution's virtual disk to another directory or drive.
///
/// The thing WSL itself makes hard: its own answer is `wsl --export` followed by
/// `wsl --import`, which is slow and loses the default user, the flags and the
/// GUID. This moves the file and repoints the registry, and proves the
/// distribution still boots before it deletes anything.
[[nodiscard]] int run_move(const Services& services, const MoveOptions& options, const GlobalOptions& global,
                           ILogger& logger, std::ostream& out, std::ostream& err);

/// Adds the `move` subcommand to `app`.
void add_move_command(CLI::App& app, GlobalOptions& global, MoveOptions& options);

}  // namespace wsldisk::cli
