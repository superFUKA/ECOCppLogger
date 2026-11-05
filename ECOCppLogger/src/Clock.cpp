#include "ECOCppLogger/Clock.h"

#include <atomic>

namespace eco::log {

namespace {

// 測る前の仮の値（3GHz）。ECO_LOG_EVERY_MS の間隔の見積もりにだけ使う。
std::atomic<double> g_ticksPerMillisecond{ECO_LOG_HAS_TSC ? 3.0e6 : 1.0e6};

std::int64_t unixNanosecondsNow() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

double ticksPerMillisecond() noexcept {
    return g_ticksPerMillisecond.load(std::memory_order_relaxed);
}

Clock::Clock()
    : baseTick_(ticks()), baseSteady_(std::chrono::steady_clock::now()), baseUnixNanoseconds_(unixNanosecondsNow()) {
#if ECO_LOG_HAS_TSC
    // 最初の見積もりのために 2ms だけ待つ（start() で1回だけ）。
    while (std::chrono::steady_clock::now() - baseSteady_ < std::chrono::milliseconds(2)) {
    }
#endif
    recalibrate();
}

void Clock::recalibrate() {
#if ECO_LOG_HAS_TSC
    const std::uint64_t tick = ticks();
    const auto steady = std::chrono::steady_clock::now();
    const double nanoseconds = std::chrono::duration<double, std::nano>(steady - baseSteady_).count();
    if (nanoseconds > 0.0 && tick > baseTick_) {
        ticksPerNanosecond_ = static_cast<double>(tick - baseTick_) / nanoseconds;
    }
#else
    ticksPerNanosecond_ = 1.0;
#endif
    g_ticksPerMillisecond.store(ticksPerNanosecond_ * 1.0e6, std::memory_order_relaxed);
}

std::int64_t Clock::toUnixNanoseconds(std::uint64_t tick) const noexcept {
    // start() の前に書かれた記録は base より前になるので、符号付きで計算する。
    const double delta = static_cast<double>(static_cast<std::int64_t>(tick - baseTick_));
    return baseUnixNanoseconds_ + static_cast<std::int64_t>(delta / ticksPerNanosecond_);
}

}  // namespace eco::log
