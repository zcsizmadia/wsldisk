#include "info_command.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <ostream>

#include "app.h"
#include "logger.h"
#include "lookup.h"
#include "model/size.h"
#include "model/text.h"
#include "render.h"

namespace wsldisk::cli {
namespace {

/// The three flags wslapi.h documents. Bit 3 is set on every distribution spike
/// #4 measured but is not in any published header, so it is reported by number
/// rather than given a name this project invented.
[[nodiscard]] std::string decode_flags(std::uint32_t flags) {
    std::vector<std::string> names;
    if ((flags & 0x1) != 0) {
        names.emplace_back("interop");
    }
    if ((flags & 0x2) != 0) {
        names.emplace_back("append-nt-path");
    }
    if ((flags & 0x4) != 0) {
        names.emplace_back("drive-mounting");
    }
    if (const std::uint32_t rest = flags & ~0x7U; rest != 0) {
        names.push_back(std::format("undocumented(0x{:x})", rest));
    }
    if (names.empty()) {
        return "none";
    }

    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

/// A tri-state as text: yes, no, or unknown. Three states rather than two,
/// because "we could not measure it" is not "no".
/// The state column: running, stopped, or unknown -- and unknown is not
/// stopped, because they lead to different next steps.
[[nodiscard]] std::string running_state(const std::optional<bool>& running) {
    if (!running.has_value()) {
        return "-";
    }
    return *running ? "running" : "stopped";
}

[[nodiscard]] std::string yes_no(const std::optional<bool>& value) {
    if (!value.has_value()) {
        return "-";
    }
    return *value ? "yes" : "no";
}

}  // namespace

Result<ListRow> gather_one(const Services& services, const InfoOptions& options, ILogger& logger) {
    const ListOptions list_options{.probe = options.probe};
    auto rows = gather(services, list_options, logger);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }

    const auto found = std::ranges::find_if(
        *rows, [&options](const ListRow& row) { return row.distro.find_matches(options.name); });
    if (found != rows->end()) {
        return *found;
    }

    std::vector<std::string> registered;
    registered.reserve(rows->size());
    for (const ListRow& row : *rows) {
        registered.push_back(row.distro.name);
    }
    return std::unexpected(distro_not_found(options.name, registered));
}

void render_details(const ListRow& row, std::ostream& out) {
    Details details;
    details.add("name", row.distro.name);
    details.add("guid", row.distro.guid);
    details.add("registry key", model::to_utf8(model::registry_key_for(row.distro)));
    details.add("wsl version", std::to_string(row.distro.version));
    details.add("default", row.distro.is_default ? "yes" : "no");
    details.add("state", running_state(row.running));

    // The stored form, prefix and all: this is the one place a user can see
    // that their BasePath is the extended-length kind, which is why `relink`
    // has to preserve it.
    details.add("base path", model::to_utf8(row.distro.base_path));
    details.add("vhd file name", row.distro.vhd_file_name.empty() ? "- (absent; defaults to ext4.vhdx)"
                                                                  : model::to_utf8(row.distro.vhd_file_name));
    details.add("disk path", row.distro.vhdx_path.string());

    details.add("modern layout", row.distro.modern ? "yes" : "no");
    details.add("flavor", row.distro.flavor.empty() ? "-" : row.distro.flavor);
    details.add("os version", row.distro.os_version.empty() ? "-" : row.distro.os_version);
    details.add("default uid", std::to_string(row.distro.default_uid));
    details.add("flags", std::format("{} ({})", row.distro.flags, decode_flags(row.distro.flags)));

    details.add("virtual size", row.info.virtual_size);
    details.add("file size", row.info.file_size);
    details.add("size on disk", row.info.size_on_disk);
    details.add("allocated", row.info.allocated_bytes);
    details.add("sparse", yes_no(row.info.is_sparse));
    details.add("block size", row.info.block_size.has_value() ? format_size(*row.info.block_size) : "-");
    details.add("sector size",
                row.info.sector_size.has_value() ? std::to_string(*row.info.sector_size) : "-");
    if (!row.info.parent_path.empty()) {
        details.add("parent disk", model::to_utf8(row.info.parent_path));
    }

    details.add("guest used", row.info.guest_used);
    details.add("guest free", row.info.guest_free);
    details.add("reclaimable", row.info.reclaimable());

    details.write(out);

    for (const std::string& note : row.info.notes) {
        out << "note: " << note << '\n';
    }
}

void render_details_json(const ListRow& row, std::ostream& out) {
    // Built from the shared line so `info --json` and `list --json` describe the
    // same distribution the same way, then extended with what only `info` knows.
    auto object = nlohmann::json::parse(to_json_line(row.distro, row.info));
    object["registry_key"] = model::to_utf8(model::registry_key_for(row.distro));
    object["base_path"] = model::to_utf8(row.distro.base_path);
    object["modern"] = row.distro.modern;
    object["default_uid"] = row.distro.default_uid;
    object["flags"] = row.distro.flags;
    object["flags_decoded"] = decode_flags(row.distro.flags);

    if (!row.distro.vhd_file_name.empty()) {
        object["vhd_file_name"] = model::to_utf8(row.distro.vhd_file_name);
    }
    if (row.running.has_value()) {
        object["running"] = *row.running;
    }
    if (row.info.block_size.has_value()) {
        object["block_size"] = *row.info.block_size;
    }
    if (row.info.sector_size.has_value()) {
        object["sector_size"] = *row.info.sector_size;
    }
    if (!row.info.parent_path.empty()) {
        object["parent_path"] = model::to_utf8(row.info.parent_path);
    }
    out << object.dump() << '\n';
}

int run_info(const Services& services, const InfoOptions& options, const GlobalOptions& global,
             ILogger& logger, std::ostream& out, std::ostream& err) {
    const auto row = gather_one(services, options, logger);
    if (!row.has_value()) {
        return report(row.error(), global, out, err);
    }
    if (global.json) {
        render_details_json(*row, out);
    } else {
        render_details(*row, out);
    }
    return exit_code_success;
}

void add_info_command(CLI::App& app, GlobalOptions& global, InfoOptions& options) {
    CLI::App* info = app.add_subcommand("info", "Show everything known about one distribution");
    info->add_option("distro", options.name, "The distribution to describe")->required();
    add_global_options(*info, global);
    info->add_flag("--probe", options.probe, "Start the distribution if stopped, to read guest usage");
}

}  // namespace wsldisk::cli
