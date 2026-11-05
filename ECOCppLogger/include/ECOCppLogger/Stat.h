#pragma once

// 集計の置き場所：呼び出し位置ごとの静的な情報（StatSite）と、スレッドごとの値（StatSlot）・最近の値（HistoryRing）。
// 値を足すのは持ち主のスレッドだけ（relaxed の読み書き。ロックも共有の書き込みもしない）。読むのは writeOut() を呼んだスレッド。
// 最小・最大は「期間」の偶奇で2つ持ち、書き出す側が期間を進めた後で前の期間の分を読む。

#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace lg {

enum class StatKind : uint8_t {
    Value,  // ECO_STAT：値の 回数・平均・最小・最大・最後
    Count,  // ECO_STAT_COUNT：通った回数（1秒あたり）
    Time,   // ECO_STAT_TIME：スコープの時間（持つのは TSC の差、出すときにマイクロ秒にする）
};

// 呼び出し位置ごと（マクロの中で static constinit に作る）。index は最初に使うときに振る。
struct StatSite {
    const char* name;
    const char* file;
    uint32_t line;
    StatKind kind;
    uint32_t historyCapacity;  // ECO_STAT_HISTORY で覚える数（0 なら覚えない）
    std::atomic<uint32_t> index{0};
};

// 最近の値（スレッドごと）。古いものから上書きする。読む側は書いている途中の値を飛ばす（seqlock）。
class HistoryRing {
public:
    explicit HistoryRing(size_t capacity);
    ~HistoryRing();
    HistoryRing(const HistoryRing&) = delete;
    HistoryRing& operator=(const HistoryRing&) = delete;

    void push(double value, uint64_t tick, uint64_t frame) noexcept {
        const uint64_t n = written_.load(std::memory_order_relaxed);
        Entry& entry = entries_[n & mask_];
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
        uint64_t tick;
        uint64_t frame;
    };

    // 古い順に、読めた値を callback(Point) に渡す（書いている途中・上書きされたものは飛ばす）。
    template <class Callback>
    void read(Callback&& callback) const {
        const uint64_t written = written_.load(std::memory_order_acquire);
        const uint64_t capacity = capacity_;
        for (uint64_t n = written > capacity ? written - capacity : 0; n < written; ++n) {
            const Entry& entry = entries_[n & mask_];
            const uint64_t before = entry.sequence.load(std::memory_order_acquire);
            const Point point{entry.value.load(std::memory_order_relaxed), entry.tick.load(std::memory_order_relaxed),
                              entry.frame.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == n + 1 && entry.sequence.load(std::memory_order_relaxed) == before) {
                callback(point);
            }
        }
    }

private:
    struct Entry {
        std::atomic<uint64_t> sequence{0};
        std::atomic<double> value{0.0};
        std::atomic<uint64_t> tick{0};
        std::atomic<uint64_t> frame{0};
    };

    std::atomic<uint64_t> written_{0};
    uint64_t capacity_;  // 覚える数（読むときはこの数まで）
    uint64_t mask_;
    Entry* entries_;
};

// スレッドごとの集計1つ。
struct StatSlot {
    std::atomic<const StatSite*> site{nullptr};
    std::atomic<uint64_t> count{0};
    std::atomic<double> sum{0.0};
    std::atomic<double> last{0.0};
    std::atomic<uint64_t> epoch{0};  // 最小・最大を最後に書いた期間
    std::atomic<double> minimum[2]{};
    std::atomic<double> maximum[2]{};
    std::atomic<HistoryRing*> history{nullptr};  // ECO_STAT_HISTORY のときだけ

    // 書き出す側だけが使う（前に出したときの値）
    uint64_t reportedCount = 0;
    double reportedSum = 0.0;

    StatSlot() = default;
    StatSlot(const StatSlot&) = delete;
    StatSlot& operator=(const StatSlot&) = delete;
    ~StatSlot() { delete history.load(std::memory_order_relaxed); }

    void add(double value, uint64_t currentEpoch) noexcept {
        count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        sum.store(sum.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
        last.store(value, std::memory_order_relaxed);
        const size_t parity = currentEpoch & 1;
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

inline constexpr uint32_t kStatChunkSlots = 256;
inline constexpr uint32_t kStatChunks = 64;  // 集計の呼び出し位置は最大 16384 か所

struct StatChunk {
    StatSlot slots[kStatChunkSlots];
};

}  // namespace lg
