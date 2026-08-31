#include "commands.h"

#include <CLI/CLI.hpp>

#include <string>

#include "version.h"

namespace wsldisk::cli {

void add_all_commands(CLI::App& app, CommandOptions& options) {
    app.set_version_flag("-V,--version", std::string{version_banner()});
    app.require_subcommand(0, 1);

    add_global_options(app, options.global);
    add_list_command(app, options.global, options.list);
    add_info_command(app, options.global, options.info);
    add_orphans_command(app, options.global, options.orphans);
    add_trim_command(app, options.global, options.trim);
    add_compact_command(app, options.global, options.compact);
    add_config_command(app, options.global, options.config);
    add_completion_command(app, options.global, options.completion);
}

}  // namespace wsldisk::cli
