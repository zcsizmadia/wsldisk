#include "app.h"

#include <CLI/CLI.hpp>

#include <ostream>

#include "errors.h"
#include "version.h"

namespace wsldisk::cli {

int run(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
    CLI::App app{"Compact, shrink, move, inspect and snapshot WSL2 virtual disks", "wsldisk"};
    app.set_version_flag("-V,--version", std::string{version_banner()});
    app.require_subcommand(0, 1);

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
        err << "error: " << e.what() << "\n\n" << app.help();
        return exit_code_for(ErrorCode::Usage);
    }

    // No subcommand yet: every command lands in M1 (see ROADMAP.md).
    out << app.help();
    return exit_code_success;
}

}  // namespace wsldisk::cli
