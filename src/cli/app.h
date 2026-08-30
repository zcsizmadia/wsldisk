#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include "errors.h"
#include "options.h"

namespace wsldisk::cli {

/// Prints an error the way the top level does and returns its exit code.
///
/// In `--json` mode it goes to stdout as an object, so a script reading stdout
/// gets a parseable answer whether or not the command worked; otherwise to
/// stderr, where a human expects it. Never a stack trace either way.
[[nodiscard]] int report(const Error& error, const GlobalOptions& options, std::ostream& out,
                         std::ostream& err);

/// Runs one command line and returns the process exit code.
///
/// `args` excludes the program name. Nothing is written to the real stdio: the
/// caller supplies the streams, which is what lets the unit tests assert on
/// output without spawning a process.
[[nodiscard]] int run(std::span<const std::string> args, std::ostream& out, std::ostream& err);

/// `main` in everything but name: converts argv, runs the command, and turns any
/// exception into a diagnosed failure rather than letting it reach the CRT.
///
/// `argv[0]` is dropped. Being `noexcept` is what keeps `main` itself trivial --
/// the argument copy can allocate, and an exception escaping `main` would surface
/// as a crash dialog instead of a message.
[[nodiscard]] int main_entry(int argc, char** argv, std::ostream& out, std::ostream& err) noexcept;

}  // namespace wsldisk::cli
