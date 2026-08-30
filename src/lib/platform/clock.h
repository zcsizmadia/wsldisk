#pragma once

#include "../interfaces.h"

namespace wsldisk::platform {

/// `IClock` on the real monotonic clock, sleeping through the `Win32Api` table.
///
/// The sleep goes through the table so a test that drives production code
/// holding this clock still does not wait; the fake in `tests/fakes` is for the
/// usual case where the operation takes an `IClock` directly.
class SystemClock final : public IClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const override;
    void sleep_for(std::chrono::milliseconds duration) const override;
};

}  // namespace wsldisk::platform
