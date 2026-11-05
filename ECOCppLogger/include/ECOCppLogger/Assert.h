#pragma once

// アサート：条件が偽なら、ログ（重さ Error・型 assert）・最近の値・スタックを残してから、決めた動き（止める・落とす・続ける）をする。
// 同じ場所の失敗は最初の kAssertDetailed 回だけ詳しく書き、その後は数だけ数える（集計 "assert: 条件"）。

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <atomic>

#include "ECOCppLogger/Config.h"
#include "ECOCppLogger/Record.h"

namespace lg {

// 失敗したときの動きを変える（start() の Config::assertAction も同じ）。
void setAssertAction(AssertAction action) noexcept;
AssertAction assertAction() noexcept;

namespace detail {

inline constexpr uint64_t kAssertDetailed = 10;

// 書式 "assertion failed: <条件>[: <メッセージ>]" をコンパイル時に作る（条件の { } は文字そのものにする）。
template <size_t TextSize, size_t FormatSize>
constexpr auto assertFormat(const char (&text)[TextSize], const char (&format)[FormatSize]) noexcept {
    constexpr char kPrefix[] = "assertion failed: ";
    std::array<char, sizeof(kPrefix) + TextSize * 2 + 2 + FormatSize> out{};
    size_t at = 0;
    for (size_t i = 0; kPrefix[i] != '\0'; ++i) out[at++] = kPrefix[i];
    for (size_t i = 0; i + 1 < TextSize; ++i) {
        out[at++] = text[i];
        if (text[i] == '{' || text[i] == '}') out[at++] = text[i];
    }
    if (FormatSize > 1) {
        out[at++] = ':';
        out[at++] = ' ';
        for (size_t i = 0; i + 1 < FormatSize; ++i) out[at++] = format[i];
    }
    out[at] = '\0';
    return out;
}

// 失敗の後始末（最初の1回は最近の値・スタックも書く）。applyAction なら決めた動きをし、デバッガーで止めるべきなら true。
bool assertFailed(const Site& site, uint64_t failure, bool applyAction) noexcept;

// デバッガーで止める。
void debugBreak() noexcept;

}  // namespace detail

}  // namespace lg
