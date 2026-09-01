#include "app.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "commands.h"
#include "compact_command.h"
#include "completion_command.h"
#include "config_command.h"
#include "errors.h"
#include "info_command.h"
#include "list_command.h"
#include "logger.h"
#include "model/config.h"
#include "model/text.h"
#include "move_command.h"
#include "options.h"
#include "orphans_command.h"
#include "platform/clock.h"
#include "platform/filesystem.h"
#include "platform/registry.h"
#include "platform/virtual_disk.h"
#include "platform/wsl_host.h"
#include "relink_command.h"
#include "render.h"
#include "trim_command.h"
#include "usage_command.h"
#include "version.h"

namespace wsldisk::cli {

int report(const Error& error, const GlobalOptions& options, std::ostream& out, std::ostream& err) {
    // In JSON mode the error goes to stdout as an object, because a script
    // reading stdout should get a parseable answer whether or not it worked.
    // Otherwise it goes to stderr, where a human expects it.
    if (options.json) {
        out << to_json_line(error) << '\n';
    } else {
        err << "error: " << to_human_line(error) << '\n';
    }
    return exit_code_for(error.code);
}

int run(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
    CLI::App app{"Compact, shrink, move, inspect and snapshot WSL2 virtual disks", "wsldisk"};

    // Built by the same function `completion` walks, so a new command or flag
    // appears in the completions and in the parser or in neither.
    CommandOptions commands;
    add_all_commands(app, commands);
    // A reference rather than a copy: CLI11 writes into `commands.global` as it
    // parses, so a copy taken here would hold the defaults.
    const GlobalOptions& options = commands.global;

    // CLI11 parses in reverse order, so hand it a reversed copy of argv-style input.
    std::vector<std::string> reversed(args.rbegin(), args.rend());

    try {
        app.parse(std::move(reversed));
    } catch (const CLI::CallForHelp&) {
        out << app.help();
        return exit_code_success;
    } catch (const CLI::CallForVersion&) {
        out << app.version() << '\n';
        return exit_code_success;
    } catch (const CLI::ParseError& e) {
        // `options.json` cannot be trusted here: parsing is what failed, so the
        // flag may never have been reached. The raw arguments are the only
        // reliable answer to "did they ask for JSON".
        //
        // docs/JSON.md says a failure is one object on stdout, and this was the
        // one failure that gave a script an exit code and an empty stream --
        // `wsldisk info --json` with no name left nothing to branch on.
        if (std::ranges::find(args, "--json") != args.end()) {
            out << to_json_line(Error{ErrorCode::Usage, e.what(),
                                      "run `wsldisk --help` to see the arguments this takes"})
                << '\n';
            return exit_code_for(ErrorCode::Usage);
        }
        err << "error: " << e.what() << "\n\n" << app.help();
        return exit_code_for(ErrorCode::Usage);
    }

    StreamLogger logger{err, options.verbose, options.log_file};

    // Nothing but the command tree, which is already built: no registry, no
    // filesystem, no WSL. `completion` has to work on a machine where none of
    // those answer.
    if (app.got_subcommand("completion")) {
        return run_completion(commands.completion, options, out, err);
    }

    if (app.got_subcommand("list") || app.got_subcommand("info") || app.got_subcommand("orphans") ||
        app.got_subcommand("trim") || app.got_subcommand("compact") || app.got_subcommand("config") ||
        app.got_subcommand("relink") || app.got_subcommand("move") || app.got_subcommand("usage")) {
        // The real implementations. Every one is an interface, which is what
        // lets the unit tests drive the same code with fakes.
        platform::Win32Registry registry;
        platform::Win32FileSystem filesystem;
        const platform::Win32VirtualDisk disks;
        const platform::WslExeHost host;
        const platform::SystemClock clock;

        // Loaded once, here, so no command can be wired up without it. The
        // decisions live in `load_configuration` rather than inline, so its
        // failure paths are reachable from a test.
        const Services services{.registry = &registry,
                                .filesystem = &filesystem,
                                .disks = &disks,
                                .host = &host,
                                .clock = &clock,
                                .config = load_configuration(filesystem, logger)};

        if (app.got_subcommand("info")) {
            return run_info(services, commands.info, options, logger, out, err);
        }
        if (app.got_subcommand("config")) {
            // `bind_front` rather than a lambda: a lambda here would be a
            // function body no test can reach without launching a real editor.
            const LaunchEditor launch = std::bind_front(&open_in_editor, std::cref(filesystem));
            return run_config(services, commands.config, options, logger, launch, out, err);
        }
        if (app.got_subcommand("compact")) {
            return run_compact(services, commands.compact, options, logger, out, err);
        }
        if (app.got_subcommand("trim")) {
            return run_trim(services, commands.trim, options, logger, out, err);
        }
        if (app.got_subcommand("relink")) {
            return run_relink(services, commands.relink, options, logger, out, err);
        }
        if (app.got_subcommand("move")) {
            return run_move(services, commands.move, options, logger, out, err);
        }
        if (app.got_subcommand("usage")) {
            return run_usage(services, commands.usage, options, logger, out, err);
        }
        if (app.got_subcommand("orphans")) {
            // The prompt reads the real console. Everything else about the
            // command is driven from interfaces, so the tests answer it
            // themselves rather than typing.
            return run_orphans(services, commands.orphans, options, logger, console_confirm(std::cin, out),
                               out, err);
        }
        return run_list(services, commands.list, options, logger, out, err);
    }

    // Every other command lands later in M1 (see ROADMAP.md).
    out << app.help();
    return exit_code_success;
}

namespace {

/// Reports a top-level failure, tolerating a stream that is itself broken.
///
/// If `err` cannot be written to there is nowhere left to report; the exit code
/// still carries the outcome, and terminating instead would be strictly worse.
void report_failure(std::ostream& err, const char* what) noexcept {
    try {
        err << "error: " << what << '\n';
    } catch (...) {  // NOLINT(bugprone-empty-catch) -- deliberate, see above
    }
}

}  // namespace

int main_entry(int argc, wchar_t** argv, std::ostream& out, std::ostream& err) noexcept {
    try {
        // A span rather than pointer arithmetic on argv, and argc is not assumed
        // to be at least one.
        const std::span<wchar_t* const> raw_arguments{argv, static_cast<std::size_t>(argc)};
        const std::size_t skip = raw_arguments.empty() ? 0 : 1;

        // Wide, and converted here through the same codec the registry goes
        // through. Narrow `main` hands over arguments already flattened to the
        // active code page, so a distribution named with any character the ACP
        // cannot represent could never match: `wsldisk info Ubuntu` with a
        // non-ASCII name reported `distro-not-found` and then listed the name
        // the user had just typed among the suggestions.
        std::vector<std::string> arguments;
        arguments.reserve(raw_arguments.size() - skip);
        for (wchar_t* const argument : raw_arguments.subspan(skip)) {
            arguments.push_back(model::to_utf8(argument));
        }

        return run(arguments, out, err);
    } catch (const std::exception& error) {
        report_failure(err, error.what());
        return exit_code_for(ErrorCode::Generic);
    }
}

}  // namespace wsldisk::cli
