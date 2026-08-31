#include <iostream>

#include "app.h"

// clang-tidy knows `main` is an entry point and does not know `wmain` is, so it
// offers to give this internal linkage. Taking that advice would produce a
// binary that does not link. The suppression has to sit on the line before the
// declaration, not before this comment.
//
// NOLINTNEXTLINE(misc-use-internal-linkage)
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
