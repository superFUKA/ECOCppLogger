#pragma once

// スレッドごとのバッファ：記録のリングバッファ（書くのは持ち主のスレッド1つ、読むのは writeOut() を呼んだスレッド1つ）。
// ログを書く側はロックも共有の書き込みもしない。
//
// 【広げる】一杯になったら（writeOut() を待たずに）倍の大きさのリングを malloc で新しく取り、続きをそこへ書く（上限まで）。
// 古いリングは書き出す側が読み切ってから解放する。位置（write_・read_）はリングをまたいで続く数で、新しいリングは古いリングの
// 終わりの位置から始まる。速い道（reserve の空きがあるとき）は今までと同じで、広げるのは遅い道（reserveSlow）だけ。
// 上限まで広げても入らなければ、今までと同じく捨てて数える。
//
// 【ファイバー】ファイバーは中断の前後で別のスレッドへ移る。thread_local の番地をコンパイラが使い回すと、古いスレッドの
// バッファに書いて壊すので、自分のスレッドのバッファは TLS スロットを gs から毎回直接読む（threadBuffer()）。
// 測定：今までのインラインの thread_local と同じ速さ（非インライン関数で読むと約 +2.3ns）。

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "ECOCppLogger/Record.h"
#include "ECOCppLogger/Stat.h"

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#define ECO_LOG_TLS_SLOT 1
#else
#define ECO_LOG_TLS_SLOT 0
#endif

namespace lg {

class ThreadBuffer {
public:
    // capacityBytes：初めの大きさ。maxCapacityBytes：広げてよい上限（初めの大きさ以下なら広げない）。
    // どちらも 2 のべき乗に切り上げる（上限は、それを超えない 2 のべき乗まで広げる）。
    ThreadBuffer(size_t capacityBytes, uint32_t reservePercent, uint32_t index, uint64_t osThreadId,
                 size_t maxCapacityBytes = 0);
    ~ThreadBuffer();
    ThreadBuffer(const ThreadBuffer&) = delete;
    ThreadBuffer& operator=(const ThreadBuffer&) = delete;

    // ---- 持ち主のスレッドが使う -------------------------------------------------------------------------

    // size バイト（8 の倍数）を取る。空きがなければ広げ、上限でも入らなければ nullptr（呼んだ側は捨てて数える）。
    // important でない記録は、空きが「取っておく分」を切ったら（広げられなければ）取らない（大事なログを押し出さないため）。
    unsigned char* reserve(size_t size, bool important) noexcept {
        const uint64_t write = localWrite_;
        const size_t offset = static_cast<size_t>(write & mask_);
        const size_t contiguous = capacity_ - offset;
        const size_t needed = size <= contiguous ? size : contiguous + size;
        const uint64_t limit = important ? capacity_ : normalLimit_;
        if (size > capacity_ / 2 || write + needed - cachedRead_ > limit) [[unlikely]] {
            return reserveSlow(size, important);
        }
        return place(write, offset, contiguous, size);
    }

    void commit() noexcept {
        localWrite_ = pendingWrite_;
        write_.store(pendingWrite_, std::memory_order_release);
    }

    void countDrop() noexcept {
        dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    // 呼び出し位置ごと・スレッドごとの小さな状態（EVERY_N の回数・ON_CHANGE の前の値）。持ち主のスレッドだけが使う。
    // 共有の atomic で数えると、スレッド間でキャッシュラインを取り合う（研究の測定：約 6ns → 約 1.3ns）。
    struct SiteState {
        uint64_t value;
        uint64_t count;
    };
    static constexpr uint32_t kStateChunkSize = 1024;
    static constexpr uint32_t kStateChunks = 1024;

    SiteState& siteState(uint32_t site) noexcept {
        SiteState* chunk = stateChunks_[(site / kStateChunkSize) % kStateChunks];
        if (!chunk) [[unlikely]] {
            chunk = createStateChunk(site);
        }
        return chunk[site % kStateChunkSize];
    }

    // 集計の置き場所（番号は台帳が振る。1〜）。取れなければ nullptr（その値は数えない）。
    StatSlot* statSlot(uint32_t index) noexcept {
        const uint32_t chunkIndex = index / kStatChunkSlots;
        if (chunkIndex >= kStatChunks) [[unlikely]] {
            return nullptr;
        }
        StatChunk* chunk = statChunks_[chunkIndex].load(std::memory_order_relaxed);
        if (!chunk) [[unlikely]] {
            chunk = createStatChunk(chunkIndex);
            if (!chunk) return nullptr;
        }
        return &chunk->slots[index % kStatChunkSlots];
    }

    // ---- 書き出す側が使う ------------------------------------------------------------------------------

    // まだ読んでいない部分を、続いているバイト列ごとに callback(data, size) に渡す（リングごとに多くて2回：終わりで
    // 折り返すため）。中身は記録の並び（詰め物を含むことがある）。読み切った古いリングはここで解放する。
    template <class Callback>
    size_t consume(Callback&& callback) {
        uint64_t read = read_.load(std::memory_order_relaxed);
        const uint64_t start = read;
        for (;;) {
            // write_ を先に読む：write_ が新しいリングの位置なら、next はもう見える（持ち主は next を先に書く）。
            const uint64_t write = write_.load(std::memory_order_acquire);
            Segment* const segment = readSegment_;
            Segment* const next = segment->next.load(std::memory_order_acquire);
            const uint64_t end = next ? segment->end : write;
            while (read < end) {
                const size_t offset = static_cast<size_t>(read & segment->mask);
                const uint64_t left = end - read;
                const size_t room = segment->capacity - offset;
                const size_t length = static_cast<size_t>(left < room ? left : room);
                callback(static_cast<const unsigned char*>(segment->data + offset), length);
                read += length;
            }
            if (!next) {
                break;
            }
            readSegment_ = next;
            freeSegment(segment);
        }
        read_.store(read, std::memory_order_release);
        return static_cast<size_t>(read - start);
    }

    // 使われている集計すべてに callback(StatSlot&) を呼ぶ。
    template <class Callback>
    void forEachStat(Callback&& callback) {
        for (auto& entry : statChunks_) {
            StatChunk* chunk = entry.load(std::memory_order_acquire);
            if (!chunk) continue;
            for (StatSlot& slot : chunk->slots) {
                if (slot.site.load(std::memory_order_acquire)) callback(slot);
            }
        }
    }

    bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    uint64_t reportedDropped = 0;  // 書き出す側だけが使う

    // 今の大きさ（広げると増える。どのスレッドから読んでもよい）。
    size_t capacity() const noexcept { return publishedCapacity_.load(std::memory_order_relaxed); }
    size_t maxCapacity() const noexcept { return maxCapacity_; }
    size_t reportedCapacity = 0;  // 書き出す側だけが使う（書いた大きさ。初めの大きさは書かない）

    // ---- メタデータ（ファイルには1回だけ書く） ----
    uint32_t index() const noexcept { return index_; }
    uint64_t osThreadId() const noexcept { return osThreadId_; }

    std::atomic<bool> retired{false};  // スレッドが終わった（読み切ったら片付ける）

private:
    // リング1つ。頭と中身を1回の確保で取る（頭の後ろ 64 バイト目から中身）。
    struct Segment {
        unsigned char* data;
        size_t capacity;
        uint64_t mask;
        uint64_t end;                      // 次のリングへ移った位置（next を書く前に、持ち主が書く）
        std::atomic<Segment*> next{nullptr};
    };

    static Segment* createSegment(size_t capacity) noexcept;
    static void freeSegment(Segment* segment) noexcept;

    unsigned char* place(uint64_t write, size_t offset, size_t contiguous, size_t size) noexcept {
        if (size > contiguous) {
            // 終わりまでの残りを詰め物にして、先頭から書く。
            const RecordHeader padding{static_cast<uint16_t>(contiguous), RecordKind::Padding, 0, 0};
            memcpy(data_ + offset, &padding, sizeof(padding));
            pendingWrite_ = write + contiguous + size;
            return data_;
        }
        pendingWrite_ = write + size;
        return data_ + offset;
    }

    // 空きがないとき：書き出す側が読んだ位置を読み直し、それでもなければ広げる（非インライン）。
    unsigned char* reserveSlow(size_t size, bool important) noexcept;
    bool grow(size_t size, bool important) noexcept;
    void useSegment(Segment* segment) noexcept;

    SiteState* createStateChunk(uint32_t site) noexcept;

    StatChunk* createStatChunk(uint32_t chunkIndex) noexcept;

    SiteState* stateChunks_[kStateChunks] = {};
    std::atomic<StatChunk*> statChunks_[kStatChunks] = {};

    // 持ち主のスレッドが書く側
    alignas(64) std::atomic<uint64_t> write_{0};
    uint64_t localWrite_ = 0;
    uint64_t pendingWrite_ = 0;
    uint64_t cachedRead_ = 0;   // 今のリングの始まりより前にはしない
    uint64_t segmentBase_ = 0;  // 今のリングの始まりの位置
    std::atomic<uint64_t> dropped_{0};

    // 書き出す側が書く側（別のキャッシュラインに置き、書き合いを避ける）
    alignas(64) std::atomic<uint64_t> read_{0};
    Segment* readSegment_ = nullptr;  // 書き出す側が読んでいるリング（一番古い）

    // 持ち主のスレッドが書いているリング（速い道で読むものは写しておく）
    alignas(64) unsigned char* data_ = nullptr;
    size_t capacity_ = 0;
    uint64_t mask_ = 0;
    uint64_t normalLimit_ = 0;  // important でない記録が使える量
    Segment* writeSegment_ = nullptr;
    size_t maxCapacity_ = 0;
    uint32_t reservePercent_ = 0;
    std::atomic<size_t> publishedCapacity_{0};
    uint32_t index_;
    uint64_t osThreadId_;
};

namespace detail {

// 自分のスレッドのバッファ（遅い道：初めてならここで作る。thread_local を非インラインで読む）。
ThreadBuffer* threadBufferSlow() noexcept;

#if ECO_LOG_TLS_SLOT
// TEB の TlsSlots の位置（gs からの位置。0 なら使えない）。起動時に1回だけ決まる。
extern uint32_t g_tlsSlotOffset;
#endif

// 自分のスレッドのバッファ。ファイバーが別のスレッドへ移っても、毎回今のスレッドのものを読む。
inline ThreadBuffer* threadBuffer() noexcept {
#if ECO_LOG_TLS_SLOT
    const uint32_t offset = g_tlsSlotOffset;
    if (offset != 0) [[likely]] {
        auto* buffer = reinterpret_cast<ThreadBuffer*>(__readgsqword(offset));
        if (buffer) [[likely]] {
            return buffer;
        }
    }
#endif
    return threadBufferSlow();
}

}  // namespace detail

}  // namespace lg
