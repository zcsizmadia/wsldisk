#pragma once

#include <optional>

#include "../errors.h"
#include "operation.h"

namespace wsldisk::ops {

/// How the runner should treat the operation.
struct RunOptions {
    /// Plan and stop. Nothing mutating runs, and the plan is the whole answer.
    bool dry_run = false;
};

/// What a run produced.
struct RunOutcome {
    /// Always present: the plan is produced even for a dry run, and is what the
    /// caller renders.
    Plan plan;
    /// Absent for a dry run, because nothing executed.
    std::optional<Report> report;
    // There was a `rolled_back` flag here. It could only ever be set on the path
    // that returns `std::unexpected` and discards the outcome, so no caller
    // could observe it true -- the test that covered it had to assert through
    // the progress sink instead. A field that cannot be read is worse than no
    // field: it reads as an available answer. What was undone is reported to the
    // sink as it happens, which is where a user needs it.
};

/// Runs an operation through its lifecycle.
///
/// plan, then -- unless this is a dry run -- execute, then verify. Execution
/// failing rolls the operation back before the error is returned.
///
/// A *verify* failure deliberately does not roll back. Execution reported
/// success, so the undo entries describe changes that were made on purpose;
/// what failed is the check that they added up to the intended result. Undoing
/// on that signal would turn "the tool is unsure" into "the tool changed your
/// disk again", which is the worse of the two. The error says so and the
/// changes stand.
[[nodiscard]] Result<RunOutcome> run(IOperation& operation, ProgressSink& progress,
                                     const RunOptions& options);

/// Whether a plan orders its irreversible steps last.
///
/// The lifecycle only means anything if everything undoable happens before
/// anything that cannot be undone: once a point of no return has passed, a
/// rollback can no longer restore the starting state. Exposed so an operation
/// can check its own plan in a test rather than finding out at runtime.
[[nodiscard]] bool irreversible_steps_are_last(const Plan& plan) noexcept;

}  // namespace wsldisk::ops
