#include "app.h"

#include <CLI/CLI.hpp>

#include <exception>
#include <ostream>
#include <span>
#include <string>
#include <vector>

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

int main_entry(int argc, char** argv, std::ostream& out, std::ostream& err) noexcept {
    try {
        // A span rather than pointer arithmetic on argv, and argc is not assumed
        // to be at least one.
        const std::span<char* const> raw_arguments{argv, static_cast<std::size_t>(argc)};
        const std::size_t skip = raw_arguments.empty() ? 0 : 1;

        std::vector<std::string> arguments;
        arguments.reserve(raw_arguments.size() - skip);
        for (char* const argument : raw_arguments.subspan(skip)) {
            arguments.emplace_back(argument);
        }

        return run(arguments, out, err);
    } catch (const std::exception& error) {
        report_failure(err, error.what());
        return exit_code_for(ErrorCode::Generic);
    }
}

}  // namespace wsldisk::cli
