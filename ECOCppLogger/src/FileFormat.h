#pragma once

// .ecolog ファイルの形（書く側：Writer、読む側：Reader）。
//
// 動いている間は文字列にせず、スレッドのバッファの中身をそのまま書き写す。見るときに Reader が
// 人向けの文章・LLM 向けの JSON Lines に直す。
//
//   ファイル = kMagic（8）＋ kVersion（4）＋ フレームの並び
//   フレーム = FrameHeader（種類・中身の大きさ）＋ 中身
//
//   Clock    ：時刻の換算（ticks → UNIX 時刻）。始め・集計のたび・落ちたときに書く（読む側は最後のものを使う）
//   TypeDef  ：ログの型の番号（このファイルの中の連番）→ 名前。その型が初めて出たときに書く
//   SiteDef  ：呼び出し位置の番号 → 重さ・型・書式・ファイル・行・関数・引数の形。ファイルで初めて出たときに書く
//   ThreadDef：スレッドの番号 → OS のスレッド ID・名前（名前が変わったら書き直す）
//   Block    ：あるスレッドのバッファの中身（記録の並び）をそのまま
//   Event    ：めったにないもの（集計・始め・終わり・落ちた・捨てた 等）。時刻＋JSON の中身（"t" を除く）

#include <stdint.h>

namespace lg::detail::file {

inline constexpr char kMagic[8] = {'E', 'C', 'O', 'L', 'O', 'G', '\0', '\1'};
inline constexpr uint32_t kVersion = 1;

enum class FrameType : uint32_t {
    Clock = 1,
    TypeDef = 2,
    SiteDef = 3,
    ThreadDef = 4,
    Block = 5,
    Event = 6,
};

struct FrameHeader {
    FrameType type;
    uint32_t size;  // 中身のバイト数（FrameHeader を含まない）
};

struct ClockPayload {
    uint64_t tick;
    int64_t unixNanoseconds;
    double ticksPerNanosecond;
    int32_t utcOffsetMinutes;
    uint32_t reserved;
};

// TypeDef の中身：uint32 の番号＋名前のバイト列

// SiteDef の中身：SiteDefPayload、引数の形（tagCount バイト）、書式・ファイル・関数、
// ECO_LOG_FIELDS の型の項目名（structCount 個）（文字列はそれぞれ uint32 の長さ＋バイト列）
struct SiteDefPayload {
    uint32_t id;
    uint32_t line;
    uint64_t samplingValue;
    uint32_t type;  // TypeDef の番号
    uint8_t level;
    uint8_t sampling;
    uint8_t argCount;
    uint8_t structCount;
    uint16_t tagCount;
    uint16_t reserved;
    uint32_t reserved2;
};

// ThreadDef の中身：ThreadDefPayload＋名前のバイト列
struct ThreadDefPayload {
    uint32_t index;
    uint32_t reserved;
    uint64_t osThreadId;
};

// Block の中身：BlockPayload＋記録の並び
struct BlockPayload {
    uint32_t thread;
    uint32_t reserved;
    uint64_t drainTick;  // 書き写した時刻（時刻のない記録を並べるのに使う）
};

// Event の中身：uint64 の時刻＋JSON の中身のバイト列

}  // namespace lg::detail::file
