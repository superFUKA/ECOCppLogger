#pragma once

// 使う側の関数（名前空間 lg のふつうの関数。中の状態は1つだけ持つ）と、マクロから呼ぶログを書く側の道。
//
// ログを書く側：重さの確認 →（時刻を読む）→ 自分のスレッドのバッファに 頭8B＋引数 をコピー → 書いた位置を公開。
// 文字列は作らない。バッファが一杯なら待たずに捨てて数える。
// ファイルへの書き出しは lg::writeOut() を呼んだスレッドが行う（裏のスレッドはない）。

#include <stddef.h>
#include <stdint.h>

#include <string.h>

#include <atomic>
#include <string_view>
#include <type_traits>

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

// ---- 間引き ---------------------------------------------------------------------------------------------

// EVERY_N：この呼び出し位置を、このスレッドで通った回数（0 から）。数は自分のスレッドのバッファの中に持つ。
inline uint64_t countPass(const Site& site, SiteIdSlot& slot) noexcept {
    const uint32_t id = siteId(site, slot);
    return id != 0 ? threadBuffer()->siteState(id).count++ : 0;
}

// FIRST_N：最初の n 回（すべてのスレッドで合わせて）。n 回を過ぎた後は読むだけ。
struct FirstCounter {
    std::atomic<uint64_t> passes{0};

    // 書くなら 1 から数えた回数、書かないなら 0。
    uint64_t next(uint64_t n) noexcept {
        if (passes.load(std::memory_order_relaxed) >= n) {
            return 0;
        }
        const uint64_t pass = passes.fetch_add(1, std::memory_order_relaxed);
        return pass < n ? pass + 1 : 0;
    }
};

// EVERY_MS：ms ミリ秒に1回まで（すべてのスレッドで合わせて）。省いた回数を次に書くときに渡す。
struct RateLimiter {
    std::atomic<uint64_t> nextTick{0};
    std::atomic<uint64_t> skipped{0};

    bool allow(uint64_t milliseconds, uint64_t& skippedBefore) noexcept {
        const uint64_t now = ticks();
        uint64_t next = nextTick.load(std::memory_order_relaxed);
        if (now >= next &&
            nextTick.compare_exchange_strong(
                next, now + static_cast<uint64_t>(static_cast<double>(milliseconds) * ticksPerMillisecond()),
                std::memory_order_relaxed)) {
            skippedBefore = skipped.exchange(0, std::memory_order_relaxed);
            return true;
        }
        skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
};

// ON_CHANGE の値：数値・bool・char・enum・ポインター（8 バイトまで、比べるのはビット）。
template <class T>
inline auto changeValue(const T& value) noexcept {
    const auto normalized = normalize(value);
    using N = decltype(normalized);
    static_assert(std::is_trivially_copyable_v<N> && sizeof(N) <= 8 && !std::is_same_v<N, std::string_view>,
                  "ECOCppLogger: ECO_LOG_ON_CHANGE の値は 数値・bool・char・enum・ポインター だけです");
    return normalized;
}

// ON_CHANGE：前の値（呼び出し位置・スレッドごと）と比べる。書くなら true（change は 0＝最初の値、1〜＝何回目の変化）。
template <class N>
inline bool observeChange(const Site& site, SiteIdSlot& slot, const N& value, uint64_t& change, N& previous) noexcept {
    const uint32_t id = siteId(site, slot);
    if (id == 0) [[unlikely]] {
        return false;
    }
    ThreadBuffer::SiteState& state = threadBuffer()->siteState(id);
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(N));
    if (state.count == 0) {
        state.count = 1;
        state.value = bits;
        change = 0;
        return true;
    }
    if (state.value == bits) {
        return false;
    }
    memcpy(&previous, &state.value, sizeof(N));
    state.value = bits;
    change = state.count++;
    return true;
}

// ---- まとめ書き ------------------------------------------------------------------------------------------

// 塊を1件として書く：頭［＋始めの時刻］＋BatchInfo＋値の並び。
inline void writeBatch(const Site& site, SiteIdSlot& slot, uint64_t startTick, uint32_t count, uint32_t elementSize,
                       const unsigned char* elements) noexcept {
    ThreadBuffer* buffer = threadBuffer();
    const uint32_t id = siteId(site, slot);
    const bool withTime = startTick != 0;
    const size_t bytes = static_cast<size_t>(count) * elementSize;
    const size_t size = alignRecord(sizeof(RecordHeader) + (withTime ? 8 : 0) + sizeof(BatchInfo) + bytes);
    unsigned char* out = size <= kMaxRecordBytes && id != 0 ? buffer->reserve(size, site.level >= Level::Warn) : nullptr;
    if (!out) [[unlikely]] {
        buffer->countDrop();
        return;
    }
    const RecordHeader header{static_cast<uint16_t>(size), RecordKind::Batch,
                              static_cast<uint8_t>(withTime ? kRecordHasTick : 0), id};
    memcpy(out, &header, sizeof(header));
    unsigned char* cursor = out + sizeof(RecordHeader);
    if (withTime) {
        memcpy(cursor, &startTick, 8);
        cursor += 8;
    }
    const BatchInfo info{count, elementSize, withTime ? ticks() : 0};
    memcpy(cursor, &info, sizeof(info));
    cursor += sizeof(info);
    memcpy(cursor, elements, bytes);
    buffer->commit();
}

}  // namespace detail

// まとめ書き（ECO_LOG_BATCH が作る）。同じ書式のログが続くループ用に、1件ごとには頭も時刻も付けず、値だけを
// 手元（スタック）に元の大きさで並べる。一杯になったとき・flush()・スコープの終わりに、まとめて1件として書く。
template <class... Types>
class Batch {
    static_assert(sizeof...(Types) > 0, "ECOCppLogger: ECO_LOG_BATCH には値の型を1つ以上書いてください");

public:
    static constexpr size_t kElementSize = detail::BatchTags<Types...>::elementSize;
    static constexpr size_t kScratchBytes = 8192;
    static constexpr uint32_t kCapacity = static_cast<uint32_t>(kScratchBytes / kElementSize > 0 ? kScratchBytes / kElementSize : 1);

    Batch(const Site& site, SiteIdSlot& id, bool compiled) noexcept
        : site_(site), id_(id), enabled_(compiled && detail::isEnabled(site.level)) {}
    ~Batch() { flush(); }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    // 1件足す（一杯かの確認＋値のコピーだけ）。
    void add(const Types&... values) noexcept {
        if (!enabled_) {
            return;
        }
        if (count_ == kCapacity) [[unlikely]] {
            flush();
        }
        if (count_ == 0) {
            startTick_ = detail::recordTime() ? ticks() : 0;
        }
        unsigned char* cursor = scratch_ + static_cast<size_t>(count_) * kElementSize;
        ((cursor = put(cursor, detail::batchValue(values))), ...);
        ++count_;
    }

    // 溜めた分を今すぐ書く。
    void flush() noexcept {
        if (count_ == 0) {
            return;
        }
        detail::writeBatch(site_, id_, startTick_, count_, static_cast<uint32_t>(kElementSize), scratch_);
        count_ = 0;
    }

    bool enabled() const noexcept { return enabled_; }
    uint32_t size() const noexcept { return count_; }

private:
    template <class V>
    static unsigned char* put(unsigned char* out, const V& value) noexcept {
        memcpy(out, &value, sizeof(V));
        return out + sizeof(V);
    }

    const Site& site_;
    SiteIdSlot& id_;
    bool enabled_;
    uint32_t count_ = 0;
    uint64_t startTick_ = 0;
    alignas(8) unsigned char scratch_[kCapacity * kElementSize];
};

// ECO_LOG_ENABLE=0 のときの ECO_LOG_BATCH（何もしない）。
struct NullBatch {
    template <class... Args>
    void add(const Args&...) const noexcept {}
    void flush() const noexcept {}
    bool enabled() const noexcept { return false; }
    uint32_t size() const noexcept { return 0; }
};

}  // namespace lg
