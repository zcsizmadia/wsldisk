#include "orphans_command.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <istream>
#include <ostream>

#include "app.h"
#include "logger.h"
#include "model/size.h"
#include "model/text.h"
#include "relink_command.h"
#include "render.h"

namespace wsldisk::cli {
namespace {

[[nodiscard]] std::uint64_t total_size(const std::vector<model::Orphan>& orphans) {
    std::uint64_t total = 0;
    for (const model::Orphan& orphan : orphans) {
        total += orphan.size_on_disk.value_or(0);
    }
    return total;
}

void render_orphan_table(const std::vector<model::Orphan>& orphans, std::ostream& out) {
    Table table{{"SIZE ON DISK", "PATH"}};
    for (const model::Orphan& orphan : orphans) {
        table.add_row({Cell{.bytes = orphan.size_on_disk}, Cell{.text = model::path_to_utf8(orphan.path)}});
    }
    table.render(out);
}

void render_orphan_json(const std::vector<model::Orphan>& orphans, std::ostream& out) {
    for (const model::Orphan& orphan : orphans) {
        nlohmann::json object;
        object["path"] = model::path_to_utf8(orphan.path);
        if (orphan.size_on_disk.has_value()) {
            object["size_on_disk"] = *orphan.size_on_disk;
        }
        out << object.dump() << '\n';
    }
}

}  // namespace

bool ask(std::istream& in, std::ostream& out, std::string_view question) {
    out << question << " [y/N] " << std::flush;

    std::string answer;
    if (!std::getline(in, answer)) {
        // Nothing to answer with -- a piped command, or a closed console. That
        // is not consent.
        return false;
    }
    std::ranges::transform(answer, answer.begin(),
                           [](unsigned char character) { return std::tolower(character); });
    return answer == "y" || answer == "yes";
}

Confirm console_confirm(std::istream& in, std::ostream& out) {
    return [&in, &out](std::string_view question) { return ask(in, out, question); };
}

Result<std::vector<model::Orphan>> scan_orphans(const Services& services, const OrphansOptions& options,
                                                ILogger& logger) {
    const auto distros = model::enumerate(*services.registry);
    if (!distros.has_value()) {
        return std::unexpected(distros.error());
    }
    for (const std::string& warning : distros->warnings) {
        logger.warn(warning);
    }

    auto patterns = model::default_scan_patterns(*services.filesystem);
    if (!patterns.has_value()) {
        return std::unexpected(patterns.error());
    }
    // `scan.dirs` from the file, then `--scan` from the command line. Both add
    // to the built-in roots rather than replacing them: a configured directory
    // is somewhere else the user also keeps disks, not the only place.
    for (const std::string& directory : services.config.scan_dirs) {
        patterns->emplace_back(directory);
    }
    for (const std::string& directory : options.scan_dirs) {
        patterns->emplace_back(directory);
    }

    std::vector<std::string> warnings;
    auto orphans = model::find_orphans(*services.filesystem, *distros, *patterns, warnings);
    for (const std::string& warning : warnings) {
        // Verbose rather than a warning: a scan pattern naming a directory this
        // machine does not have is the normal case, not a problem.
        logger.verbose(warning);
    }
    return orphans;
}

int delete_orphans(const Services& services, const std::vector<model::Orphan>& orphans,
                   const GlobalOptions& global, const Confirm& confirm, std::ostream& out,
                   std::ostream& err) {
    if (orphans.empty()) {
        out << "nothing to delete\n";
        return exit_code_success;
    }

    render_orphan_table(orphans, out);
    out << '\n' << format_size(total_size(orphans)) << " would be freed\n";

    // Said before the prompt, because it is the thing most likely to change the
    // answer. Docker Desktop keeps a docker_data.vhdx that no distribution
    // claims and that holds every volume the user has; "no registry entry
    // points at it" is not the same as "nothing needs it".
    out << "\nnot every disk here is unused: software other than WSL keeps virtual disks\n"
           "that no distribution claims. Check what each one is before deleting it.\n";

    if (global.dry_run) {
        out << "--dry-run: nothing was deleted\n";
        return exit_code_success;
    }

    if (!global.assume_yes && !confirm(std::format("delete {} file(s)?", orphans.size()))) {
        out << "nothing was deleted\n";
        return exit_code_success;
    }

    // A file that will not delete is reported and the rest are still tried:
    // stopping at the first failure leaves the user to work out how far it got.
    int failures = 0;
    for (const model::Orphan& orphan : orphans) {
        // A file something else has open is not one to delete, whatever the
        // registry says about it.
        const auto locked = services.filesystem->is_locked(orphan.path);
        if (!locked.has_value()) {
            err << "error: " << locked.error().to_string() << '\n';
            ++failures;
            continue;
        }
        if (*locked) {
            err << "error: " << model::path_to_utf8(orphan.path) << " is in use by another process\n";
            ++failures;
            continue;
        }

        if (const Status removed = services.filesystem->remove(orphan.path); !removed.has_value()) {
            err << "error: " << removed.error().to_string() << '\n';
            ++failures;
            continue;
        }
        out << "deleted " << model::path_to_utf8(orphan.path) << '\n';
    }

    if (failures > 0) {
        return report(Error{ErrorCode::DistroBusy,
                            std::format("{} of {} file(s) could not be deleted", failures, orphans.size()),
                            "a disk held open by something else is not an orphan; close whatever "
                            "is using it -- `wsl --shutdown` for WSL, or quit Docker Desktop -- "
                            "and try again"},
                      global, out, err);
    }
    return exit_code_success;
}

int run_orphans(const Services& services, const OrphansOptions& options, const GlobalOptions& global,
                ILogger& logger, const Confirm& confirm, std::ostream& out, std::ostream& err) {
    if (options.relinking()) {
        // The same command reached from the other side. `orphans` finds a disk
        // and asks who should own it; `relink` knows the owner and the path.
        return run_relink(services, RelinkOptions{.name = options.relink_distro, .path = options.relink_path},
                          global, logger, out, err);
    }

    const auto orphans = scan_orphans(services, options, logger);
    if (!orphans.has_value()) {
        return report(orphans.error(), global, out, err);
    }

    if (options.remove) {
        return delete_orphans(services, *orphans, global, confirm, out, err);
    }

    if (global.json) {
        render_orphan_json(*orphans, out);
        return exit_code_success;
    }

    if (orphans->empty()) {
        out << "no orphaned disks found\n";
        return exit_code_success;
    }
    render_orphan_table(*orphans, out);
    out << '\n'
        << format_size(total_size(*orphans)) << " in " << orphans->size()
        << " file(s) that no distribution claims\n";
    return exit_code_success;
}

void add_orphans_command(CLI::App& app, GlobalOptions& global, OrphansOptions& options) {
    CLI::App* orphans = app.add_subcommand("orphans", "Find virtual disks that no distribution claims");
    add_global_options(*orphans, global);

    orphans->add_option("--scan", options.scan_dirs, "Extra directories to search")->option_text("DIR ...");
    orphans->add_flag("--delete", options.remove, "Delete what was found, after confirming");
    orphans->add_option("--relink", options.relink_distro, "Point this distribution at a disk")
        ->option_text("DISTRO")
        ->needs(
            orphans->add_option("--to", options.relink_path, "The disk to point it at")->option_text("PATH"));
}

}  // namespace wsldisk::cli
