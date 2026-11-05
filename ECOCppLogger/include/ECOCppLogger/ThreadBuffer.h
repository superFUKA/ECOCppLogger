#pragma once

// スレッドごとのバッファ：記録のリングバッファ（書くのは持ち主のスレッド1つ、読むのは writeOut() を呼んだスレッド1つ）。
// ログを書く側はロックも共有の書き込みもしない。
//
// 【ファイバー】ファイバーは中断の前後で別のスレッドへ移る。thread_local の番地をコンパイラが使い回すと、古いスレッドの
// バッファに書いて壊すので、自分のスレッドのバッファは TLS スロットを gs から毎回直接読む（threadBuffer()）。
// 測定：今までのインラインの thread_local と同じ速さ（非インライン関数で読むと約 +2.3ns）。

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "ECOCppLogger/Record.h"

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#define ECO_LOG_TLS_SLOT 1
#else
#define ECO_LOG_TLS_SLOT 0
#endif

namespace lg {

class ThreadBuffer {
public:
    ThreadBuffer(size_t capacityBytes, uint32_t reservePercent, uint32_t index, uint64_t osThreadId);
    ~ThreadBuffer();
    ThreadBuffer(const ThreadBuffer&) = delete;
    ThreadBuffer& operator=(const ThreadBuffer&) = delete;

    // ---- 持ち主のスレッドが使う -------------------------------------------------------------------------

    // size バイト（8 の倍数）を取る。空きがなければ nullptr（呼んだ側は捨てて数える）。
    // important でない記録は、空きが「取っておく分」を切ったら取らない（大事なログを押し出さないため）。
    unsigned char* reserve(size_t size, bool important) noexcept {
        if (size > capacity_ / 2) [[unlikely]] {
            return nullptr;
        }
        const uint64_t write = localWrite_;
        const size_t offset = static_cast<size_t>(write & mask_);
        const size_t contiguous = capacity_ - offset;
        const size_t needed = size <= contiguous ? size : contiguous + size;
        const uint64_t limit = important ? capacity_ : normalLimit_;
        if (write + needed - cachedRead_ > limit) {
            cachedRead_ = read_.load(std::memory_order_acquire);
            if (write + needed - cachedRead_ > limit) {
                return nullptr;
            }
        }
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

    // ---- 書き出す側が使う ------------------------------------------------------------------------------

    // まだ読んでいない部分を、続いているバイト列ごとに callback(data, size) に渡す（多くて2回：終わりで折り返すため）。
    // 中身は記録の並び（詰め物を含むことがある）。
    template <class Callback>
    size_t consume(Callback&& callback) {
        uint64_t read = read_.load(std::memory_order_relaxed);
        const uint64_t write = write_.load(std::memory_order_acquire);
        const size_t total = static_cast<size_t>(write - read);
        while (read < write) {
            const size_t offset = static_cast<size_t>(read & mask_);
            const uint64_t left = write - read;
            const size_t length = static_cast<size_t>(left < capacity_ - offset ? left : capacity_ - offset);
            callback(static_cast<const unsigned char*>(data_ + offset), length);
            read += length;
        }
        read_.store(read, std::memory_order_release);
        return total;
    }

    bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    uint64_t reportedDropped = 0;  // 書き出す側だけが使う

    // ---- メタデータ（ファイルには1回だけ書く） ----
    uint32_t index() const noexcept { return index_; }
    uint64_t osThreadId() const noexcept { return osThreadId_; }
    size_t capacity() const noexcept { return capacity_; }

    std::atomic<bool> retired{false};  // スレッドが終わった（読み切ったら片付ける）

private:
    SiteState* createStateChunk(uint32_t site) noexcept;

    SiteState* stateChunks_[kStateChunks] = {};

    // 持ち主のスレッドが書く側
    alignas(64) std::atomic<uint64_t> write_{0};
    uint64_t localWrite_ = 0;
    uint64_t pendingWrite_ = 0;
    uint64_t cachedRead_ = 0;
    std::atomic<uint64_t> dropped_{0};

    // 書き出す側が書く側（別のキャッシュラインに置き、書き合いを避ける）
    alignas(64) std::atomic<uint64_t> read_{0};

    alignas(64) unsigned char* data_;
    size_t capacity_;
    uint64_t mask_;
    uint64_t normalLimit_;  // important でない記録が使える量
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
