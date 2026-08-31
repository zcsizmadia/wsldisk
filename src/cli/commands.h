#pragma once

#include "compact_command.h"
#include "completion_command.h"
#include "config_command.h"
#include "info_command.h"
#include "list_command.h"
#include "options.h"
#include "orphans_command.h"
#include "trim_command.h"

namespace CLI {
class App;
}  // namespace CLI

namespace wsldisk::cli {

/// Somewhere for every command's parsed arguments to live.
///
/// One struct rather than eight locals so the whole tree can be built by one
/// call, which is what lets `completion` build a throwaway copy and walk it.
struct CommandOptions {
    GlobalOptions global;
    ListOptions list;
    InfoOptions info;
    OrphansOptions orphans;
    TrimOptions trim;
    CompactCommandOptions compact;
    ConfigOptions config;
    CompletionOptions completion;
};

/// Builds the complete command tree on `app`.
///
/// Shared between `run` and `completion` on purpose. The generated completion
/// scripts are read *out of* this tree, so "the completions cannot drift from
/// the flags" is a property of the code rather than a promise in a comment: a
/// new subcommand or flag appears in both or in neither.
void add_all_commands(CLI::App& app, CommandOptions& options);

}  // namespace wsldisk::cli
