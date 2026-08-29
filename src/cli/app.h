#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace wsldisk::cli {

/// Runs one command line and returns the process exit code.
///
/// `args` excludes the program name. Nothing is written to the real stdio: the
/// caller supplies the streams, which is what lets the unit tests assert on
/// output without spawning a process.
[[nodiscard]] int run(std::span<const std::string> args, std::ostream& out, std::ostream& err);

}  // namespace wsldisk::cli
