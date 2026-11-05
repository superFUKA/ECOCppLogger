#pragma once

// 記録の形：呼び出し位置ごとの静的な情報（Site）と、リングバッファに書く1件（RecordHeader＋引数の生の値）。
// ホットパスでは文字列の整形をせず、引数を決まった形（下の ArgTag）に直してコピーするだけにする。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

#include "ECOCppLogger/Level.h"

namespace eco::log {

// 引数を書くときの形。型の組は呼び出し位置ごとにコンパイル時に決まり、Site が持つ（記録には書かない）。
enum class ArgTag : std::uint8_t {
    None,
    Bool,
    Char,
    Int,      // int64
    UInt,     // uint64
    Double,   // double
    String,   // uint32 の長さ＋バイト列
    Pointer,  // アドレス
};

// 1つの文字列引数で残す最大のバイト数（超えた分は捨て、"…" を付ける）。
inline constexpr std::size_t kMaxStringBytes = 4096;

// ログの型（種類）。重さ（Level）とは別の軸で、「physics」「ai」「assert」のように文字列で付ける。
// 呼び出し位置の静的な情報にだけ持ち、記録には何も足さない（動いている間のコストはない）。
// ファイルには 型の名前 → 連番の ID の表を書き、見るとき（ECOCppLoggerView・readLog）に型で絞り込める。
struct LogType {
    const char* name;
};

namespace types {
inline constexpr LogType Log{"log"};        // 型を指定しないログ
inline constexpr LogType Assert{"assert"};  // アサート（ECO_ASSERT 用に取っておく）
}  // namespace types

constexpr LogType toLogType(LogType type) noexcept { return type; }
constexpr LogType toLogType(const char* name) noexcept { return LogType{name}; }

// 呼び出し位置ごとの静的な情報（マクロの中で static constexpr に作る）。
struct Site {
    Level level;
    std::string_view format;
    const char* file;
    const char* function;
    std::uint32_t line;
    const ArgTag* tags;     // 引数の形（argCount 個）
    std::uint8_t argCount;
    std::uint8_t sampling;  // Sampling（間引きの種類。出力に添える）
    std::uint64_t samplingValue;
    const char* type = types::Log.name;  // ログの型の名前
};

enum class Sampling : std::uint8_t {
    None,
    EveryN,   // N回に1回（aux＝何回目か）
    FirstN,   // 最初のN回（aux＝何回目か）
    EveryMs,  // 一定時間に1回（aux＝前の記録の後に省いた回数）
    OnChange, // 値が変わったときだけ（aux＝何回目の変化か。0 は最初の値）
};

// 集計（ECO_STAT・ECO_COUNT・ECO_SCOPE_TIME・ECO_HISTORY）の呼び出し位置。index は最初に使われたときに振る。
enum class StatKind : std::uint8_t {
    Value,     // 値の 回数・合計・最小・最大・最後
    Count,     // 回数だけ
    Duration,  // 区間の時間（ticks で集め、出すときにマイクロ秒にする）
    History,   // Value に加えて、最近の値を historyCapacity 個まで覚えておく（落ちたとき・dumpHistory で書き出す）
};

struct StatSite {
    const char* name;
    const char* file;
    std::uint32_t line;
    StatKind kind;
    std::uint32_t historyCapacity;
    std::atomic<std::uint32_t> index{0};

    constexpr StatSite(const char* name_, const char* file_, std::uint32_t line_, StatKind kind_,
                       std::uint32_t historyCapacity_ = 0) noexcept
        : name(name_), file(file_), line(line_), kind(kind_), historyCapacity(historyCapacity_) {}
};

// 呼び出し位置の番号の置き場所。Site は static constexpr（書き換えられない）なので、番号は隣の
// static constinit に持ち、最初に使われたときに振る（以後は load 1回と、ほぼ必ず外れる分岐1つ）。0 はまだ振っていない。
using SiteIdSlot = std::atomic<std::uint32_t>;

enum class RecordKind : std::uint8_t {
    Padding,  // リングバッファの終わりを飛ばすための詰め物
    Log,      // 1件のログ：頭［＋時刻］［＋間引きの情報］＋引数
    Frame,    // ECO_FRAME の印：頭［＋時刻］＋フレーム番号（uint64）。ここから後の記録はこのフレーム
    Batch,    // まとめ書き（ECO_LOG_BATCH）：頭［＋時刻］＋BatchInfo＋引数の並び（件数ぶん、頭なし）
};

enum RecordFlags : std::uint8_t {
    kRecordHasTick = 1,  // 頭の後に uint64 の時刻（Config::recordTime）
    kRecordHasAux = 2,   // その後に uint64 の間引きの情報（Sampling を参照。間引くログだけ）
};

// リングバッファの1件の頭（8 バイト）。1件の大きさ（頭を含む）は 8 の倍数で、65528 バイトまで。
// 以前の形（40 バイト：大きさ4・種類4・時刻8・フレーム番号8・Site のアドレス8・間引き8）から、
// 大きさを2、種類・印を1ずつ、呼び出し位置を4バイトの番号にし、時刻・間引きは要るときだけ後ろに付ける。
struct RecordHeader {
    std::uint16_t size;
    RecordKind kind;
    std::uint8_t flags;  // RecordFlags
    std::uint32_t site;  // 呼び出し位置の番号（1 から。Padding・Frame は 0）
};
static_assert(sizeof(RecordHeader) == 8);

inline constexpr std::size_t kMaxRecordBytes = 65528;

// まとめ書きの塊の情報（Batch の時刻の後）。
struct BatchInfo {
    std::uint32_t count;        // 件数
    std::uint32_t elementSize;  // 1件の引数のバイト数（引数はすべて決まった大きさ）
    std::uint64_t endTick;      // 最後の1件の後の時刻（時刻を残さない設定なら 0）
};

// 頭の後ろの部分（時刻・間引き・中身）を読む位置。
struct RecordView {
    std::uint64_t tick = 0;  // 0 なら時刻なし
    std::uint64_t aux = 0;
    const std::byte* payload = nullptr;
};

inline RecordView viewRecord(const RecordHeader& header) noexcept {
    RecordView view;
    const auto* cursor = reinterpret_cast<const std::byte*>(&header) + sizeof(RecordHeader);
    if (header.flags & kRecordHasTick) {
        std::memcpy(&view.tick, cursor, 8);
        cursor += 8;
    }
    if (header.flags & kRecordHasAux) {
        std::memcpy(&view.aux, cursor, 8);
        cursor += 8;
    }
    view.payload = cursor;
    return view;
}

namespace detail {

// ---- 引数の正規化：どの型も ArgTag のどれかに直す -------------------------------------------------

struct Pointer {
    std::uint64_t address;
};

template <class T>
struct AlwaysFalse : std::false_type {};

template <class T>
constexpr auto normalize(const T& value) noexcept {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return value;
    } else if constexpr (std::is_same_v<U, char> || std::is_same_v<U, char8_t>) {
        return static_cast<char>(value);
    } else if constexpr (std::is_enum_v<U>) {
        return normalize(static_cast<std::underlying_type_t<U>>(value));
    } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
        return static_cast<std::int64_t>(value);
    } else if constexpr (std::is_integral_v<U>) {
        return static_cast<std::uint64_t>(value);
    } else if constexpr (std::is_floating_point_v<U>) {
        return static_cast<double>(value);
    } else if constexpr (std::is_same_v<U, std::string_view> || std::is_same_v<U, std::string>) {
        return std::string_view(value);
    } else if constexpr (std::is_array_v<U> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>) {
        // 文字列リテラル・char の配列：終わりの '\0' までだけ。
        std::size_t length = 0;
        while (length < std::extent_v<U> && value[length] != '\0') {
            ++length;
        }
        return std::string_view(value, length);
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        return value ? std::string_view(value) : std::string_view("(null)");
    } else if constexpr (std::is_pointer_v<U> || std::is_null_pointer_v<U>) {
        return Pointer{static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(static_cast<const volatile void*>(value)))};
    } else {
        static_assert(AlwaysFalse<U>::value,
                      "ECOCppLogger: この型はログに渡せません。数値・文字列・ポインター・enum に直して渡してください");
        return false;
    }
}

template <class T>
using Normalized = decltype(normalize(std::declval<const T&>()));

template <class N>
constexpr ArgTag tagOfNormalized() noexcept {
    if constexpr (std::is_same_v<N, bool>) return ArgTag::Bool;
    else if constexpr (std::is_same_v<N, char>) return ArgTag::Char;
    else if constexpr (std::is_same_v<N, std::int64_t>) return ArgTag::Int;
    else if constexpr (std::is_same_v<N, std::uint64_t>) return ArgTag::UInt;
    else if constexpr (std::is_same_v<N, double>) return ArgTag::Double;
    else if constexpr (std::is_same_v<N, std::string_view>) return ArgTag::String;
    else if constexpr (std::is_same_v<N, Pointer>) return ArgTag::Pointer;
    else return ArgTag::None;
}

// 引数の型の組 → ArgTag の静的な配列。マクロの中で decltype(tagList(引数...)) として使う（評価されない）。
template <ArgTag... Tags>
struct TagList {
    static constexpr std::size_t count = sizeof...(Tags);
    static constexpr ArgTag values[count + 1] = {Tags..., ArgTag::None};
};

template <class... Args>
TagList<tagOfNormalized<Normalized<Args>>()...> tagList(const Args&...);

// まとめ書きの型の並び（ECO_LOG_BATCH に書いた型）→ ArgTag の配列。
template <class... Types>
using BatchTags = TagList<tagOfNormalized<Normalized<Types>>()...>;

// 決まった大きさで書ける型か（文字列以外）と、その大きさ。
template <class N>
constexpr std::size_t fixedEncodedSize() noexcept {
    if constexpr (std::is_same_v<N, bool> || std::is_same_v<N, char>) return 1;
    else return 8;
}

// ---- 書式の確認：{} の数をコンパイル時に数える（{{ と }} は文字そのもの） ------------------------------

constexpr std::size_t countPlaceholders(std::string_view format) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] == '{') {
            if (i + 1 < format.size() && format[i + 1] == '{') {
                ++i;
            } else if (i + 1 < format.size() && format[i + 1] == '}') {
                ++count;
                ++i;
            }
        } else if (format[i] == '}' && i + 1 < format.size() && format[i + 1] == '}') {
            ++i;
        }
    }
    return count;
}

// ---- 書き込み：正規化した値の大きさとコピー ---------------------------------------------------------

inline std::size_t encodedSize(bool) noexcept { return 1; }
inline std::size_t encodedSize(char) noexcept { return 1; }
inline std::size_t encodedSize(std::int64_t) noexcept { return 8; }
inline std::size_t encodedSize(std::uint64_t) noexcept { return 8; }
inline std::size_t encodedSize(double) noexcept { return 8; }
inline std::size_t encodedSize(Pointer) noexcept { return 8; }
inline std::size_t encodedSize(std::string_view text) noexcept {
    return 4 + (text.size() < kMaxStringBytes ? text.size() : kMaxStringBytes);
}

template <class N>
inline std::byte* encode(std::byte* out, const N& value) noexcept {
    if constexpr (std::is_same_v<N, std::string_view>) {
        const std::uint32_t length =
            static_cast<std::uint32_t>(value.size() < kMaxStringBytes ? value.size() : kMaxStringBytes);
        // 切り詰めたかどうかを最上位ビットに持たせる。
        const std::uint32_t stored = length | (value.size() > kMaxStringBytes ? 0x80000000u : 0u);
        std::memcpy(out, &stored, 4);
        std::memcpy(out + 4, value.data(), length);
        return out + 4 + length;
    } else {
        std::memcpy(out, &value, encodedSize(value));
        return out + encodedSize(value);
    }
}

constexpr std::size_t alignRecord(std::size_t size) noexcept {
    return (size + 7) & ~static_cast<std::size_t>(7);
}

}  // namespace detail

}  // namespace eco::log
