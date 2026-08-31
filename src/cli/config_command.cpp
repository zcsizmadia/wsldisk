#include "config_command.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <ostream>
#include <vector>

#include "app.h"
#include "logger.h"
#include "model/config.h"
#include "platform/editor.h"
#include "render.h"

namespace wsldisk::cli {
namespace {

/// Prints every setting, and the read-only `.wslconfig` keys under it.
void render_settings(const model::Config& config, const model::WslConfig& wsl,
                     const std::filesystem::path& path, std::ostream& out) {
    out << path.string() << "\n\n";

    Details settings;
    for (const std::string& key : model::config_keys()) {
        settings.add(key, model::get_config_value(config, key).value_or(""));
    }
    settings.write(out);

    if (wsl.empty()) {
        return;
    }

    // Shown, never written. These belong to WSL; wsldisk only reads them so the
    // user does not have to hold two files in their head at once.
    out << "\nfrom .wslconfig (read-only):\n";
    Details from_wsl;
    if (wsl.default_vhd_size.has_value()) {
        from_wsl.add("wsl2.defaultVhdSize", *wsl.default_vhd_size);
    }
    if (wsl.vhd_size.has_value()) {
        from_wsl.add("wsl2.vhdSize", *wsl.vhd_size);
    }
    if (wsl.swap_file.has_value()) {
        from_wsl.add("wsl2.swapFile", *wsl.swap_file);
    }
    from_wsl.write(out);
}

void render_settings_json(const model::Config& config, const model::WslConfig& wsl,
                          const std::filesystem::path& path, std::ostream& out) {
    nlohmann::json object;
    object["path"] = path.string();
    for (const std::string& key : model::config_keys()) {
        object["settings"][key] = model::get_config_value(config, key).value_or("");
    }
    if (wsl.default_vhd_size.has_value()) {
        object["wslconfig"]["defaultVhdSize"] = *wsl.default_vhd_size;
    }
    if (wsl.vhd_size.has_value()) {
        object["wslconfig"]["vhdSize"] = *wsl.vhd_size;
    }
    if (wsl.swap_file.has_value()) {
        object["wslconfig"]["swapFile"] = *wsl.swap_file;
    }
    out << object.dump() << '\n';
}

/// Reads `.wslconfig`, treating anything unreadable as absent.
///
/// A missing or unreadable `.wslconfig` is normal -- most machines have none --
/// and it is not this command's subject, so it never turns into a failure.
[[nodiscard]] model::WslConfig read_wslconfig(const IFileSystem& filesystem) {
    const auto path = model::wslconfig_path(filesystem);
    if (!path.has_value() || !filesystem.exists(*path)) {
        return {};
    }
    const auto text = filesystem.read_text_file(*path);
    if (!text.has_value()) {
        return {};
    }
    return model::parse_wslconfig(*text);
}

/// Writes the config back, creating its directory the first time.
[[nodiscard]] Status save(IFileSystem& filesystem, const std::filesystem::path& path,
                          const model::Config& config) {
    if (const Status made = filesystem.create_directories(path.parent_path()); !made.has_value()) {
        return made;
    }
    return filesystem.write_text_file(path, model::render_config(config));
}

[[nodiscard]] int run_get(const model::Config& config, const ConfigOptions& options,
                          const GlobalOptions& global, std::ostream& out, std::ostream& err) {
    if (options.key.empty()) {
        for (const std::string& key : model::config_keys()) {
            out << key << " = " << model::get_config_value(config, key).value_or("") << '\n';
        }
        return exit_code_success;
    }

    const auto value = model::get_config_value(config, options.key);
    if (!value.has_value()) {
        // The same error `set` gives, from the same list of keys.
        model::Config discard = config;
        const Status unknown = model::set_config_value(discard, options.key, "");
        return report(unknown.error(), global, out, err);
    }
    out << *value << '\n';
    return exit_code_success;
}

[[nodiscard]] int run_set(const Services& services, model::Config config, const ConfigOptions& options,
                          const GlobalOptions& global, const std::filesystem::path& path, std::ostream& out,
                          std::ostream& err) {
    // Through the parser, not the file: a value that will not round-trip is
    // refused before anything is written.
    if (const Status set = model::set_config_value(config, options.key, options.value); !set.has_value()) {
        return report(set.error(), global, out, err);
    }

    if (global.dry_run) {
        out << "--dry-run: " << path.string() << " was not changed. It would have set " << options.key
            << " = " << model::get_config_value(config, options.key).value_or("") << '\n';
        return exit_code_success;
    }

    if (const Status written = save(*services.filesystem, path, config); !written.has_value()) {
        return report(written.error(), global, out, err);
    }
    out << options.key << " = " << model::get_config_value(config, options.key).value_or("") << '\n';
    return exit_code_success;
}

[[nodiscard]] int run_edit(const Services& services, const model::Config& config, const GlobalOptions& global,
                           const LaunchEditor& launch, const std::filesystem::path& path, std::ostream& out,
                           std::ostream& err) {
    // Written first when it is not there: an editor opened on a file that does
    // not exist leaves the user to write the schema from memory.
    if (!services.filesystem->exists(path)) {
        if (const Status written = save(*services.filesystem, path, config); !written.has_value()) {
            return report(written.error(), global, out, err);
        }
    }

    if (global.dry_run) {
        out << "--dry-run: would open " << path.string() << '\n';
        return exit_code_success;
    }
    if (const Status opened = launch(path); !opened.has_value()) {
        return report(opened.error(), global, out, err);
    }
    return exit_code_success;
}

}  // namespace

Status open_in_editor(const IFileSystem& filesystem, const std::filesystem::path& file) {
    return platform::launch_editor(editor_command(filesystem), file);
}

std::string editor_command(const IFileSystem& filesystem) {
    const auto editor = filesystem.expand_environment(LR"(%EDITOR%)");
    // Unset variables expand to themselves, so `%EDITOR%` coming back unchanged
    // means there is none.
    if (editor.has_value() && editor->wstring() != LR"(%EDITOR%)" && !editor->empty()) {
        return editor->string();
    }
    return "notepad";
}

int run_config(const Services& services, const ConfigOptions& options, const GlobalOptions& global,
               ILogger& logger, const LaunchEditor& launch, std::ostream& out, std::ostream& err) {
    const auto path = model::config_path(*services.filesystem);
    if (!path.has_value()) {
        return report(path.error(), global, out, err);
    }

    if (options.action == "path") {
        // Printed without reading the file: `config path` has to work even when
        // the file is the thing that is broken.
        out << path->string() << '\n';
        return exit_code_success;
    }

    const auto config = model::load_config(*services.filesystem, *path);
    if (!config.has_value()) {
        return report(config.error(), global, out, err);
    }
    logger.verbose("read " + path->string());

    if (options.action == "get") {
        return run_get(*config, options, global, out, err);
    }
    if (options.action == "set") {
        return run_set(services, *config, options, global, *path, out, err);
    }
    if (options.action == "edit") {
        return run_edit(services, *config, global, launch, *path, out, err);
    }

    const model::WslConfig wsl = read_wslconfig(*services.filesystem);
    if (global.json) {
        render_settings_json(*config, wsl, *path, out);
    } else {
        render_settings(*config, wsl, *path, out);
    }
    return exit_code_success;
}

void add_config_command(CLI::App& app, GlobalOptions& global, ConfigOptions& options) {
    CLI::App* config = app.add_subcommand("config", "Show or change wsldisk's settings");
    add_global_options(*config, global);

    CLI::App* path = config->add_subcommand("path", "Print where the config file lives");
    add_global_options(*path, global);
    path->parse_complete_callback([&options]() { options.action = "path"; });

    CLI::App* get = config->add_subcommand("get", "Print one setting, or all of them");
    add_global_options(*get, global);
    get->add_option("key", options.key, "The setting to print")->option_text("KEY");
    get->parse_complete_callback([&options]() { options.action = "get"; });

    CLI::App* set = config->add_subcommand("set", "Change one setting");
    add_global_options(*set, global);
    set->add_option("key", options.key, "The setting to change")->option_text("KEY")->required();
    set->add_option("value", options.value, "Its new value")->option_text("VALUE")->required();
    set->parse_complete_callback([&options]() { options.action = "set"; });

    CLI::App* edit = config->add_subcommand("edit", "Open the config file in $EDITOR");
    add_global_options(*edit, global);
    edit->parse_complete_callback([&options]() { options.action = "edit"; });

    // At most one verb: bare `wsldisk config` shows everything.
    config->require_subcommand(0, 1);
}

}  // namespace wsldisk::cli
