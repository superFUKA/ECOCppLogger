#pragma once

// 使う側の関数と、マクロから呼ぶホットパス。
// ホットパス：重さの確認（relaxed の load 1回）→ TSC を読む → 自分のスレッドのリングバッファに引数をコピー → 書いた位置を公開。
// 整形・ファイルへの書き出しは裏のスレッドが行う。バッファが一杯なら待たずに捨て、捨てた数を記録に残す。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>

#include "ECOCppLogger/Clock.h"
#include "ECOCppLogger/Level.h"
#include "ECOCppLogger/Record.h"
#include "ECOCppLogger/ThreadBuffer.h"

// 0 にすると、ログの時刻をコンパイルで消す（Config::recordTime の確認も消える）。
#ifndef ECO_LOG_RECORD_TIME
#define ECO_LOG_RECORD_TIME 1
#endif

namespace eco::log {

struct Config {
    // 書き出す先。ファイル名は <stem>_<YYYYMMDD-HHMMSS>_<pid>.ecolog（stem が空なら実行ファイルの名前）。
    // 中身は2進（動いている間は文字列にしない）。見るときは ECOCppLoggerView か eco::log::readLog で文章・JSON にする。
    std::filesystem::path directory = "logs";
    std::string stem;

    Level level = Level::Trace;       // これより軽いログは記録しない（実行中に setLevel で変えられる）
    Level consoleLevel = Level::Off;  // これ以上の重さは標準エラーにも文章で出す（そのぶん裏のスレッドが文字列を作る）

    // ログ1件ごとに時刻を残すか。false にすると TSC を読まない（1件あたり数ns・8バイト軽くなる）が、
    // 表示はスレッドの中の順番だけになり、スレッドをまたいだ前後は裏のスレッドが書き写した順（約2ms単位）になる。
    bool recordTime = true;
    // ログ1件ごとに今のフレーム番号（ECO_FRAME）を残すか。
    bool recordFrame = true;

    std::size_t threadBufferBytes = std::size_t{1} << 20;  // スレッドごとのリングバッファ（2のべき乗に切り上げ）
    std::uint32_t pollIntervalMs = 2;                      // 裏のスレッドが記録を見に行く間隔
    std::uint32_t statIntervalMs = 1000;                   // 集計を書き出す間隔

    std::uint64_t maxFileBytes = std::uint64_t{256} << 20;  // 超えたら次のファイル（_part2 …）へ
    std::uint32_t maxFiles = 8;                              // この実行で残すファイルの数（古いものから消す）

    bool installCrashHandler = true;  // 落ちたときに、残っている記録・集計・スタックを書き出す
    bool fullPath = false;            // src にソースのフルパスを出す（false ならファイル名だけ）
};

// 裏のスレッドを起こし、ファイルを開く。start() の前のログはバッファに溜まり、start() の後に書き出される。
bool start(const Config& config = {});

// 残りをすべて書き出して止める（atexit からも呼ばれる）。
void stop();

// ここまでに記録したログをすべてファイルへ書き出すまで待つ。
void flush();

// 集計を今すぐ書き出す（間隔を待たずに）。flush() も行う。
void flushStats();

bool isRunning() noexcept;
std::filesystem::path currentFile();

void setLevel(Level level) noexcept;
Level level() noexcept;

// 呼んだスレッドの名前（出力の "th"）。
void setThreadName(std::string_view name);

// ECO_HISTORY で覚えている最近の値を、すべてのスレッドの分ファイルへ書き出す（"lv":"history" の行）。
// 呼んだ時刻より後の値は含めない。落ちたときは自動で書き出す。
void dumpHistory(std::string_view reason);

// フレームの区切り。記録に今のフレーム番号（"f"）が付く。
void nextFrame() noexcept;
std::uint64_t frame() noexcept;

namespace detail {

extern std::atomic<std::uint8_t> g_level;
extern std::atomic<std::uint64_t> g_frame;
extern std::atomic<std::uint64_t> g_statEpoch;
extern std::atomic<bool> g_recordTime;
extern std::atomic<bool> g_recordFrame;

// 重さが Error 以上の記録を書いたら、裏のスレッドをすぐ起こす（落ちる前に書き出せるように）。
void wakeBackend() noexcept;

inline bool isEnabled(Level level) noexcept {
    return static_cast<std::uint8_t>(level) >= g_level.load(std::memory_order_relaxed);
}

inline bool recordTime() noexcept {
#if ECO_LOG_RECORD_TIME
    return g_recordTime.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

inline std::uint32_t siteId(const Site& site, SiteIdSlot& slot) noexcept {
    std::uint32_t id = slot.load(std::memory_order_relaxed);
    if (id == 0) [[unlikely]] {
        id = Registry::instance().registerSite(site, slot);
    }
    return id;
}

// 1件：頭（8）［＋時刻（8）］［＋間引きの情報（8）］＋引数。
template <bool HasAux, class... Normalized>
inline void emitNormalized(const Site& site, SiteIdSlot& slot, std::uint64_t aux, const Normalized&... values) noexcept {
    ThreadBuffer* buffer = threadBuffer();
    const std::uint32_t id = siteId(site, slot);
    const bool withTime = recordTime();
    const std::size_t size = alignRecord(sizeof(RecordHeader) + (withTime ? 8 : 0) + (HasAux ? 8 : 0) +
                                         (std::size_t{0} + ... + encodedSize(values)));
    std::byte* out = size <= kMaxRecordBytes && id != 0 ? buffer->reserve(size) : nullptr;
    if (!out) [[unlikely]] {
        buffer->countDrop();
        return;
    }
    const RecordHeader header{static_cast<std::uint16_t>(size), RecordKind::Log,
                              static_cast<std::uint8_t>((withTime ? kRecordHasTick : 0) | (HasAux ? kRecordHasAux : 0)),
                              id};
    std::memcpy(out, &header, sizeof(header));
    std::byte* cursor = out + sizeof(RecordHeader);
    if (withTime) {
        const std::uint64_t tick = ticks();
        std::memcpy(cursor, &tick, 8);
        cursor += 8;
    }
    if constexpr (HasAux) {
        std::memcpy(cursor, &aux, 8);
        cursor += 8;
    }
    ((cursor = encode(cursor, values)), ...);
    buffer->commit();
    if (site.level >= Level::Error) [[unlikely]] {
        wakeBackend();
    }
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
    std::uint64_t aux;

    template <class... Args>
    void operator()(const Args&... args) const noexcept {
        emitNormalized<true>(site, id, aux, normalize(args)...);
    }
};

// まとめ書きの塊を1件として書く。
inline void writeBatch(const Site& site, SiteIdSlot& slot, std::uint64_t startTick, std::uint32_t count,
                       std::uint32_t elementSize, const std::byte* elements) noexcept {
    ThreadBuffer* buffer = threadBuffer();
    const std::uint32_t id = siteId(site, slot);
    const bool withTime = startTick != 0;
    const std::size_t bytes = static_cast<std::size_t>(count) * elementSize;
    const std::size_t size = alignRecord(sizeof(RecordHeader) + (withTime ? 8 : 0) + sizeof(BatchInfo) + bytes);
    std::byte* out = size <= kMaxRecordBytes && id != 0 ? buffer->reserve(size) : nullptr;
    if (!out) [[unlikely]] {
        buffer->countDrop();
        return;
    }
    const RecordHeader header{static_cast<std::uint16_t>(size), RecordKind::Batch,
                              static_cast<std::uint8_t>(withTime ? kRecordHasTick : 0), id};
    std::memcpy(out, &header, sizeof(header));
    std::byte* cursor = out + sizeof(RecordHeader);
    if (withTime) {
        std::memcpy(cursor, &startTick, 8);
        cursor += 8;
    }
    const BatchInfo info{count, elementSize, withTime ? ticks() : 0};
    std::memcpy(cursor, &info, sizeof(info));
    cursor += sizeof(info);
    std::memcpy(cursor, elements, bytes);
    buffer->commit();
}

}  // namespace detail

// まとめ書き（ECO_LOG_BATCH が作る）。同じ書式のログが続く前提で、1件ごとには頭も時刻も付けず、
// 引数の値だけを手元（スタック）の場所に並べる。一杯になったとき・スコープの終わりに、まとめて1件として書く。
// 1件の追加は 一杯かの確認＋値のコピー だけ。引数はすべて決まった大きさの型（文字列は不可）。
template <class... Types>
class Batch {
    static_assert(sizeof...(Types) > 0, "ECOCppLogger: ECO_LOG_BATCH には引数の型を1つ以上書いてください");
    static_assert((!std::is_same_v<detail::Normalized<Types>, std::string_view> && ...),
                  "ECOCppLogger: ECO_LOG_BATCH の引数に文字列は使えません（数値・bool・char・enum・ポインター）");

public:
    static constexpr std::size_t kElementSize = (std::size_t{0} + ... + detail::fixedEncodedSize<detail::Normalized<Types>>());
    static constexpr std::size_t kScratchBytes = 8192;
    static constexpr std::uint32_t kCapacity = static_cast<std::uint32_t>(
        kScratchBytes / kElementSize > 0 ? kScratchBytes / kElementSize : 1);

    Batch(const Site& site, SiteIdSlot& id, bool compiled) noexcept
        : site_(site), id_(id), enabled_(compiled && detail::isEnabled(site.level)) {}
    ~Batch() { flush(); }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

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
        std::byte* cursor = scratch_ + static_cast<std::size_t>(count_) * kElementSize;
        ((cursor = detail::encode(cursor, detail::normalize(values))), ...);
        ++count_;
    }

    // 溜めた分を今すぐ書く（スコープの終わりでも書く）。
    void flush() noexcept {
        if (count_ == 0) {
            return;
        }
        detail::writeBatch(site_, id_, startTick_, count_, static_cast<std::uint32_t>(kElementSize), scratch_);
        count_ = 0;
    }

    bool enabled() const noexcept { return enabled_; }

private:
    const Site& site_;
    SiteIdSlot& id_;
    bool enabled_;
    std::uint32_t count_ = 0;
    std::uint64_t startTick_ = 0;
    alignas(8) std::byte scratch_[kCapacity * kElementSize];
};

namespace detail {

inline StatSlot* statSlotFor(StatSite& site) noexcept {
    ThreadBuffer* buffer = threadBuffer();
    std::uint32_t index = site.index.load(std::memory_order_relaxed);
    if (index == 0) [[unlikely]] {
        index = Registry::instance().registerStat(site);
    }
    StatSlot* slot = buffer->statSlot(index);
    if (slot && !slot->site.load(std::memory_order_relaxed)) [[unlikely]] {
        slot->site.store(&site, std::memory_order_release);
    }
    return slot;
}

inline void addStat(StatSite& site, double value) noexcept {
    if (StatSlot* slot = statSlotFor(site)) [[likely]] {
        slot->add(value, g_statEpoch.load(std::memory_order_relaxed));
    }
}

HistoryRing* createHistory(StatSlot& slot, const StatSite& site);

// 集計に足し、最近の値として覚えておく（ECO_HISTORY）。
inline void addHistory(StatSite& site, double value) noexcept {
    StatSlot* slot = statSlotFor(site);
    if (!slot) [[unlikely]] {
        return;
    }
    slot->add(value, g_statEpoch.load(std::memory_order_relaxed));
    HistoryRing* ring = slot->history.load(std::memory_order_relaxed);
    if (!ring) [[unlikely]] {
        ring = createHistory(*slot, site);
    }
    ring->push(value, ticks(), g_frame.load(std::memory_order_relaxed));
}

// ECO_LOG_ON_CHANGE：呼び出し位置・スレッドごとに、前の値を覚えておく。
template <class T>
struct ChangeTracker {
    bool initialized = false;
    T previous{};
    std::uint64_t changes = 0;
};

template <class T>
constexpr T changeValue(const T& value) noexcept {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>,
                  "ECOCppLogger: ECO_LOG_ON_CHANGE に渡せるのは 数値・bool・enum・ポインター です");
    return value;
}

// 区間の時間を ticks で集計に足す（ECO_SCOPE_TIME）。
class ScopeTimer {
public:
    explicit ScopeTimer(StatSite& site) noexcept : site_(site), start_(ticks()) {}
    ~ScopeTimer() { addStat(site_, static_cast<double>(ticks() - start_)); }
    ScopeTimer(const ScopeTimer&) = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;

private:
    StatSite& site_;
    std::uint64_t start_;
};

// ECO_LOG_EVERY_MS の判定：前に通してから interval 経っていれば通す。省いた回数を数える。
struct RateLimiter {
    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> suppressed{0};

    bool allow(std::uint64_t intervalMs, std::uint64_t& suppressedOut) noexcept {
        const std::uint64_t now = ticks();
        std::uint64_t expected = next.load(std::memory_order_relaxed);
        if (now < expected) {
            suppressed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto interval = static_cast<std::uint64_t>(static_cast<double>(intervalMs) * ticksPerMillisecond());
        if (!next.compare_exchange_strong(expected, now + interval, std::memory_order_relaxed)) {
            suppressed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        suppressedOut = suppressed.exchange(0, std::memory_order_relaxed);
        return true;
    }
};

}  // namespace detail

}  // namespace eco::log
