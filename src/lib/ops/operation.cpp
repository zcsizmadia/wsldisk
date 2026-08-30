#include "operation.h"

#include <algorithm>
#include <format>
#include <utility>

namespace wsldisk::ops {

bool Plan::mutates() const noexcept {
    return std::ranges::any_of(steps, [](const StepPlan& step) { return step.mutates; });
}

void UndoStack::push(std::string description, std::function<Status()> undo) {
    entries_.push_back(Entry{.description = std::move(description), .undo = std::move(undo)});
}

void UndoStack::unwind(ProgressSink& progress) noexcept {
    // Reverse order: the last change made is the first put back, so each entry
    // sees the state it created.
    while (!entries_.empty()) {
        Entry entry = std::move(entries_.back());
        entries_.pop_back();

        // An undo that throws must not stop the ones after it, and must not
        // escape: this is already the failure path.
        try {
            progress.message(std::format("undoing: {}", entry.description));
            if (const Status undone = entry.undo(); !undone.has_value()) {
                progress.message(
                    std::format("could not undo {}: {}", entry.description, undone.error().to_string()));
            }
        } catch (const std::exception& error) {
            progress.message(std::format("could not undo {}: {}", entry.description, error.what()));
        } catch (...) {
            progress.message(std::format("could not undo {}: unknown failure", entry.description));
        }
    }
}

}  // namespace wsldisk::ops
