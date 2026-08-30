#pragma once

#include <chrono>
#include <vector>

#include "interfaces.h"

namespace wsldisk::testing {

/// An `IClock` that never waits.
///
/// `sleep_for` advances the clock by exactly what it was asked to sleep and
/// records it, so a retry loop runs at full speed and a test can assert how long
/// it *would* have waited -- that the backoff grows, that it gave up at the
/// deadline rather than one attempt late.
class FakeClock final : public IClock {
public:
    FakeClock() = default;

    /// `IClock` deletes move to stop slicing through a base reference. This one
    /// is `final`, so there is nothing to slice.
    FakeClock(FakeClock&& other) noexcept : IClock(), now_(other.now_), slept_(std::move(other.slept_)) {}

    [[nodiscard]] std::chrono::steady_clock::time_point now() const override { return now_; }

    void sleep_for(std::chrono::milliseconds duration) const override {
        slept_.push_back(duration);
        now_ += duration;
    }

    /// Moves time on without a sleep, for whatever the code under test is
    /// waiting on happening by itself.
    void advance(std::chrono::milliseconds duration) { now_ += duration; }

    /// Every sleep, in order.
    [[nodiscard]] const std::vector<std::chrono::milliseconds>& slept() const noexcept { return slept_; }

    /// What the sleeps add up to.
    [[nodiscard]] std::chrono::milliseconds total_slept() const noexcept {
        std::chrono::milliseconds total{};
        for (const auto& slice : slept_) {
            total += slice;
        }
        return total;
    }

private:
    // Mutable so the const interface can still record what happened; the fake is
    // a test double, not a value type.
    mutable std::chrono::steady_clock::time_point now_{};
    mutable std::vector<std::chrono::milliseconds> slept_;
};

}  // namespace wsldisk::testing
