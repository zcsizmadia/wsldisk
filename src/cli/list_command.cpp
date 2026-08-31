#include "list_command.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <ostream>
#include <string>

#include "app.h"
#include "logger.h"
#include "model/text.h"
#include "render.h"

namespace wsldisk::cli {
namespace {

/// A WSL1 distribution has no VHDX at all, so measuring one would be asking the
/// filesystem about a path that means nothing. It is still listed -- this is the
/// one command that shows them (D8) -- with its disk columns blank.
[[nodiscard]] bool has_a_disk(const model::Distro& distro) {
    return distro.is_wsl2();
}

}  // namespace

Result<std::vector<ListRow>> gather(const Services& services, const ListOptions& options, ILogger& logger,
                                    std::string_view probe_only) {
    const auto distros = model::enumerate(*services.registry);
    if (!distros.has_value()) {
        return std::unexpected(distros.error());
    }

    // Reported rather than swallowed: a key that could not be read is the thing
    // `orphans` is for, and the user should hear about it even from `list`.
    for (const std::string& warning : distros->warnings) {
        logger.warn(warning);
    }

    // Asked once for the whole listing rather than once per distribution: it
    // spawns a process, and the answer is the same for every row.
    const auto running = services.host->running();
    if (!running.has_value()) {
        logger.warn(running.error().to_string());
    }

    std::vector<ListRow> rows;
    rows.reserve(distros->distros.size());
    for (const model::Distro& distro : distros->distros) {
        ListRow row{.distro = distro};

        if (running.has_value()) {
            row.running = std::ranges::find(*running, distro.name) != running->end();
        }

        if (has_a_disk(distro)) {
            // Per row, because probing boots a stopped distribution and a caller
            // that asked about one must not start the others.
            const model::ProbeOptions probe{
                .probe_guest = options.probe && (probe_only.empty() || distro.find_matches(probe_only))};
            row.info = model::measure(distro, *services.filesystem, *services.disks, *services.host, probe);
            // Notes explain why a column is blank. Verbose rather than a
            // warning: on a machine in normal use every running distribution
            // produces one, and shouting about the expected case trains people
            // to ignore the output.
            for (const std::string& note : row.info.notes) {
                logger.verbose(note);
            }
        }

        rows.push_back(std::move(row));
    }
    return rows;
}

void render_table(const std::vector<ListRow>& rows, std::ostream& out) {
    Table table{{"NAME", "VER", "STATE", "SIZE ON DISK", "GUEST USED", "RECLAIMABLE", "PATH"}};

    for (const ListRow& row : rows) {
        // The default distribution is what `wsl.exe` acts on when no name is
        // given, so it is worth a marker rather than a separate column.
        const std::string name = row.distro.is_default ? row.distro.name + " *" : row.distro.name;

        std::optional<std::string> state;
        if (row.running.has_value()) {
            state = *row.running ? "running" : "stopped";
        }

        table.add_row({Cell{.text = name}, Cell{.text = std::to_string(row.distro.version)},
                       Cell{.text = state}, Cell{.bytes = row.info.size_on_disk},
                       Cell{.bytes = row.info.guest_used}, Cell{.bytes = row.info.reclaimable()},
                       Cell{.text = model::path_to_utf8(row.distro.vhdx_path)}});
    }
    table.render(out);
}

void render_json(const std::vector<ListRow>& rows, std::ostream& out) {
    // One object per line rather than one array: a caller can process the first
    // row without waiting for the last, and `wsldisk list --json | head -1` does
    // something sensible.
    for (const ListRow& row : rows) {
        out << to_json_line(row.distro, row.info) << '\n';
    }
}

int run_list(const Services& services, const ListOptions& options, const GlobalOptions& global,
             ILogger& logger, std::ostream& out, std::ostream& err) {
    const auto rows = gather(services, options, logger);
    if (!rows.has_value()) {
        return report(rows.error(), global, out, err);
    }
    if (global.json) {
        render_json(*rows, out);
    } else {
        render_table(*rows, out);
    }
    return exit_code_success;
}

void add_list_command(CLI::App& app, GlobalOptions& global, ListOptions& options) {
    CLI::App* list = app.add_subcommand("list", "List every registered distribution and its disk");
    add_global_options(*list, global);
    list->add_flag("--probe", options.probe,
                   "Start stopped distributions to read guest usage (off by default)");
}

}  // namespace wsldisk::cli
