#include "usage_command.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "app.h"
#include "logger.h"
#include "lookup.h"
#include "model/distro.h"
#include "model/size.h"
#include "ops/usage.h"
#include "render.h"

namespace wsldisk::cli {
namespace {

void render_usage(const ops::UsageReport& report, std::ostream& out) {
    if (report.entries.empty()) {
        out << report.distribution << ": nothing in the cache catalogue is using space\n";
    } else {
        Table table{{"SIZE", "WHAT", "CLEARABLE", "PATH"}};
        for (const ops::UsageEntry& entry : report.entries) {
            table.add_row({Cell{.bytes = entry.bytes}, Cell{.text = entry.label}, Cell{.flag = entry.safe},
                           Cell{.text = entry.path}});
        }
        table.render(out);
        out << '\n';
    }

    // The comparison the user actually wants: what was found against what the
    // guest says is in use. Nested entries are counted once, which is why this
    // is a separate number from the column above.
    out << format_size(report.counted) << " found, of " << format_size(report.guest_used)
        << " the guest reports in use\n";

    // `clearable` is the honest word: `false` means wsldisk cannot tell whether
    // the contents matter, not that removing them would break something.
    bool any_unclear = false;
    for (const ops::UsageEntry& entry : report.entries) {
        if (!entry.safe) {
            any_unclear = true;
        }
    }
    if (any_unclear) {
        out << "rows marked no hold things wsldisk cannot judge -- images you built, logs "
               "something may be reading\n";
    }
    for (const ops::UsageEntry& entry : report.entries) {
        if (entry.contains_others) {
            out << entry.path << " contains other rows above; its size is not added twice\n";
        }
    }
    if (!report.directories.empty()) {
        out << '\n';
        Table table{{"SIZE", "DIRECTORY", "OF WHICH KNOWN", "LARGEST KNOWN"}};
        for (const ops::UsageDirectory& directory : report.directories) {
            table.add_row({Cell{.bytes = directory.bytes}, Cell{.text = directory.path},
                           Cell{.bytes = directory.attributed_bytes > 0
                                             ? std::optional<std::uint64_t>{directory.attributed_bytes}
                                             : std::optional<std::uint64_t>{}},
                           Cell{.text = directory.attributed_to.empty()
                                            ? std::optional<std::string>{}
                                            : std::optional<std::string>{directory.attributed_to}}});
        }
        table.render(out);
        out << '\n';
        // The two tables overlap on purpose, and a reader who added them up would
        // be double-counting. The "of which known" column is what makes that
        // legible: the rest of each row is space nothing above accounted for.
        out << "directories are the whole guest, not extra findings. `of which known` is how "
               "much of each row the table above already showed\n";
    }

    for (const std::string& note : report.notes) {
        out << "note: " << note << '\n';
    }
}

void render_usage_json(const ops::UsageReport& report, std::ostream& out) {
    nlohmann::json object;
    object["distribution"] = report.distribution;
    object["guest_used"] = report.guest_used;
    object["guest_free"] = report.guest_free;
    object["counted"] = report.counted;

    nlohmann::json entries = nlohmann::json::array();
    for (const ops::UsageEntry& entry : report.entries) {
        nlohmann::json item;
        item["path"] = entry.path;
        item["label"] = entry.label;
        item["bytes"] = entry.bytes;
        item["safe"] = entry.safe;
        item["contains_others"] = entry.contains_others;
        if (!entry.note.empty()) {
            item["note"] = entry.note;
        }
        entries.push_back(std::move(item));
    }
    object["entries"] = std::move(entries);

    if (!report.directories.empty()) {
        nlohmann::json directories = nlohmann::json::array();
        for (const ops::UsageDirectory& directory : report.directories) {
            nlohmann::json item;
            item["path"] = directory.path;
            item["bytes"] = directory.bytes;
            item["depth"] = directory.depth;
            item["attributed_bytes"] = directory.attributed_bytes;
            if (!directory.attributed_to.empty()) {
                item["attributed_to"] = directory.attributed_to;
            }
            directories.push_back(std::move(item));
        }
        object["directories"] = std::move(directories);
    }

    if (!report.notes.empty()) {
        object["notes"] = report.notes;
    }
    out << object.dump() << '\n';
}

}  // namespace

int run_usage(const Services& services, const UsageCommandOptions& options, const GlobalOptions& global,
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

    // `--dry-run` on a command that changes nothing is not an error, but it is
    // worth answering honestly rather than pretending something was withheld.
    if (global.dry_run) {
        if (global.json) {
            nlohmann::json object;
            object["distribution"] = options.name;
            object["dry_run"] = true;
            object["note"] = "usage only reads; there is nothing it would have changed";
            out << object.dump() << '\n';
        } else {
            out << "usage only reads; there is nothing it would have changed\n";
        }
        return exit_code_success;
    }

    ops::UsageOperation operation{
        *services.host, *distro,
        ops::UsageOptions{.top = options.top, .by_directory = options.by_directory, .depth = options.depth}};

    // `du` over a large filesystem takes minutes. Under `--json` it goes to
    // stderr with everything else, so stdout stays parseable.
    const auto note = [&global, &err](std::string_view path) {
        if (global.verbose || global.json) {
            err << "  measuring " << path << '\n';
        }
    };

    const auto measured = operation.measure(note);
    if (!measured.has_value()) {
        return report(measured.error(), global, out, err);
    }

    if (global.json) {
        render_usage_json(*measured, out);
    } else {
        render_usage(*measured, out);
    }
    return exit_code_success;
}

void add_usage_command(CLI::App& app, GlobalOptions& global, UsageCommandOptions& options) {
    CLI::App* usage = app.add_subcommand("usage", "Show where the space inside a distribution went");
    usage->add_option("distro", options.name, "The distribution to look inside")->required();
    usage->add_option("--top", options.top, "Show only the largest N entries");
    usage->add_flag("--by-directory", options.by_directory, "Also break the whole guest down by directory");
    // Only meaningful with the breakdown, and `--depth 3` on its own is an
    // instruction that would otherwise be silently ignored.
    usage->add_option("--depth", options.depth, "How deep the directory breakdown goes")
        ->needs("--by-directory")
        ->check(CLI::Range(std::size_t{1}, std::size_t{8}));
    add_global_options(*usage, global);
}

}  // namespace wsldisk::cli
