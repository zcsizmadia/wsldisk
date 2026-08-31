#include "trim_command.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <ostream>

#include "app.h"
#include "logger.h"
#include "lookup.h"
#include "model/size.h"
#include "ops/runner.h"
#include "ops/trim.h"
#include "preflight.h"
#include "progress.h"
#include "render.h"

namespace wsldisk::cli {
namespace {

void render_trim(const model::Distro& distro, const ops::TrimOperation& operation, std::ostream& out) {
    out << distro.name << ": trimmed.";
    if (operation.trimmed_bytes().has_value()) {
        out << " fstrim reported " << format_size(*operation.trimmed_bytes()) << ".";
    } else {
        // The ordinary case when `-v` was refused. Saying so beats printing a
        // zero, which would read as "nothing was freed".
        out << " fstrim did not say how much.";
    }
    out << '\n';

    // The caveat belongs to the figure: with no figure there is nothing to be
    // misread.
    if (operation.trimmed_bytes().has_value()) {
        out << ops::trimmed_bytes_are_misleading << '\n';
    }
    out << "run `wsldisk compact " << distro.name << "` to shrink the file itself\n";
}

void render_trim_json(const model::Distro& distro, const ops::TrimOperation& operation, std::ostream& out) {
    nlohmann::json object;
    object["distribution"] = distro.name;
    object["trimmed"] = true;
    if (operation.trimmed_bytes().has_value()) {
        // Named for what it is. `bytes_freed` would be read as space reclaimed,
        // and it is not: see ops::trimmed_bytes_are_misleading.
        object["bytes_offered"] = *operation.trimmed_bytes();
    }
    object["note"] = std::string{ops::trimmed_bytes_are_misleading};
    out << object.dump() << '\n';
}

}  // namespace

int run_trim(const Services& services, const TrimOptions& options, const GlobalOptions& global,
             ILogger& logger, std::ostream& out, std::ostream& err) {
    const auto distro = find_distro(*services.registry, options.name, logger);
    if (!distro.has_value()) {
        return report(distro.error(), global, out, err);
    }
    if (const Status supported = require_wsl2(*distro); !supported.has_value()) {
        return report(supported.error(), global, out, err);
    }

    ops::TrimOperation operation{*services.host, *distro};

    ConsoleSink progress{out};
    ops::NullSink quiet;
    ops::ProgressSink& sink = global.json ? static_cast<ops::ProgressSink&>(quiet) : progress;

    const auto outcome = ops::run(operation, sink, ops::RunOptions{.dry_run = global.dry_run});
    if (!outcome.has_value()) {
        return report(outcome.error(), global, out, err);
    }

    if (global.dry_run) {
        render_dry_run(outcome->plan, "distribution", distro->name, global.json, out);
        return exit_code_success;
    }

    if (global.json) {
        render_trim_json(*distro, operation, out);
    } else {
        render_trim(*distro, operation, out);
    }
    return exit_code_success;
}

void add_trim_command(CLI::App& app, GlobalOptions& global, TrimOptions& options) {
    CLI::App* trim =
        app.add_subcommand("trim", "Tell one distribution's filesystem to release unused blocks");
    trim->add_option("distro", options.name, "The distribution to trim")->required();
    add_global_options(*trim, global);
}

}  // namespace wsldisk::cli
