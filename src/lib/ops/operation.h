#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../errors.h"
#include "../interfaces.h"

namespace wsldisk::ops {

/// One step an operation intends to take.
struct StepPlan {
    /// Written for the user, not the log: it appears verbatim under `--dry-run`.
    std::string description;
    /// Whether it changes anything. A step that only reads needs no undo and
    /// runs unchanged in a dry run.
    bool mutates = false;
    /// How the step would be undone, when it can be. A mutating step with no
    /// undo is a point of no return, and the runner insists those come last.
    std::optional<std::string> undo_description;

    /// Whether reaching this step commits the operation.
    [[nodiscard]] bool is_irreversible() const noexcept { return mutates && !undo_description.has_value(); }
};

/// What an operation expects to achieve, where it can say.
struct Estimate {
    std::optional<std::uint64_t> bytes_freed;
    std::optional<std::chrono::milliseconds> duration;
};

/// Something the user should know before agreeing.
struct Warning {
    std::string message;
    std::string remedy;
};

/// The result of the read-only preflight.
struct Plan {
    std::vector<StepPlan> steps;
    Estimate estimate;
    std::vector<Warning> warnings;

    /// Whether running this would change anything at all.
    [[nodiscard]] bool mutates() const noexcept;
};

/// What actually happened.
struct Report {
    /// Descriptions of the steps that ran, in order.
    std::vector<std::string> completed;
    Estimate actual;
};

/// Where an operation reports what it is doing.
///
/// An interface rather than a callback so a renderer can hold state -- a
/// progress bar needs to know which step it is drawing.
class ProgressSink {
public:
    ProgressSink() = default;
    ProgressSink(const ProgressSink&) = delete;
    ProgressSink& operator=(const ProgressSink&) = delete;
    ProgressSink(ProgressSink&&) = delete;
    ProgressSink& operator=(ProgressSink&&) = delete;
    virtual ~ProgressSink() = default;

    virtual void step_started(std::size_t index, const StepPlan& step) = 0;
    virtual void step_finished(std::size_t index, const StepPlan& step) = 0;
    /// Progress within a long step, such as a compaction.
    virtual void step_progress(const DiskProgress& progress) = 0;
    /// Anything else worth showing: a warning, or what an undo is doing.
    virtual void message(std::string_view text) = 0;
};

/// A sink that drops everything, for callers that do not want output.
class NullSink final : public ProgressSink {
public:
    void step_started(std::size_t /*index*/, const StepPlan& /*step*/) override {}

    void step_finished(std::size_t /*index*/, const StepPlan& /*step*/) override {}

    void step_progress(const DiskProgress& /*progress*/) override {}

    void message(std::string_view /*text*/) override {}
};

/// Undo entries, popped in reverse.
///
/// A mutating step pushes one as soon as it has changed something, so an
/// operation that fails half way can put back exactly what it altered and
/// nothing it did not.
class UndoStack {
public:
    void push(std::string description, std::function<Status()> undo);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    /// Runs every entry, most recent first, and empties the stack.
    ///
    /// `noexcept`, and a failing entry does not stop the ones after it: this
    /// runs while something has already gone wrong, and giving up half way
    /// through would leave more behind than finishing badly does. Failures are
    /// reported to `progress` because the user has to know what was not undone.
    void unwind(ProgressSink& progress) noexcept;

private:
    struct Entry {
        std::string description;
        std::function<Status()> undo;
    };

    std::vector<Entry> entries_;
};

/// An operation: plan, execute, verify, and undo if it goes wrong.
class IOperation {
public:
    IOperation() = default;
    IOperation(const IOperation&) = delete;
    IOperation& operator=(const IOperation&) = delete;
    IOperation(IOperation&&) = delete;
    IOperation& operator=(IOperation&&) = delete;
    virtual ~IOperation() = default;

    /// Read-only preflight. Must change nothing, because `--dry-run` stops here.
    [[nodiscard]] virtual Result<Plan> plan() = 0;

    /// Runs the steps, pushing undo entries as it mutates.
    [[nodiscard]] virtual Result<Report> execute(ProgressSink& progress) = 0;

    /// Checks the operation achieved what it claimed.
    [[nodiscard]] virtual Status verify() = 0;

    /// Puts back whatever `execute` changed. Best effort, LIFO, and never
    /// throws -- it runs when something has already failed.
    virtual void rollback(ProgressSink& progress) noexcept = 0;
};

}  // namespace wsldisk::ops
