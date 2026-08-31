#pragma once

#include <string>
#include <vector>

#include "ops/operation.h"

namespace wsldisk::testing {

/// A `ProgressSink` that remembers everything it was told.
///
/// Assertions about an operation's behaviour are mostly assertions about what
/// it reported and in what order -- that rollback ran the right undos, in
/// reverse, and said so.
class RecordingSink final : public ops::ProgressSink {
public:
    void step_started(std::size_t index, const ops::StepPlan& step) override {
        started.push_back(index);
        events.push_back("start: " + step.description);
    }

    void step_finished(std::size_t index, const ops::StepPlan& step) override {
        finished.push_back(index);
        events.push_back("finish: " + step.description);
    }

    void step_progress(const DiskProgress& progress) override { progress_reports.push_back(progress); }

    void message(std::string_view text) override {
        messages.emplace_back(text);
        events.emplace_back(text);
    }

    /// Kept apart from `messages`: a transient line is not something the user
    /// reads once, and a test that counts messages should not count these.
    void status(std::string_view text) override { statuses.emplace_back(text); }

    /// Whether any message contains `needle`.
    [[nodiscard]] bool said(std::string_view needle) const {
        return std::ranges::any_of(messages, [needle](const std::string& message) {
            return message.find(needle) != std::string::npos;
        });
    }

    std::vector<std::size_t> started;
    std::vector<std::size_t> finished;
    std::vector<DiskProgress> progress_reports;
    std::vector<std::string> messages;
    std::vector<std::string> statuses;
    /// Everything in the order it happened, for asserting sequence.
    std::vector<std::string> events;
};

}  // namespace wsldisk::testing
