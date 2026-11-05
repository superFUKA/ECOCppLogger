#pragma once

// 記録を文字列にする（見るとき：Reader が使う。動いている間は、標準エラーへ出す設定のときだけ使う）。

#include <cstdint>
#include <string>
#include <string_view>

#include "ECOCppLogger/Record.h"

namespace eco::log::detail {

void appendJsonString(std::string& out, std::string_view text);
void appendDouble(std::string& out, double value);
void appendUnsigned(std::string& out, std::uint64_t value);
void appendSigned(std::string& out, std::int64_t value);

// UNIX 時刻（ナノ秒）→ "2026-11-05T21:03:04.123456+09:00"（マイクロ秒まで）。
void appendTimestamp(std::string& out, std::int64_t unixNanoseconds, int utcOffsetMinutes);

// "void __cdecl Game::Player::update(float)" → "Game::Player::update"
std::string_view shortFunctionName(std::string_view function) noexcept;

// フルパス → ファイル名（fullPath なら そのまま）。
std::string_view sourceFileName(std::string_view file, bool fullPath) noexcept;

int localUtcOffsetMinutes();

// 書式と、記録の引数のバイト列 → 本文。
void renderMessage(std::string& out, const Site& site, const std::byte* arguments);

// ticks → 時刻の換算。
struct TimeBase {
    std::uint64_t tick = 0;
    std::int64_t unixNanoseconds = 0;
    double ticksPerNanosecond = 1.0;
    int utcOffsetMinutes = 0;

    std::int64_t toUnixNanoseconds(std::uint64_t value) const noexcept {
        const double delta = static_cast<double>(static_cast<std::int64_t>(value - tick));
        return unixNanoseconds + static_cast<std::int64_t>(delta / ticksPerNanosecond);
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

    // 記録1件（tick が 0 なら時刻を記録しない設定、frame が 0 ならフレームなし）→ 1行（改行つき）。
    void appendRecord(std::string& out, std::uint64_t tick, std::uint64_t frame, std::uint64_t aux, const Site& site,
                      const std::byte* arguments, std::string_view thread);

    // まとめ書きの塊 → JSON は1行（rows に1件ずつの値）、文章は頭の行＋1件ずつの行（maxRows まで。0 なら全部）。
    void appendBatch(std::string& out, std::uint64_t tick, std::uint64_t frame, const Site& site, const BatchInfo& info,
                     const std::byte* elements, std::string_view thread, std::size_t maxRows);

    // めったにないもの（集計・最近の値 等）。body は "t" を除く JSON の中身。
    void appendEvent(std::string& out, std::uint64_t tick, std::string_view body);

private:
    void appendTime(std::string& out, std::uint64_t tick);

    OutputFormat format_;
    bool fullPath_;
    TimeBase timeBase_;
    std::string message_;
    std::string source_;
    std::int64_t cachedSecond_ = 0;
    std::string cachedPrefix_;
    std::string cachedSuffix_;
};

}  // namespace eco::log::detail
