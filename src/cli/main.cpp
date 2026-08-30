#include <iostream>

#include "app.h"

int main(int argc, char** argv) {
    // Everything, including the argument copy, happens inside main_entry, which
    // is noexcept: nothing may escape to the CRT and turn a diagnosable failure
    // into a crash dialog.
    return wsldisk::cli::main_entry(argc, argv, std::cout, std::cerr);
}
