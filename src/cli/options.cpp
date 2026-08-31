#include "options.h"

#include <CLI/CLI.hpp>

namespace wsldisk::cli {

void add_global_options(CLI::App& app, GlobalOptions& options, bool machine_readable) {
    if (machine_readable) {
        app.add_flag("--json", options.json, "Machine-readable output on stdout");
    }
    app.add_flag("-v,--verbose", options.verbose, "Explain what is happening, on stderr");
    app.add_flag("-y,--yes", options.assume_yes, "Do not prompt for confirmation");
    app.add_flag("--dry-run", options.dry_run, "Show what would happen and change nothing");
    app.add_option("--log", options.log_file, "Append a log to this file")->option_text("FILE");
}

}  // namespace wsldisk::cli
