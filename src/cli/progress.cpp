#include "progress.h"

#include <ostream>

namespace wsldisk::cli {

void ConsoleSink::step_started(std::size_t /*index*/, const ops::StepPlan& step) {
    *out_ << "  " << step.description << " ...\n";
}

void ConsoleSink::step_finished(std::size_t /*index*/, const ops::StepPlan& /*step*/) {
    // Deliberately silent. A "done" line per step doubles the output to say
    // nothing new; a step that did not finish is reported as an error instead.
}

void ConsoleSink::step_progress(const DiskProgress& progress) {
    if (progress.total == 0) {
        // A percentage of nothing. The operation is running and has not said
        // how much there is to do, which is not something to print.
        return;
    }
    const std::uint64_t percent = progress.current * 100 / progress.total;
    *out_ << "  " << percent << "%\r" << std::flush;
}

void ConsoleSink::message(std::string_view text) {
    *out_ << "  " << text << '\n';
}

}  // namespace wsldisk::cli
