#include "progress.h"

#include <ostream>
#include <string>

namespace wsldisk::cli {

ConsoleSink::~ConsoleSink() {
    erase_transient();
}

void ConsoleSink::step_started(std::size_t /*index*/, const ops::StepPlan& step) {
    write_line(step.description + " ...");
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
    write_transient(std::to_string(percent) + "%");
}

void ConsoleSink::message(std::string_view text) {
    write_line(text);
}

void ConsoleSink::status(std::string_view text) {
    write_transient(text);
}

void ConsoleSink::write_line(std::string_view text) {
    erase_transient();
    *out_ << "  " << text << '\n';
}

void ConsoleSink::write_transient(std::string_view text) {
    erase_transient();
    *out_ << "  " << text << '\r' << std::flush;
    transient_width_ = text.size() + 2;
}

void ConsoleSink::erase_transient() {
    if (transient_width_ == 0) {
        return;
    }
    *out_ << '\r' << std::string(transient_width_, ' ') << '\r' << std::flush;
    transient_width_ = 0;
}

}  // namespace wsldisk::cli
