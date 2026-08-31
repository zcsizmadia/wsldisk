#include <iostream>

#include "app.h"

int wmain(int argc, wchar_t** argv) {
    // `wmain`, so arguments arrive as UTF-16 and are converted once, by the same
    // codec everything else uses. Narrow `main` would hand them over already
    // flattened to the active code page.
    //
    // Everything, including the argument copy, happens inside main_entry, which
    // is noexcept: nothing may escape to the CRT and turn a diagnosable failure
    // into a crash dialog.
    return wsldisk::cli::main_entry(argc, argv, std::cout, std::cerr);
}
