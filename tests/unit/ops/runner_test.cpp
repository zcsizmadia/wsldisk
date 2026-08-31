#include "ops/runner.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "errors.h"
#include "recording_sink.h"

using wsldisk::ErrorCode;
using wsldisk::Status;
using wsldisk::ops::irreversible_steps_are_last;
using wsldisk::ops::Plan;
using wsldisk::ops::Report;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::ops::StepPlan;
using wsldisk::ops::UndoStack;
using wsldisk::testing::RecordingSink;

namespace {

/// An operation whose behaviour is written by the test.
///
/// Steps are undoable unless said otherwise, and `fail_at` decides which one
/// goes wrong -- which is what makes "rollback undoes exactly the steps that
/// completed, in reverse" something that can be asserted rather than assumed.
class TestOperation final : public wsldisk::ops::IOperation {
public:
    /// Index of the step that fails, or -1 for none.
    int fail_at = -1;
    /// Steps to plan. Defaults to three undoable mutating steps.
    std::vector<StepPlan> steps{
        StepPlan{.description = "first", .mutates = true, .undo_description = "undo first"},
        StepPlan{.description = "second", .mutates = true, .undo_description = "undo second"},
        StepPlan{.description = "third", .mutates = true, .undo_description = "undo third"}};

    bool plan_fails = false;
    bool verify_fails = false;
    /// Index of an undo entry that should itself fail.
    int undo_fails_at = -1;
    /// Index of an undo entry that should throw rather than return an error.
    int undo_throws_at = -1;
    /// Index of an undo entry that throws something that is not a
    /// std::exception, which a `catch (const std::exception&)` alone would let
    /// escape a noexcept function and terminate the process.
    int undo_throws_oddly_at = -1;

    /// Every step that actually ran, and every undo that actually ran.
    std::vector<std::string> ran;
    std::vector<std::string> undone;
    int plans = 0;
    int verifies = 0;

    [[nodiscard]] wsldisk::Result<Plan> plan() override {
        ++plans;
        if (plan_fails) {
            return wsldisk::fail(ErrorCode::Preflight, "the preflight refused", "fix it and retry");
        }
        return Plan{.steps = steps};
    }

    [[nodiscard]] wsldisk::Result<Report> execute(wsldisk::ops::ProgressSink& progress) override {
        Report report;
        for (std::size_t index = 0; index < steps.size(); ++index) {
            const StepPlan& step = steps[index];
            progress.step_started(index, step);

            if (fail_at >= 0 && index == static_cast<std::size_t>(fail_at)) {
                return wsldisk::fail(ErrorCode::Generic, "step " + step.description + " failed",
                                     "look at the log");
            }

            ran.push_back(step.description);
            if (step.undo_description.has_value()) {
                undo_.push(*step.undo_description, [this, index, step]() -> Status {
                    if (undo_throws_at >= 0 && index == static_cast<std::size_t>(undo_throws_at)) {
                        throw std::runtime_error("the undo threw");
                    }
                    if (undo_throws_oddly_at >= 0 &&
                        index == static_cast<std::size_t>(undo_throws_oddly_at)) {
                        throw 42;
                    }
                    undone.push_back(*step.undo_description);
                    if (undo_fails_at >= 0 && index == static_cast<std::size_t>(undo_fails_at)) {
                        return wsldisk::fail(ErrorCode::Generic, "the undo failed", "put it back by hand");
                    }
                    return {};
                });
            }

            report.completed.push_back(step.description);
            progress.step_finished(index, step);
        }
        return report;
    }

    [[nodiscard]] Status verify() override {
        ++verifies;
        if (verify_fails) {
            return wsldisk::fail(ErrorCode::Partial, "the result is not what was planned",
                                 "run `wsldisk info` to see the state");
        }
        return {};
    }

    void rollback(wsldisk::ops::ProgressSink& progress) noexcept override { undo_.unwind(progress); }

    [[nodiscard]] std::size_t pending_undos() const noexcept { return undo_.size(); }

private:
    UndoStack undo_;
};

}  // namespace

TEST_CASE("a successful run plans, executes and verifies", "[ops][runner]") {
    TestOperation operation;
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK(operation.plans == 1);
    CHECK(operation.verifies == 1);
    REQUIRE(outcome->report.has_value());
    CHECK(outcome->report->completed.size() == 3);
    // Nothing was undone. Asserted on the operation, because the outcome no
    // longer carries a flag that no caller could read.
    CHECK(operation.undone.empty());
    CHECK(progress.started == std::vector<std::size_t>{0, 1, 2});
    CHECK(progress.finished == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE("a dry run executes nothing and still returns the plan", "[ops][runner]") {
    TestOperation operation;
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{.dry_run = true});

    REQUIRE(outcome.has_value());
    CHECK(outcome->plan.steps.size() == 3);
    CHECK_FALSE(outcome->report.has_value());
    CHECK(operation.ran.empty());
    CHECK(operation.verifies == 0);
    CHECK(progress.started.empty());
}

TEST_CASE("a failed preflight stops before anything runs", "[ops][runner]") {
    TestOperation operation;
    operation.plan_fails = true;
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::Preflight);
    CHECK(operation.ran.empty());
    CHECK(operation.verifies == 0);
}

TEST_CASE("a failure rolls back exactly the steps that completed", "[ops][runner]") {
    // The third step fails, so the first two are undone and nothing else is.
    TestOperation operation;
    operation.fail_at = 2;
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(operation.ran == std::vector<std::string>{"first", "second"});
    CHECK(operation.undone == std::vector<std::string>{"undo second", "undo first"});
}

TEST_CASE("rollback runs in reverse order", "[ops][runner]") {
    // Each undo has to see the state the one after it created, so the order is
    // part of the contract rather than an implementation detail.
    TestOperation operation;
    operation.fail_at = 2;
    RecordingSink progress;

    std::ignore = run(operation, progress, RunOptions{});

    REQUIRE(operation.undone.size() == 2);
    CHECK(operation.undone[0] == "undo second");
    CHECK(operation.undone[1] == "undo first");
}

TEST_CASE("a failure at the first step undoes nothing", "[ops][runner]") {
    TestOperation operation;
    operation.fail_at = 0;
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(operation.ran.empty());
    CHECK(operation.undone.empty());
}

TEST_CASE("the outcome records that a rollback happened", "[ops][runner]") {
    TestOperation operation;
    operation.fail_at = 1;
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    // The error is what the caller sees, but the sink was told what was undone.
    CHECK(progress.said("undoing: undo first"));
}

TEST_CASE("a failing undo does not stop the others", "[ops][runner]") {
    // Rollback runs when something has already gone wrong; giving up half way
    // leaves more behind than finishing badly does.
    TestOperation operation;
    operation.fail_at = 2;
    operation.undo_fails_at = 1;
    RecordingSink progress;

    std::ignore = run(operation, progress, RunOptions{});

    CHECK(operation.undone == std::vector<std::string>{"undo second", "undo first"});
    CHECK(progress.said("could not undo undo second"));
}

TEST_CASE("an undo that throws does not stop the others", "[ops][runner]") {
    // rollback is noexcept, so an exception escaping an undo would terminate
    // the process during error handling.
    TestOperation operation;
    operation.fail_at = 2;
    operation.undo_throws_at = 1;
    RecordingSink progress;

    std::ignore = run(operation, progress, RunOptions{});

    // The second step's undo threw; the first step's still ran.
    CHECK(operation.undone == std::vector<std::string>{"undo first"});
    CHECK(progress.said("the undo threw"));
}

TEST_CASE("an undo that throws something odd does not escape", "[ops][runner]") {
    // rollback is noexcept, so anything escaping it terminates the process --
    // including a throw that is not a std::exception at all.
    TestOperation operation;
    operation.fail_at = 2;
    operation.undo_throws_oddly_at = 1;
    RecordingSink progress;

    std::ignore = run(operation, progress, RunOptions{});

    CHECK(operation.undone == std::vector<std::string>{"undo first"});
    CHECK(progress.said("unknown failure"));
}

TEST_CASE("the undo stack empties even when every undo fails", "[ops][runner]") {
    TestOperation operation;
    operation.fail_at = 2;
    operation.undo_throws_at = 0;
    RecordingSink progress;

    std::ignore = run(operation, progress, RunOptions{});

    CHECK(operation.pending_undos() == 0);
}

TEST_CASE("a failed verify is reported without undoing the work", "[ops][runner]") {
    // Execution succeeded, so the changes were made on purpose; what failed is
    // the check that they added up. Undoing on that signal would turn "unsure"
    // into "changed your disk again".
    TestOperation operation;
    operation.verify_fails = true;
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::Partial);
    CHECK(operation.undone.empty());
    CHECK(operation.ran.size() == 3);
}

TEST_CASE("a step that only reads needs no undo", "[ops][runner]") {
    TestOperation operation;
    operation.steps = {StepPlan{.description = "look at the disk", .mutates = false}};
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->plan.mutates());
    CHECK(operation.pending_undos() == 0);
}

TEST_CASE("a plan is rejected when an undoable step follows a point of no return", "[ops][runner]") {
    // Once past a step that cannot be undone, a rollback can no longer restore
    // the starting state, so the ordering is a contract rather than a style.
    TestOperation operation;
    operation.steps = {
        StepPlan{.description = "delete the old disk", .mutates = true},
        StepPlan{.description = "rewrite the registry", .mutates = true, .undo_description = "put it back"}};
    RecordingSink progress;

    const auto outcome = run(operation, progress, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("point of no return") != std::string::npos);
    CHECK(operation.ran.empty());
}

TEST_CASE("a plan ending with the irreversible step is accepted", "[ops][runner]") {
    TestOperation operation;
    operation.steps = {
        StepPlan{.description = "rewrite the registry", .mutates = true, .undo_description = "put it back"},
        StepPlan{.description = "delete the old disk", .mutates = true}};
    RecordingSink progress;

    CHECK(run(operation, progress, RunOptions{}).has_value());
}

TEST_CASE("a read-only step after a point of no return is allowed", "[ops][runner]") {
    // Only *mutating* steps have to come before it; a check afterwards is
    // exactly the verify checkpoint the design asks for.
    Plan plan;
    plan.steps = {StepPlan{.description = "delete", .mutates = true},
                  StepPlan{.description = "check it worked", .mutates = false}};

    CHECK(irreversible_steps_are_last(plan));
}

TEST_CASE("an empty plan orders its irreversible steps vacuously", "[ops][runner]") {
    CHECK(irreversible_steps_are_last(Plan{}));
}

TEST_CASE("Plan::mutates is false when every step only reads", "[ops][runner]") {
    Plan plan;
    plan.steps = {StepPlan{.description = "read"}, StepPlan{.description = "read again"}};

    CHECK_FALSE(plan.mutates());
}

TEST_CASE("Plan::mutates is true when any step changes something", "[ops][runner]") {
    Plan plan;
    plan.steps = {StepPlan{.description = "read"}, StepPlan{.description = "write", .mutates = true}};

    CHECK(plan.mutates());
}

TEST_CASE("the null sink accepts everything and reports nothing", "[ops][runner]") {
    // It exists so a caller that wants no output does not have to write one.
    wsldisk::ops::NullSink sink;
    const StepPlan step{.description = "anything"};

    sink.step_started(0, step);
    sink.step_progress(wsldisk::DiskProgress{.current = 1, .total = 2});
    sink.message("ignored");
    sink.status("ignored too");
    sink.step_finished(0, step);

    SUCCEED("the null sink swallowed every call");
}

TEST_CASE("an undo stack unwinds even with no progress to report", "[ops][runner]") {
    UndoStack stack;
    int undone = 0;
    stack.push("only entry", [&undone]() -> Status {
        ++undone;
        return {};
    });
    wsldisk::ops::NullSink sink;

    stack.unwind(sink);

    CHECK(undone == 1);
    CHECK(stack.empty());
}
