#pragma once

// 記録の形：呼び出し位置ごとの静的な情報（Site）と、バッファに書く1件（RecordHeader＋後ろの部分）。
// ログを書く側では文字列を作らず、引数を決まった形（ArgTag）に直してコピーするだけにする。
// この形はメモリーとファイル（.ecolog）で同じ。

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <string_view>
#include <type_traits>

#include "ECOCppLogger/Level.h"

namespace lg {

// ---- ログの型（種類）：重さとは別の軸。見るときの絞り込みに使う。記録には何も足さない ----------------------

struct LogType {
    const char* name;
};

namespace types {
inline constexpr LogType Log{"log"};        // 型を指定しないログ
inline constexpr LogType Assert{"assert"};  // アサート
}  // namespace types

// 呼び出し位置の「重さ＋型」。マクロの第1引数（Warn・Warn | Physics・Warn | "physics"）から作る。
struct SiteOptions {
    Level level;
    const char* type = types::Log.name;

    constexpr SiteOptions(Level level_) noexcept : level(level_) {}  // NOLINT: 重さだけでも書けるように
    constexpr SiteOptions(Level level_, const char* type_) noexcept : level(level_), type(type_) {}
};

constexpr SiteOptions operator|(Level level, LogType type) noexcept { return {level, type.name}; }
constexpr SiteOptions operator|(Level level, const char* type) noexcept { return {level, type}; }

// ---- 呼び出し位置 ---------------------------------------------------------------------------------------

// 引数を書くときの形。型の組は呼び出し位置ごとにコンパイル時に決まり、Site が持つ（記録には書かない）。
enum class ArgTag : uint8_t {
    None,
    Bool,
    Char,
    Int,      // int64
    UInt,     // uint64
    Double,   // double
    String,   // uint32 の長さ（最上位ビット＝切り詰めた）＋バイト列
    Pointer,  // アドレス（uint64）
    Int8, Int16, Int32, UInt8, UInt16, UInt32, Float,  // まとめ書きで、元の大きさのまま持つとき
};

// 1つの文字列引数で残す最大のバイト数（超えた分は捨て、"…" を付ける）。
inline constexpr size_t kMaxStringBytes = 4096;

enum class Sampling : uint8_t {
    None,
    EveryN,    // N回に1回（aux＝何回目か）
    FirstN,    // 最初のN回（aux＝何回目か）
    EveryMs,   // 一定時間に1回（aux＝前の記録の後に省いた回数）
    OnChange,  // 値が変わったときだけ（aux＝何回目の変化か。0 は最初の値）
};

// 呼び出し位置ごとの静的な情報（マクロの中で static constexpr に作る。ファイルには位置ごとに1回だけ書く）。
// 書式の {} には名前と書式指定を書ける（{hp}・{ratio:.3}・{id:x}）。名前・書式指定は読む側が解く。
struct Site {
    Level level;
    const char* type;
    const char* format;
    const char* file;
    const char* function;
    uint32_t line;
    const ArgTag* tags;  // 引数の形（argCount 個）
    uint8_t argCount;
    Sampling sampling;
    uint64_t samplingValue;
};

// 呼び出し位置の番号の置き場所（Site は書き換えられないので隣の static constinit に持つ）。0 はまだ振っていない。
using SiteIdSlot = std::atomic<uint32_t>;

// ---- 1件の形 --------------------------------------------------------------------------------------------

enum class RecordKind : uint8_t {
    Padding,      // リングバッファの終わりを飛ばす詰め物
    Log,          // 頭［＋時刻］［＋間引き］＋引数
    Batch,        // 頭［＋時刻］＋BatchInfo＋値の並び（件数ぶん、頭なし）
    Frame,        // 頭［＋時刻］＋uint64 のフレーム番号（ここから後の記録はこのフレーム）
    ScopeEnter,   // 頭［＋時刻］＋uint64 のトークン＋引数
    ScopeExit,    // 頭［＋時刻］＋uint64 のトークン
    FiberSwitch,  // 頭［＋時刻］＋uint64 のファイバーの ID（このスレッドで今から走るファイバー）
};

enum RecordFlags : uint8_t {
    kRecordHasTick = 1,  // 頭の後に uint64 の時刻
    kRecordHasAux = 2,   // その後に uint64 の間引きの情報（間引くログだけ）
};

// 1件の頭（8 バイト）。1件の大きさ（頭を含む）は 8 の倍数で、kMaxRecordBytes まで。
struct RecordHeader {
    uint16_t size;
    RecordKind kind;
    uint8_t flags;  // RecordFlags
    uint32_t site;  // 呼び出し位置の番号（Log・Batch・ScopeEnter。ほかは 0）
};
static_assert(sizeof(RecordHeader) == 8);

inline constexpr size_t kMaxRecordBytes = 65528;

struct BatchInfo {
    uint32_t count;        // 件数
    uint32_t elementSize;  // 1件の値のバイト数
    uint64_t endTick;      // 最後の1件の後の時刻（時刻を残さない設定なら 0）
};

// 頭の後ろ（時刻・間引き・中身）を読む位置。
struct RecordView {
    uint64_t tick = 0;  // 0 なら時刻なし
    uint64_t aux = 0;
    const unsigned char* payload = nullptr;
};

inline RecordView viewRecord(const RecordHeader& header) noexcept {
    RecordView view;
    const auto* cursor = reinterpret_cast<const unsigned char*>(&header) + sizeof(RecordHeader);
    if (header.flags & kRecordHasTick) {
        memcpy(&view.tick, cursor, 8);
        cursor += 8;
    }
    if (header.flags & kRecordHasAux) {
        memcpy(&view.aux, cursor, 8);
        cursor += 8;
    }
    view.payload = cursor;
    return view;
}

constexpr size_t alignRecord(size_t size) noexcept {
    return (size + 7) & ~static_cast<size_t>(7);
}

namespace detail {

// ---- 書式の確認：{…} の数をコンパイル時に数える（{{ と }} は文字そのもの）。形がおかしければ -1 ----------------

constexpr int countPlaceholders(const char* format) noexcept {
    int count = 0;
    for (size_t i = 0; format[i] != '\0'; ++i) {
        const char c = format[i];
        if (c == '{') {
            if (format[i + 1] == '{') {
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (format[j] != '\0' && format[j] != '}' && format[j] != '{') {
                ++j;
            }
            if (format[j] != '}') {
                return -1;  // 閉じていない・入れ子
            }
            ++count;
            i = j;
        } else if (c == '}') {
            if (format[i + 1] != '}') {
                return -1;  // 対のない }
            }
            ++i;
        }
    }
    return count;
}

// ---- 引数の正規化：どの型も ArgTag のどれかに直す ---------------------------------------------------------

struct Pointer {
    uint64_t address;
};

template <class T>
struct AlwaysFalse : std::false_type {};

// 自分の型を渡す口（ECO_LOG_FIELDS で特殊化する）。
template <class T>
struct Fields;

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
        return static_cast<int64_t>(value);
    } else if constexpr (std::is_integral_v<U>) {
        return static_cast<uint64_t>(value);
    } else if constexpr (std::is_floating_point_v<U>) {
        return static_cast<double>(value);
    } else if constexpr (std::is_array_v<U> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>) {
        size_t length = 0;  // 文字列リテラル・char の配列：終わりの '\0' まで
        while (length < std::extent_v<U> && value[length] != '\0') {
            ++length;
        }
        return std::string_view(value, length);
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        return value ? std::string_view(value) : std::string_view("(null)");
    } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
        return std::string_view(value);  // std::string・std::string_view 等
    } else if constexpr (std::is_pointer_v<U> || std::is_null_pointer_v<U>) {
        return Pointer{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<const volatile void*>(value)))};
    } else {
        static_assert(AlwaysFalse<U>::value,
                      "ECOCppLogger: この型はログに渡せません。数値・文字列・ポインター・enum に直すか、ECO_LOG_FIELDS で登録してください");
        return false;
    }
}

template <class T>
using Normalized = decltype(normalize(std::declval<const T&>()));

template <class N>
constexpr ArgTag tagOfNormalized() noexcept {
    if constexpr (std::is_same_v<N, bool>) return ArgTag::Bool;
    else if constexpr (std::is_same_v<N, char>) return ArgTag::Char;
    else if constexpr (std::is_same_v<N, int64_t>) return ArgTag::Int;
    else if constexpr (std::is_same_v<N, uint64_t>) return ArgTag::UInt;
    else if constexpr (std::is_same_v<N, double>) return ArgTag::Double;
    else if constexpr (std::is_same_v<N, std::string_view>) return ArgTag::String;
    else if constexpr (std::is_same_v<N, Pointer>) return ArgTag::Pointer;
    else return ArgTag::None;
}

// 引数の型の組 → ArgTag の静的な配列。マクロの中で decltype(tagList(引数...)) として使う（評価されない）。
template <ArgTag... Tags>
struct TagList {
    static constexpr int count = static_cast<int>(sizeof...(Tags));
    static constexpr ArgTag values[sizeof...(Tags) + 1] = {Tags..., ArgTag::None};
};

template <class... Args>
TagList<tagOfNormalized<Normalized<Args>>()...> tagList(const Args&...);

// ---- 書き込み：正規化した値の大きさとコピー -----------------------------------------------------------

inline size_t encodedSize(bool) noexcept { return 1; }
inline size_t encodedSize(char) noexcept { return 1; }
inline size_t encodedSize(int64_t) noexcept { return 8; }
inline size_t encodedSize(uint64_t) noexcept { return 8; }
inline size_t encodedSize(double) noexcept { return 8; }
inline size_t encodedSize(Pointer) noexcept { return 8; }
inline size_t encodedSize(std::string_view text) noexcept {
    return 4 + (text.size() < kMaxStringBytes ? text.size() : kMaxStringBytes);
}

template <class N>
inline unsigned char* encode(unsigned char* out, const N& value) noexcept {
    if constexpr (std::is_same_v<N, std::string_view>) {
        const auto length = static_cast<uint32_t>(value.size() < kMaxStringBytes ? value.size() : kMaxStringBytes);
        const uint32_t stored = length | (value.size() > kMaxStringBytes ? 0x80000000u : 0u);
        memcpy(out, &stored, 4);
        memcpy(out + 4, value.data(), length);
        return out + 4 + length;
    } else {
        memcpy(out, &value, sizeof(N));
        return out + sizeof(N);
    }
}

}  // namespace detail

}  // namespace lg
