#include "clock.h"

#include <windows.h>

#include <algorithm>

#include "win32_api.h"

namespace wsldisk::platform {

std::chrono::steady_clock::time_point SystemClock::now() const {
    return std::chrono::steady_clock::now();
}

void SystemClock::sleep_for(std::chrono::milliseconds duration) const {
    // A negative duration is a caller that computed a deadline already past, not
    // a request to sleep for four billion milliseconds -- which is what the
    // unsigned conversion would otherwise produce.
    const auto milliseconds = std::max<std::chrono::milliseconds::rep>(duration.count(), 0);
    win32().sleep(static_cast<DWORD>(milliseconds));
}

}  // namespace wsldisk::platform
