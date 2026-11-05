#pragma once

#include <chrono>
#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define ECO_LOG_HAS_TSC 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#define ECO_LOG_HAS_TSC 1
#else
#define ECO_LOG_HAS_TSC 0
#endif

namespace eco::log {

// ホットパスで使う時刻。x86/x64 では TSC（数ナノ秒で読める）、ほかは steady_clock のナノ秒。
// 実時刻への変換は裏のスレッドで Clock が行う。
inline std::uint64_t ticks() noexcept {
#if ECO_LOG_HAS_TSC
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// ticks() の値を実時刻・経過時間に直す。作ったときと recalibrate() のたびに、ticks と steady_clock の比を測り直す
// （測る区間が長いほど正確になる）。
class Clock {
public:
    Clock();

    // 作ってから今までの区間で、1ナノ秒あたりの ticks を測り直す。
    void recalibrate();

    // ticks() の値 → UNIX 時刻（ナノ秒）。
    std::int64_t toUnixNanoseconds(std::uint64_t tick) const noexcept;

    // ticks の差 → ナノ秒。
    double toNanoseconds(double tickDelta) const noexcept { return tickDelta / ticksPerNanosecond_; }

    double ticksPerNanosecond() const noexcept { return ticksPerNanosecond_; }

private:
    std::uint64_t baseTick_;
    std::chrono::steady_clock::time_point baseSteady_;
    std::int64_t baseUnixNanoseconds_;
    double ticksPerNanosecond_ = 1.0;
};

// ホットパス（ECO_LOG_EVERY_MS 等）で使う、1ミリ秒あたりの ticks の見積もり。Clock が測るたびに更新する。
double ticksPerMillisecond() noexcept;

}  // namespace eco::log
