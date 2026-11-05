#pragma once

// .ecolog ファイルの形（書く側：Logger.cpp、読む側：Reader.cpp）。
//
// 動いている間は文字列にせず、スレッドのリングバッファの中身をほぼそのまま書き写す。見るときに Reader が
// 人向けの文章・LLM 向けの JSON Lines に直す。
//
//   ファイル = 頭（Magic・版） + フレームの並び
//   フレーム = FrameHeader（種類・中身の大きさ） + 中身
//
//   Clock    ：時刻の換算（ticks → UNIX 時刻）。始めと、集計のたび・落ちたときに書く（最後のものが一番正確）
//   SiteDef  ：呼び出し位置（Site のアドレス → 書式・ファイル・行・関数・引数の型）。ファイルで初めて出たときに書く
//   ThreadDef：スレッドの番号 → 名前
//   Block    ：あるスレッドのリングバッファの中身（RecordHeader＋引数 の並び。詰め物を含むことがある）そのまま
//   Event    ：めったにないもの（集計・最近の値・捨てた数・落ちた・始め・終わり）。時刻＋JSON の中身（"t" を除く）

#include <cstdint>

namespace eco::log::detail::file {

inline constexpr char kMagic[8] = {'E', 'C', 'O', 'L', 'O', 'G', '\0', '\1'};
inline constexpr std::uint32_t kVersion = 1;

enum class FrameType : std::uint32_t {
    Clock = 1,
    SiteDef = 2,
    ThreadDef = 3,
    Block = 4,
    Event = 5,
};

struct FrameHeader {
    FrameType type;
    std::uint32_t size;  // 中身のバイト数（FrameHeader を含まない）
};

struct ClockPayload {
    std::uint64_t tick;
    std::int64_t unixNanoseconds;
    double ticksPerNanosecond;
    std::int32_t utcOffsetMinutes;
    std::uint32_t reserved;
};

// SiteDef の中身：SiteDefPayload、引数の型（argCount バイト）、書式・ファイル・関数（それぞれ uint32 の長さ＋バイト列）
struct SiteDefPayload {
    std::uint64_t address;
    std::uint64_t samplingValue;
    std::uint32_t line;
    std::uint8_t level;
    std::uint8_t sampling;
    std::uint8_t argCount;
    std::uint8_t reserved;
};

// ThreadDef の中身：uint32 の番号＋名前のバイト列
// Block の中身：uint32 のスレッドの番号＋uint32（予備）＋リングバッファのバイト列
// Event の中身：uint64 の tick＋JSON の中身のバイト列（例 "lv":"stat","stat":"x",...）

}  // namespace eco::log::detail::file
