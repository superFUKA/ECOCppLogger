#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ECOCppLogger/Clock.h"

TEST(Clock, TicksIncrease) {
    const auto first = eco::log::ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_GT(eco::log::ticks(), first);
}

TEST(Clock, ConvertsTicksToWallTime) {
    eco::log::Clock clock;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto converted = clock.toUnixNanoseconds(eco::log::ticks());
    EXPECT_LT(std::llabs(converted - now), 50'000'000LL);  // 50ms 以内
}

TEST(Clock, MeasuresElapsedTime) {
    eco::log::Clock clock;
    const auto start = eco::log::ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto end = eco::log::ticks();
    clock.recalibrate();
    const double milliseconds = clock.toNanoseconds(static_cast<double>(end - start)) / 1.0e6;
    EXPECT_GT(milliseconds, 15.0);
    EXPECT_LT(milliseconds, 200.0);
    EXPECT_GT(eco::log::ticksPerMillisecond(), 0.0);
}
