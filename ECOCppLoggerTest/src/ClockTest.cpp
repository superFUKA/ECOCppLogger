#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#include "ECOCppLogger/Clock.h"

TEST(Clock, TicksIncrease) {
    const auto first = lg::ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_GT(lg::ticks(), first);
}

TEST(Clock, ConvertsTicksToWallTime) {
    lg::Clock clock;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    EXPECT_LT(std::llabs(clock.toUnixNanoseconds(lg::ticks()) - now), 50'000'000LL);  // 50ms 以内
}

TEST(Clock, MeasuresElapsedTime) {
    lg::Clock clock;
    const auto start = lg::ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto end = lg::ticks();
    clock.recalibrate();
    const double milliseconds = clock.toNanoseconds(static_cast<double>(end - start)) / 1.0e6;
    EXPECT_GT(milliseconds, 15.0);
    EXPECT_LT(milliseconds, 200.0);
    EXPECT_GT(lg::ticksPerMillisecond(), 0.0);
}
