#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "app.h"

int main(int argc, char** argv) {
    // argv is the ANSI form; that is fine for the flags and distro names wsldisk
    // takes today. Paths that need the full Unicode range are read from the
    // registry as UTF-16 and never round-trip through argv.
    const std::vector<std::string> args(argv + 1, argv + argc);
    return wsldisk::cli::run(args, std::cout, std::cerr);
}
