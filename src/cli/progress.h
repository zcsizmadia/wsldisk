#pragma once

#include <cstddef>
#include <iosfwd>
#include <string_view>

#include "ops/operation.h"

namespace wsldisk::cli {

/// Reports an operation's progress to a stream.
///
/// Shared rather than written per command: every operation reports the same
/// four things, and one place to change means `compact` and `relink` cannot
/// end up formatting their steps differently.
///
/// Nothing here goes to stdout under `--json`; the caller passes `ops::NullSink`
/// instead, because a progress line in the middle of a machine-readable stream
/// makes it unparseable.
class ConsoleSink final : public ops::ProgressSink {
public:
    explicit ConsoleSink(std::ostream& out) : out_(&out) {}

    /// Wipes a transient line still on screen, so a caller that prints its own
    /// summary afterwards does not print it over the top of one.
    ~ConsoleSink() override;

    /// Neither copied nor moved: two sinks on one stream would each think they
    /// owned the transient line and blank each other's. The base deletes these
    /// already; spelling them out is what says it was meant.
    ConsoleSink(const ConsoleSink&) = delete;
    ConsoleSink& operator=(const ConsoleSink&) = delete;
    ConsoleSink(ConsoleSink&&) = delete;
    ConsoleSink& operator=(ConsoleSink&&) = delete;

    void step_started(std::size_t index, const ops::StepPlan& step) override;
    void step_finished(std::size_t index, const ops::StepPlan& step) override;
    void step_progress(const DiskProgress& progress) override;
    void message(std::string_view text) override;
    void status(std::string_view text) override;

private:
    /// Prints a line that stays.
    void write_line(std::string_view text);

    /// Prints a line the next thing printed will replace.
    void write_transient(std::string_view text);

    /// Blanks the transient line, if there is one.
    void erase_transient();

    std::ostream* out_;
    /// Width of the transient line currently on screen, or zero.
    ///
    /// Kept so the next write can blank exactly as many columns as it wrote:
    /// a bare carriage return leaves the tail of a longer line behind, which is
    /// how "waiting ... 100s" becomes "compacting ...0s".
    std::size_t transient_width_ = 0;
};

}  // namespace wsldisk::cli
