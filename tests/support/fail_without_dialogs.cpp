// A crashing or asserting test must fail the run, not stop it.
//
// The debug CRT answers an assertion, an abort, or a corrupted stack with a
// modal dialog. Locally that waits for someone to click it; in CI nobody does,
// so the job burns its whole timeout and reports as "hung" rather than as the
// failure it is. Routing all of it to stderr turns those into an ordinary
// non-zero exit with a message attached.
//
// This runs before main through a namespace-scope initialiser, so it is in
// force for every test in the binary, including anything that fails during
// static initialisation.

#include <windows.h>

#include <crtdbg.h>

#include <cstdlib>
#include <initializer_list>

namespace {

int silence_crash_dialogs() {
    // No "a problem caused this program to stop working" box, and no drive-not-
    // ready box either.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // abort() without the "abnormal program termination" dialog or a WER report.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // Debug CRT assertions and /RTC failures to stderr instead of a dialog.
    for (const int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
    return 0;
}

// NOLINTNEXTLINE(cert-err58-cpp) -- the initialiser cannot throw.
const int installed = silence_crash_dialogs();

}  // namespace
