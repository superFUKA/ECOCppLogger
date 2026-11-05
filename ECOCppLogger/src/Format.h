#pragma once

// 記録を文字列にする（読む側：Reader が使う。動いている間は、標準エラーへ出す設定のときだけ使う）。

#include <stdint.h>

#include <string>
#include <string_view>

#include "ECOCppLogger/Record.h"

namespace lg::detail {

void appendJsonString(std::string& out, std::string_view text);
void appendDouble(std::string& out, double value);
void appendUnsigned(std::string& out, uint64_t value);
void appendSigned(std::string& out, int64_t value);

// UNIX 時刻（ナノ秒）→ "2026-11-05T21:03:04.123456+09:00"（マイクロ秒まで）。
void appendTimestamp(std::string& out, int64_t unixNanoseconds, int utcOffsetMinutes);

// "void __cdecl Game::Player::update(float)" → "Game::Player::update"
std::string_view shortFunctionName(std::string_view function) noexcept;

// フルパス → ファイル名（fullPath なら そのまま）。
std::string_view sourceFileName(std::string_view file, bool fullPath) noexcept;

int localUtcOffsetMinutes();

// 書式と引数のバイト列 → 本文。argsJson を渡すと、名前付きの値があれば {"名前":値,…} を足す（なければ空のまま）。
void renderMessage(std::string& message, std::string* argsJson, const Site& site, const unsigned char* arguments);

// ticks → 時刻の換算。
struct TimeBase {
    uint64_t tick = 0;
    int64_t unixNanoseconds = 0;
    double ticksPerNanosecond = 1.0;
    int utcOffsetMinutes = 0;

    int64_t toUnixNanoseconds(uint64_t value) const noexcept {
        const double delta = static_cast<double>(static_cast<int64_t>(value - tick));
        return unixNanoseconds + static_cast<int64_t>(delta / ticksPerNanosecond);
    }
};

enum class OutputFormat {
    Json,  // LLM 向け：JSON Lines
    Text,  // 人向け：1行の文章
};

class LineFormatter {
public:
    LineFormatter(OutputFormat format, bool fullPath) : format_(format), fullPath_(fullPath) {}

    void setTimeBase(const TimeBase& timeBase) {
        timeBase_ = timeBase;
        cachedPrefix_.clear();
    }
    const TimeBase& timeBase() const noexcept { return timeBase_; }

    // ログ1件（tick が 0 なら時刻なし、frame が 0 ならフレームなし）→ 1行（改行つき）。
    void appendRecord(std::string& out, uint64_t tick, uint64_t frame, uint64_t aux, const Site& site,
                      const unsigned char* arguments, std::string_view thread);

    // めったにないもの（集計・始め・終わり 等）。body は "t" を除く JSON の中身（"lv":"…",…）。
    void appendEvent(std::string& out, uint64_t tick, std::string_view body);

private:
    void appendTime(std::string& out, uint64_t tick);
    void appendTextPrefix(std::string& out, uint64_t tick, uint64_t frame, const Site& site, std::string_view thread);
    void appendJsonPrefix(std::string& out, uint64_t tick, uint64_t frame, const Site& site, std::string_view thread);

    OutputFormat format_;
    bool fullPath_;
    TimeBase timeBase_;
    std::string message_;
    std::string args_;
    std::string source_;
    int64_t cachedSecond_ = 0;
    std::string cachedPrefix_;
    std::string cachedSuffix_;
};

}  // namespace lg::detail
