#include "platform/clock.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "fake_clock.h"
#include "platform/win32_api.h"

using wsldisk::platform::ScopedWin32Api;
using wsldisk::platform::SystemClock;
using wsldisk::platform::Win32Api;
using wsldisk::testing::FakeClock;
using namespace std::chrono_literals;

TEST_CASE("the system clock moves forward", "[platform][clock]") {
    const SystemClock clock;
    const auto first = clock.now();
    const auto second = clock.now();

    CHECK(second >= first);
}

TEST_CASE("sleep_for asks Win32 to sleep for the requested time", "[platform][clock]") {
    DWORD slept = 0;
    Win32Api api;
    api.sleep = [&slept](DWORD milliseconds) { slept = milliseconds; };
    const ScopedWin32Api scoped{api};

    const SystemClock clock;
    clock.sleep_for(250ms);

    CHECK(slept == 250);
}

TEST_CASE("a negative sleep does not become a very long one", "[platform][clock]") {
    // The unsigned conversion would otherwise turn a deadline already past into
    // roughly seven weeks of waiting.
    DWORD slept = 1;
    Win32Api api;
    api.sleep = [&slept](DWORD milliseconds) { slept = milliseconds; };
    const ScopedWin32Api scoped{api};

    const SystemClock clock;
    clock.sleep_for(-5s);

    CHECK(slept == 0);
}

TEST_CASE("the fake clock advances by exactly what it was asked to sleep", "[platform][clock]") {
    FakeClock clock;
    const auto start = clock.now();

    clock.sleep_for(100ms);
    clock.sleep_for(200ms);

    CHECK(clock.now() - start == 300ms);
    CHECK(clock.slept().size() == 2);
    CHECK(clock.total_slept() == 300ms);
}

TEST_CASE("the fake clock can move without sleeping", "[platform][clock]") {
    // For whatever the code is waiting on happening by itself.
    FakeClock clock;
    const auto start = clock.now();

    clock.advance(5s);

    CHECK(clock.now() - start == 5s);
    CHECK(clock.slept().empty());
}

TEST_CASE("a clock owned through the interface reports the same time", "[platform][clock]") {
    FakeClock backing;
    backing.advance(42ms);
    const wsldisk::IClock& clock = backing;

    CHECK(clock.now() == backing.now());
}
