#include "ECOCppLogger/Clock.h"

#include <atomic>
#include <chrono>

namespace lg {

namespace {

// 測る前の仮の値（3GHz）。ECO_LOG_EVERY_MS の間隔の見積もりにだけ使う。
std::atomic<double> g_ticksPerMillisecond{ECO_LOG_HAS_TSC ? 3.0e6 : 1.0e6};

int64_t unixNanosecondsNow() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

namespace detail {

uint64_t steadyNanoseconds() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace detail

double ticksPerMillisecond() noexcept {
    return g_ticksPerMillisecond.load(std::memory_order_relaxed);
}

Clock::Clock() noexcept
    : baseTick_(ticks()), baseSteady_(detail::steadyNanoseconds()), baseUnixNanoseconds_(unixNanosecondsNow()) {
#if ECO_LOG_HAS_TSC
    // 最初の見積もりのために 2ms だけ待つ（start() で1回だけ）。
    while (detail::steadyNanoseconds() - baseSteady_ < 2'000'000) {
    }
#endif
    recalibrate();
}

void Clock::recalibrate() noexcept {
#if ECO_LOG_HAS_TSC
    const uint64_t tick = ticks();
    const uint64_t steady = detail::steadyNanoseconds();
    if (steady > baseSteady_ && tick > baseTick_) {
        ticksPerNanosecond_ = static_cast<double>(tick - baseTick_) / static_cast<double>(steady - baseSteady_);
    }
#else
    ticksPerNanosecond_ = 1.0;
#endif
    g_ticksPerMillisecond.store(ticksPerNanosecond_ * 1.0e6, std::memory_order_relaxed);
}

int64_t Clock::toUnixNanoseconds(uint64_t tick) const noexcept {
    // start() の前に書かれた記録は base より前になるので、符号付きで計算する。
    const double delta = static_cast<double>(static_cast<int64_t>(tick - baseTick_));
    return baseUnixNanoseconds_ + static_cast<int64_t>(delta / ticksPerNanosecond_);
}

}  // namespace lg
