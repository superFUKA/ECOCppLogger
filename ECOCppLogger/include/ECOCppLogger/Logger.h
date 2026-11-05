#pragma once

// 使う側の関数（名前空間 lg のふつうの関数。中の状態は1つだけ持つ）と、マクロから呼ぶログを書く側の道。
//
// ログを書く側：重さの確認 →（時刻を読む）→ 自分のスレッドのバッファに 頭8B＋引数 をコピー → 書いた位置を公開。
// 文字列は作らない。バッファが一杯なら待たずに捨てて数える。
// ファイルへの書き出しは lg::writeOut() を呼んだスレッドが行う（裏のスレッドはない）。

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "ECOCppLogger/Clock.h"
#include "ECOCppLogger/Config.h"
#include "ECOCppLogger/Level.h"
#include "ECOCppLogger/Record.h"
#include "ECOCppLogger/ThreadBuffer.h"

// 0 にすると、ログの時刻をコンパイルで消す（Config::recordTime の確認も消える）。
#ifndef ECO_LOG_RECORD_TIME
#define ECO_LOG_RECORD_TIME 1
#endif

namespace lg {

// 始める。ファイルを開き、落ちたときの書き出しを入れる。start() の前のログはバッファに溜まり、次の writeOut() で書く。
bool start(const Config& config = {});

// 残りを全部書いて閉じる（終了時にも自動で呼ばれる）。
void stop();

// 書き出す：すべてのスレッドのバッファにたまったログを、呼んだスレッドでファイルへ書く（ゲームならループの終わりに1回）。
// 集計は statIntervalMs が来ていれば出す。何本のスレッドから呼んでもよい（中で1つずつ）。
void writeOut();

bool isRunning() noexcept;

// 今書いているファイル（UTF-8。destination を指定すればその書き出し先の）。次の start()・stop() まで有効。
const char* currentFile(const char* destination = nullptr);

void setLevel(Level level) noexcept;
Level level() noexcept;

// 呼んだスレッドの名前（出力の th）。
void setThreadName(const char* name);

// フレームの区切り（= ECO_FRAME()）。ここから後の記録に、読む側がフレーム番号を付ける。
void nextFrame() noexcept;
uint64_t frame() noexcept;

namespace detail {

extern std::atomic<uint8_t> g_level;
extern std::atomic<uint64_t> g_frame;
extern std::atomic<bool> g_recordTime;
extern std::atomic<bool> g_recordFrame;

uint32_t registerSite(const Site& site, SiteIdSlot& slot) noexcept;

inline bool isEnabled(Level level) noexcept {
    return static_cast<uint8_t>(level) >= g_level.load(std::memory_order_relaxed);
}

inline bool recordTime() noexcept {
#if ECO_LOG_RECORD_TIME
    return g_recordTime.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

inline uint32_t siteId(const Site& site, SiteIdSlot& slot) noexcept {
    uint32_t id = slot.load(std::memory_order_relaxed);
    if (id == 0) [[unlikely]] {
        id = registerSite(site, slot);
    }
    return id;
}

// 1件：頭（8）［＋時刻（8）］［＋間引き（8）］＋引数。
template <bool HasAux, class... Normalized>
inline void emitNormalized(const Site& site, SiteIdSlot& slot, uint64_t aux, const Normalized&... values) noexcept {
    ThreadBuffer* buffer = threadBuffer();
    const uint32_t id = siteId(site, slot);
    const bool withTime = recordTime();
    const size_t size = alignRecord(sizeof(RecordHeader) + (withTime ? 8 : 0) + (HasAux ? 8 : 0) +
                                    (size_t{0} + ... + encodedSize(values)));
    unsigned char* out = size <= kMaxRecordBytes && id != 0 ? buffer->reserve(size, site.level >= Level::Warn) : nullptr;
    if (!out) [[unlikely]] {
        buffer->countDrop();
        return;
    }
    const RecordHeader header{static_cast<uint16_t>(size), RecordKind::Log,
                              static_cast<uint8_t>((withTime ? kRecordHasTick : 0) | (HasAux ? kRecordHasAux : 0)), id};
    memcpy(out, &header, sizeof(header));
    unsigned char* cursor = out + sizeof(RecordHeader);
    if (withTime) {
        const uint64_t tick = ticks();
        memcpy(cursor, &tick, 8);
        cursor += 8;
    }
    if constexpr (HasAux) {
        memcpy(cursor, &aux, 8);
        cursor += 8;
    }
    ((cursor = encode(cursor, values)), ...);
    buffer->commit();
}

// マクロが作る呼び出し口：Emitter{site, id}(引数...)。引数がなくても () で呼べる。
struct Emitter {
    const Site& site;
    SiteIdSlot& id;

    template <class... Args>
    void operator()(const Args&... args) const noexcept {
        emitNormalized<false>(site, id, 0, normalize(args)...);
    }
};

// 間引くログ：間引きの情報（何回目か・省いた数）を付ける。
struct SampledEmitter {
    const Site& site;
    SiteIdSlot& id;
    uint64_t aux;

    template <class... Args>
    void operator()(const Args&... args) const noexcept {
        emitNormalized<true>(site, id, aux, normalize(args)...);
    }
};

}  // namespace detail

}  // namespace lg
