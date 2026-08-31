#include "runner.h"

#include <algorithm>

namespace wsldisk::ops {

bool irreversible_steps_are_last(const Plan& plan) noexcept {
    // Walk once: after the first irreversible step, nothing undoable may follow.
    bool past_no_return = false;
    for (const StepPlan& step : plan.steps) {
        if (step.is_irreversible()) {
            past_no_return = true;
            continue;
        }
        if (past_no_return && step.mutates) {
            return false;
        }
    }
    return true;
}

Result<RunOutcome> run(IOperation& operation, ProgressSink& progress, const RunOptions& options) {
    auto planned = operation.plan();
    if (!planned.has_value()) {
        return std::unexpected(planned.error());
    }

    if (!irreversible_steps_are_last(*planned)) {
        // A bug in the operation rather than anything the user did, but it is
        // caught here because the alternative is discovering it during a
        // rollback that cannot restore the starting state.
        return fail(ErrorCode::Generic, "the operation plans an undoable change after a point of no return",
                    "this is a bug in wsldisk; please report it with the command you ran");
    }

    RunOutcome outcome{.plan = std::move(*planned)};
    if (options.dry_run) {
        // The plan is the whole answer. Nothing runs, including the steps that
        // only read: a dry run must be free of side effects, not merely free of
        // mutations.
        return outcome;
    }

    auto report = operation.execute(progress);
    if (!report.has_value()) {
        operation.rollback(progress);
        return std::unexpected(report.error());
    }
    outcome.report = std::move(*report);

    // Deliberately no rollback on a failed verify -- see the header.
    if (const Status verified = operation.verify(); !verified.has_value()) {
        return std::unexpected(verified.error());
    }
    return outcome;
}

}  // namespace wsldisk::ops
