#pragma once

#include <iosfwd>

#include "ops/operation.h"

namespace wsldisk::cli {

/// Reports an operation's progress to a stream.
///
/// Shared rather than written per command: every operation reports the same
/// three things, and one place to change means `compact` and `relink` cannot
/// end up formatting their steps differently.
///
/// Nothing here goes to stdout under `--json`; the caller passes `ops::NullSink`
/// instead, because a progress line in the middle of a machine-readable stream
/// makes it unparseable.
class ConsoleSink final : public ops::ProgressSink {
public:
    explicit ConsoleSink(std::ostream& out) : out_(&out) {}

    void step_started(std::size_t index, const ops::StepPlan& step) override;
    void step_finished(std::size_t index, const ops::StepPlan& step) override;
    void step_progress(const DiskProgress& progress) override;
    void message(std::string_view text) override;

private:
    std::ostream* out_;
};

}  // namespace wsldisk::cli
