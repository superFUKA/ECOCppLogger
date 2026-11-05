#pragma once

// スレッドごとの書き込み先：記録のリングバッファ（書くのは持ち主のスレッド1つ、読むのは裏のスレッド1つ）と、
// 集計の置き場所（StatSlot）。どちらもホットパスではロックも共有の書き込みもしない。

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ECOCppLogger/Record.h"

namespace eco::log {

// 最近の値（ECO_HISTORY）。書くのは持ち主のスレッドだけで、ファイルには書かずにメモリーに置いておく。
// 裏のスレッド・落ちたときの書き出しは、書いている途中の1件を見分けるため seqlock の形で読む。
struct HistoryEntry {
    std::atomic<std::uint64_t> sequence{0};  // 0 は書いている途中、それ以外は 何件目か＋1
    std::atomic<double> value{0.0};
    std::atomic<std::uint64_t> tick{0};
    std::atomic<std::uint64_t> frame{0};
};

class HistoryRing {
public:
    explicit HistoryRing(std::size_t capacity);
    ~HistoryRing();
    HistoryRing(const HistoryRing&) = delete;
    HistoryRing& operator=(const HistoryRing&) = delete;

    void push(double value, std::uint64_t tick, std::uint64_t frame) noexcept {
        const std::uint64_t n = written_.load(std::memory_order_relaxed);
        HistoryEntry& entry = entries_[n & mask_];
        entry.sequence.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        entry.value.store(value, std::memory_order_relaxed);
        entry.tick.store(tick, std::memory_order_relaxed);
        entry.frame.store(frame, std::memory_order_relaxed);
        entry.sequence.store(n + 1, std::memory_order_release);
        written_.store(n + 1, std::memory_order_release);
    }

    struct Point {
        double value;
        std::uint64_t tick;
        std::uint64_t frame;
    };

    // 古い順に、読めた値を callback(Point) に渡す（書いている途中・上書きされたものは飛ばす）。
    template <class Callback>
    void read(Callback&& callback) const {
        const std::uint64_t written = written_.load(std::memory_order_acquire);
        const std::uint64_t capacity = mask_ + 1;
        for (std::uint64_t n = written > capacity ? written - capacity : 0; n < written; ++n) {
            const HistoryEntry& entry = entries_[n & mask_];
            const std::uint64_t before = entry.sequence.load(std::memory_order_acquire);
            Point point{entry.value.load(std::memory_order_relaxed), entry.tick.load(std::memory_order_relaxed),
                        entry.frame.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == n + 1 && entry.sequence.load(std::memory_order_relaxed) == before) {
                callback(point);
            }
        }
    }

private:
    std::atomic<std::uint64_t> written_{0};
    std::uint64_t mask_;
    HistoryEntry* entries_;
};

// 集計の1件。書くのは持ち主のスレッドだけ（読み・書きを分けた relaxed の load/store ＝ x64 ではただの mov）。
// 裏のスレッドは読むだけで、回数・合計は前に出した値との差を、最小・最大は期間（epoch）ごとに出す。
struct StatSlot {
    std::atomic<const StatSite*> site{nullptr};
    std::atomic<std::uint64_t> count{0};
    std::atomic<double> sum{0.0};
    std::atomic<double> last{0.0};
    std::atomic<std::uint64_t> epoch{0};  // 最小・最大を最後に書いた期間
    std::atomic<double> minimum[2]{};     // 期間の偶奇で使い分ける
    std::atomic<double> maximum[2]{};

    // 裏のスレッドだけが使う（前に出したときの値）。
    std::uint64_t reportedCount = 0;
    double reportedSum = 0.0;

    // ECO_HISTORY のときだけ、持ち主のスレッドが最初に使うときに作る。
    std::atomic<HistoryRing*> history{nullptr};

    StatSlot() = default;
    StatSlot(const StatSlot&) = delete;
    StatSlot& operator=(const StatSlot&) = delete;
    ~StatSlot() { delete history.load(std::memory_order_relaxed); }

    void add(double value, std::uint64_t currentEpoch) noexcept {
        const std::uint64_t n = count.load(std::memory_order_relaxed);
        count.store(n + 1, std::memory_order_relaxed);
        sum.store(sum.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
        last.store(value, std::memory_order_relaxed);
        const std::size_t parity = currentEpoch & 1;
        if (epoch.load(std::memory_order_relaxed) != currentEpoch) {
            minimum[parity].store(value, std::memory_order_relaxed);
            maximum[parity].store(value, std::memory_order_relaxed);
            epoch.store(currentEpoch, std::memory_order_release);
            return;
        }
        if (value < minimum[parity].load(std::memory_order_relaxed)) {
            minimum[parity].store(value, std::memory_order_relaxed);
        }
        if (value > maximum[parity].load(std::memory_order_relaxed)) {
            maximum[parity].store(value, std::memory_order_relaxed);
        }
    }
};

inline constexpr std::size_t kStatChunkSlots = 256;
inline constexpr std::size_t kStatChunks = 64;  // 集計の呼び出し位置は最大 16384 か所

struct StatChunk {
    StatSlot slots[kStatChunkSlots];
};

class ThreadBuffer {
public:
    ThreadBuffer(std::size_t capacityBytes, std::uint32_t index);
    ~ThreadBuffer();
    ThreadBuffer(const ThreadBuffer&) = delete;
    ThreadBuffer& operator=(const ThreadBuffer&) = delete;

    // ---- 持ち主のスレッドが使う -------------------------------------------------------------------

    // size バイト（8 の倍数）を取る。空きがなければ nullptr（呼んだ側は捨てて dropped を数える）。
    std::byte* reserve(std::size_t size) noexcept {
        if (size > capacity_ / 2) [[unlikely]] {
            return nullptr;
        }
        const std::uint64_t write = localWrite_;
        const std::size_t offset = static_cast<std::size_t>(write & mask_);
        const std::size_t contiguous = capacity_ - offset;
        const std::size_t needed = size <= contiguous ? size : contiguous + size;
        if (write + needed - cachedRead_ > capacity_) {
            cachedRead_ = read_.load(std::memory_order_acquire);
            if (write + needed - cachedRead_ > capacity_) {
                return nullptr;
            }
        }
        if (size > contiguous) {
            // 終わりまでの残りを詰め物にして、先頭から書く。
            auto* padding = reinterpret_cast<RecordHeader*>(data_ + offset);
            *padding = RecordHeader{static_cast<std::uint16_t>(contiguous), RecordKind::Padding, 0, 0};
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

    StatSlot* statSlot(std::uint32_t index) noexcept {
        const std::size_t chunkIndex = index / kStatChunkSlots;
        if (chunkIndex >= kStatChunks) {
            return nullptr;
        }
        StatChunk* chunk = chunks_[chunkIndex].load(std::memory_order_relaxed);
        if (!chunk) [[unlikely]] {
            chunk = allocateChunk(chunkIndex);
        }
        return &chunk->slots[index % kStatChunkSlots];
    }

    // ---- 裏のスレッドが使う -----------------------------------------------------------------------

    // まだ読んでいない記録を順に callback(const RecordHeader&) に渡す。読んだ件数を返す。
    template <class Callback>
    std::size_t consume(Callback&& callback) {
        std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        std::size_t consumed = 0;
        while (read < write) {
            const auto* header = reinterpret_cast<const RecordHeader*>(data_ + (read & mask_));
            if (header->kind != RecordKind::Padding) {
                callback(*header);
                ++consumed;
            }
            read += header->size;
        }
        read_.store(read, std::memory_order_release);
        return consumed;
    }

    // まだ読んでいない部分を、続いているバイト列ごとに callback(data, size) に渡す（多くて2回：終わりで折り返すため）。
    // 中身は記録（RecordHeader＋引数）の並びで、詰め物を含むことがある。ファイルへそのまま書き写すために使う。
    template <class Callback>
    std::size_t consumeSpans(Callback&& callback) {
        std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        const std::size_t total = static_cast<std::size_t>(write - read);
        while (read < write) {
            const std::size_t offset = static_cast<std::size_t>(read & mask_);
            const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(write - read, capacity_ - offset));
            callback(static_cast<const std::byte*>(data_ + offset), length);
            read += length;
        }
        read_.store(read, std::memory_order_release);
        return total;
    }

    bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

    template <class Callback>
    void forEachStat(Callback&& callback) {
        for (auto& entry : chunks_) {
            StatChunk* chunk = entry.load(std::memory_order_acquire);
            if (!chunk) {
                continue;
            }
            for (StatSlot& slot : chunk->slots) {
                if (slot.site.load(std::memory_order_acquire)) {
                    callback(slot);
                }
            }
        }
    }

    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t reportedDropped = 0;  // 裏のスレッドだけが使う

    std::uint32_t index() const noexcept { return index_; }
    std::size_t capacity() const noexcept { return capacity_; }

    // スレッドが終わったか（終わったら、裏のスレッドが読み切ってから片付ける）。
    std::atomic<bool> retired{false};

    std::string name;  // Registry の mutex の下で読み書きする

private:
    StatChunk* allocateChunk(std::size_t chunkIndex);

    // 持ち主のスレッドが書く側
    alignas(64) std::atomic<std::uint64_t> write_{0};
    std::uint64_t localWrite_ = 0;
    std::uint64_t pendingWrite_ = 0;
    std::uint64_t cachedRead_ = 0;
    std::atomic<std::uint64_t> dropped_{0};

    // 裏のスレッドが書く側（別のキャッシュラインに置き、書き合いを避ける）
    alignas(64) std::atomic<std::uint64_t> read_{0};

    alignas(64) std::byte* data_;
    std::size_t capacity_;
    std::uint64_t mask_;
    std::uint32_t index_;
    std::atomic<StatChunk*> chunks_[kStatChunks]{};
};

// すべてのスレッドの ThreadBuffer。プロセスの終わりまで消さない（終了処理・クラッシュ中に使うため）。
class Registry {
public:
    static Registry& instance();

    ThreadBuffer* create(std::size_t capacityBytes);

    struct Entry {
        ThreadBuffer* buffer;
        std::string name;
    };

    // 今あるバッファの一覧と名前（裏のスレッド・クラッシュ時の書き出しが使う）。
    // emergency なら、ロックを取れなくても待たずに読む（落ちたスレッドがロックを持ったままかもしれないため）。
    std::vector<Entry> snapshot(bool emergency = false);

    void setName(ThreadBuffer* buffer, std::string_view name);

    // 終わったスレッドで、読み切ったバッファを片付ける。
    void removeRetired(const std::vector<ThreadBuffer*>& done);

    std::uint32_t registerStat(StatSite& site) noexcept;

    // ログの呼び出し位置に番号を振る（最初の1回だけ）。番号 → Site は裏のスレッド・読む側の対応表に使う。
    std::uint32_t registerSite(const Site& site, SiteIdSlot& slot) noexcept;
    const Site* siteById(std::uint32_t id) const noexcept;

    static constexpr std::size_t kSiteChunkSize = 1024;
    static constexpr std::size_t kSiteChunks = 1024;  // 呼び出し位置は最大 約100万か所

    std::mutex mutex;

private:
    std::vector<std::unique_ptr<ThreadBuffer>> buffers_;
    std::uint32_t nextThreadIndex_ = 0;
    std::atomic<std::uint32_t> nextStatIndex_{1};  // 0 は「まだ振っていない」
    std::mutex siteMutex_;
    std::uint32_t nextSiteId_ = 1;
    std::atomic<std::atomic<const Site*>*> siteChunks_[kSiteChunks]{};
};

namespace detail {

// 呼んだスレッドのバッファ。最初の1回だけ作って登録する（遅い道）。
inline thread_local ThreadBuffer* t_buffer = nullptr;
ThreadBuffer* createThreadBuffer();

// これから作るスレッドのバッファの大きさ（start() の Config から）。
void setThreadBufferBytes(std::size_t bytes) noexcept;

inline ThreadBuffer* threadBuffer() noexcept {
    ThreadBuffer* buffer = t_buffer;
    if (!buffer) [[unlikely]] {
        buffer = createThreadBuffer();
    }
    return buffer;
}

}  // namespace detail

}  // namespace eco::log
