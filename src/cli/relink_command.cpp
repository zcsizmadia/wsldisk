#include "relink_command.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>
#include <ostream>

#include "app.h"
#include "logger.h"
#include "model/distro.h"
#include "model/text.h"
#include "ops/relink.h"
#include "ops/runner.h"
#include "progress.h"

namespace wsldisk::cli {
namespace {

// Deliberately the same single line `orphans --relink` has always printed. What
// was actually written to `BasePath` is in the JSON, where something can read
// it; putting it here would change the output of a command people already use,
// to tell them something they did not ask for.
void render_relink(const RelinkOptions& options, std::ostream& out) {
    out << options.name << " now points at " << options.path << '\n';
}

void render_relink_json(const RelinkOptions& options, const ops::RelinkOperation& operation,
                        std::ostream& out) {
    nlohmann::json object;
    object["distribution"] = options.name;
    object["vhdx_path"] = options.path;
    // The value actually written, which is not always the parent of `path`
    // spelled the obvious way: an entry that used `\\?\` keeps using it.
    object["base_path"] = model::to_utf8(operation.intended_base_path());
    object["relinked"] = true;
    out << object.dump() << '\n';
}

}  // namespace

int run_relink(const Services& services, const RelinkOptions& options, const GlobalOptions& global,
               ILogger& logger, std::ostream& out, std::ostream& err) {
    const auto distros = model::enumerate(*services.registry);
    if (!distros.has_value()) {
        return report(distros.error(), global, out, err);
    }
    for (const std::string& warning : distros->warnings) {
        logger.warn(warning);
    }

    const model::Distro* distro = distros->find(options.name);
    if (distro == nullptr) {
        return report(Error{ErrorCode::DistroNotFound, std::format("no distribution named {}", options.name),
                            "run `wsldisk list` to see what is registered"},
                      global, out, err);
    }

    ops::RelinkOperation operation{*services.registry, *services.filesystem, *services.host, *distro,
                                   std::filesystem::path{options.path}};

    ConsoleSink progress{out};
    ops::NullSink quiet;
    ops::ProgressSink& sink = global.json ? static_cast<ops::ProgressSink&>(quiet) : progress;

    const auto outcome = ops::run(operation, sink, ops::RunOptions{.dry_run = global.dry_run});
    if (!outcome.has_value()) {
        return report(outcome.error(), global, out, err);
    }

    if (global.dry_run) {
        out << "--dry-run: nothing was changed. It would have:\n";
        for (const ops::StepPlan& step : outcome->plan.steps) {
            out << "  " << step.description << '\n';
        }
        return exit_code_success;
    }

    if (global.json) {
        render_relink_json(options, operation, out);
    } else {
        render_relink(options, out);
    }
    return exit_code_success;
}

void add_relink_command(CLI::App& app, GlobalOptions& global, RelinkOptions& options) {
    CLI::App* relink =
        app.add_subcommand("relink", "Point a distribution at a virtual disk that has been moved");
    relink->add_option("distro", options.name, "The distribution to repoint")->required();
    relink->add_option("path", options.path, "The .vhdx it should use")->required();
    add_global_options(*relink, global);
}

}  // namespace wsldisk::cli
