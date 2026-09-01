#include "move_command.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

#include "app.h"
#include "logger.h"
#include "lookup.h"
#include "model/distro.h"
#include "model/size.h"
#include "model/text.h"
#include "ops/move.h"
#include "ops/runner.h"
#include "progress.h"
#include "render.h"

namespace wsldisk::cli {
namespace {

void render_move(const MoveOptions& options, const ops::MoveOperation& operation, std::ostream& out) {
    out << options.name << " now lives at " << operation.target().string() << " ("
        << format_size(operation.size_on_disk()) << ")\n";
}

void render_move_json(const MoveOptions& options, const ops::MoveOperation& operation, std::ostream& out) {
    nlohmann::json object;
    object["distribution"] = options.name;
    object["vhdx_path"] = operation.target().string();
    // What was actually written, which is not always the destination spelled the
    // obvious way: an entry that used `\\?\` keeps using it.
    object["base_path"] = model::to_utf8(operation.intended_base_path());
    object["moved"] = true;
    // Whether it was a rename or a copy, because one takes no time and the other
    // takes as long as the disk is big -- worth being able to tell apart.
    object["renamed"] = operation.was_renamed();
    object["same_volume"] = operation.is_same_volume();
    object["kept_source"] = options.keep_source;
    object["size_on_disk"] = operation.size_on_disk();
    out << object.dump() << '\n';
}

}  // namespace

int run_move(const Services& services, const MoveOptions& options, const GlobalOptions& global,
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
        std::vector<std::string> registered;
        registered.reserve(distros->distros.size());
        for (const model::Distro& known : distros->distros) {
            registered.push_back(known.name);
        }
        return report(distro_not_found(options.name, registered), global, out, err);
    }

    ops::MoveOperation operation{*services.registry,
                                 *services.filesystem,
                                 *services.host,
                                 *distro,
                                 std::filesystem::path{options.destination},
                                 ops::MoveOptions{.keep_source = options.keep_source}};

    ConsoleSink progress{out};
    ops::NullSink quiet;
    ops::ProgressSink& sink = global.json ? static_cast<ops::ProgressSink&>(quiet) : progress;

    const auto outcome = ops::run(operation, sink, ops::RunOptions{.dry_run = global.dry_run});
    if (!outcome.has_value()) {
        return report(outcome.error(), global, out, err);
    }

    if (global.dry_run) {
        render_dry_run(outcome->plan, "distribution", options.name, global.json, out);
        return exit_code_success;
    }

    if (global.json) {
        render_move_json(options, operation, out);
    } else {
        render_move(options, operation, out);
    }
    return exit_code_success;
}

void add_move_command(CLI::App& app, GlobalOptions& global, MoveOptions& options) {
    CLI::App* move = app.add_subcommand("move", "Move a distribution's virtual disk to another directory");
    move->add_option("distro", options.name, "The distribution to move")->required();
    move->add_option("destination", options.destination, "The directory to move its disk into")->required();
    move->add_flag("--keep-source", options.keep_source, "Leave the original file where it was");
    add_global_options(*move, global);
}

}  // namespace wsldisk::cli
