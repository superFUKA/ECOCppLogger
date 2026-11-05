#pragma once

#include <stdint.h>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define ECO_LOG_HAS_TSC 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#define ECO_LOG_HAS_TSC 1
#else
#define ECO_LOG_HAS_TSC 0
#endif

namespace lg {

namespace detail {
uint64_t steadyNanoseconds() noexcept;
}

// ログを書く側で使う時刻。x86/x64 では TSC（数ns で読める）、ほかは steady_clock のナノ秒。
// 実時刻への換算は書き出す側（Clock）と読む側で行う。
inline uint64_t ticks() noexcept {
#if ECO_LOG_HAS_TSC
    return __rdtsc();
#else
    return detail::steadyNanoseconds();
#endif
}

// ticks() の値を実時刻・経過時間に直す。作ったときと recalibrate() のたびに ticks と steady_clock の比を測り直す
// （測る区間が長いほど正確になる）。
class Clock {
public:
    Clock() noexcept;

    void recalibrate() noexcept;

    int64_t toUnixNanoseconds(uint64_t tick) const noexcept;
    double toNanoseconds(double tickDelta) const noexcept { return tickDelta / ticksPerNanosecond_; }

    double ticksPerNanosecond() const noexcept { return ticksPerNanosecond_; }
    uint64_t baseTick() const noexcept { return baseTick_; }
    int64_t baseUnixNanoseconds() const noexcept { return baseUnixNanoseconds_; }

private:
    uint64_t baseTick_;
    uint64_t baseSteady_;
    int64_t baseUnixNanoseconds_;
    double ticksPerNanosecond_ = 1.0;
};

// ログを書く側（ECO_LOG_EVERY_MS 等）で使う、1ミリ秒あたりの ticks の見積もり。Clock が測るたびに更新する。
double ticksPerMillisecond() noexcept;

}  // namespace lg
