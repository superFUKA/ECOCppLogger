#pragma once

// 裏のスレッドで、記録を JSON Lines の1行に直す。

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ECOCppLogger/Clock.h"
#include "ECOCppLogger/Record.h"

namespace eco::log::detail {

void appendJsonString(std::string& out, std::string_view text);
void appendDouble(std::string& out, double value);
void appendUnsigned(std::string& out, std::uint64_t value);
void appendSigned(std::string& out, std::int64_t value);

// UNIX 時刻（ナノ秒）→ "2026-11-05T21:03:04.123456+09:00"（マイクロ秒まで、このPCの時差）。
void appendTimestamp(std::string& out, std::int64_t unixNanoseconds, int utcOffsetMinutes);

// "void __cdecl Game::Player::update(float)" → "Game::Player::update"
std::string_view shortFunctionName(std::string_view function) noexcept;

// フルパス → ファイル名（fullPath なら そのまま）。
std::string_view sourceFileName(std::string_view file, bool fullPath) noexcept;

int localUtcOffsetMinutes();

class LineFormatter {
public:
    LineFormatter(const Clock& clock, bool fullPath);

    // 記録1件 → JSON の1行（改行つき）を out に足す。
    void appendRecord(std::string& out, const RecordHeader& header, std::string_view threadName);

    // 直前の appendRecord で作った本文（標準エラーへ出すとき用）。
    std::string_view lastMessage() const noexcept { return message_; }

    // 記録と同じ形の "t" で始まる行の頭："{\"t\":\"…\""
    void appendLineStart(std::string& out, std::uint64_t tick);

    const Clock& clock() const noexcept { return clock_; }
    bool fullPath() const noexcept { return fullPath_; }

private:
    void renderMessage(const RecordHeader& header);

    const Clock& clock_;
    bool fullPath_;
    int utcOffsetMinutes_;
    std::string message_;
    std::unordered_map<const Site*, std::string> siteCache_;
    std::int64_t cachedSecond_ = 0;
    std::string cachedPrefix_;
    std::string cachedSuffix_;
};

}  // namespace eco::log::detail
