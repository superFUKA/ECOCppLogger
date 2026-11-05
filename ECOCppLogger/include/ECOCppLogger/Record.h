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

// 呼び出し位置ごとの静的な情報（マクロの中で static constexpr に作る。実行時の登録は要らない）。
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
};

enum class Sampling : std::uint8_t {
    None,
    EveryN,   // N回に1回（aux＝何回目か）
    FirstN,   // 最初のN回（aux＝何回目か）
    EveryMs,  // 一定時間に1回（aux＝前の記録の後に省いた回数）
};

// 集計（ECO_STAT・ECO_COUNT・ECO_SCOPE_TIME）の呼び出し位置。index は最初に使われたときに振る。
enum class StatKind : std::uint8_t {
    Value,     // 値の 回数・合計・最小・最大・最後
    Count,     // 回数だけ
    Duration,  // 区間の時間（ticks で集め、出すときにマイクロ秒にする）
};

struct StatSite {
    const char* name;
    const char* file;
    std::uint32_t line;
    StatKind kind;
    std::atomic<std::uint32_t> index{0};

    constexpr StatSite(const char* name_, const char* file_, std::uint32_t line_, StatKind kind_) noexcept
        : name(name_), file(file_), line(line_), kind(kind_) {}
};

enum class RecordKind : std::uint32_t {
    Padding,  // リングバッファの終わりを飛ばすための詰め物
    Log,
};

// リングバッファの1件の頭。大きさ（頭を含む）は 8 の倍数。
struct RecordHeader {
    std::uint32_t size;
    RecordKind kind;
    std::uint64_t tick;
    std::uint64_t frame;
    const Site* site;
    std::uint64_t aux;  // 間引きの情報（Sampling を参照）
};
static_assert(sizeof(RecordHeader) == 40);

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
