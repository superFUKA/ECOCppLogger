#pragma once

// ECOCppLogger：ログを書く側を最も軽くした、LLM・人に渡すためのログ。
//
//   lg::start();                                           // 始める（lg::Config で出力先 等）
//   ECO_LOG_INFO("player {id} hp={hp}", id, hp);            // 名前付きの値（JSON では "args":{"id":…,"hp":…}）
//   ECO_LOG(Warn | Physics, "貫通 depth={depth:.3}", d);    // 重さ＋型（ECO_LOG_TYPE(Physics, "physics") で作る）
//   lg::writeOut();                                        // 呼んだスレッドでファイルへ（ゲームならループの終わり）
//   lg::stop();
//
// 書式：{} の数は引数と同じ（違えばコンパイルエラー）。{名前}・{名前:書式}（.3＝小数3桁、x＝16進、08＝0埋め8桁）。
//       書式は文字列リテラル。{{ と }} は { と } そのもの。
// 引数：bool・char・整数・浮動小数点・enum・文字列（const char*・std::string 等）・ポインター・ECO_LOG_FIELDS の型。
// 毎フレーム何万回も通る所：ECO_LOG_EVERY_N・FIRST_N・ONCE・EVERY_MS・ON_CHANGE・ECO_LOG_BATCH、
//                         集計 ECO_STAT・ECO_STAT_COUNT・ECO_STAT_TIME・ECO_STAT_HISTORY。
// 見る：ECOCppLoggerView <フォルダー>（人向け）、--json（LLM 向け）。

#include <source_location>
#include <tuple>

#include "ECOCppLogger/Logger.h"

// ---- 有効・無効（ファイルの先頭・プロジェクトの設定で） -------------------------------------------------------

// 0 にすると、すべてのマクロが消える（引数も評価しない。コードが残らない）。
#ifndef ECO_LOG_ENABLE
#define ECO_LOG_ENABLE 1
#endif

// この重さより軽いログはコンパイルで消す（0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Fatal）。
#ifndef ECO_LOG_COMPILE_LEVEL
#define ECO_LOG_COMPILE_LEVEL 0
#endif

// ---- 中で使うもの ---------------------------------------------------------------------------------------

#define ECO_LOG_CAT_IMPL_(a, b) a##b
#define ECO_LOG_CAT_(a, b) ECO_LOG_CAT_IMPL_(a, b)
// MSVC の従来のプリプロセッサは、ほかのマクロへ渡した __VA_ARGS__ を1つの引数とみなすので、もう一度展開させる。
#define ECO_LOG_EXPAND_(x) x

// 第1引数（Warn・Warn | Physics・Warn | "physics"）→ SiteOptions。Trace〜Fatal は lg::levels から短く書ける。
#define ECO_LOG_OPTIONS_(opts_)                       \
    []() constexpr {                                  \
        using namespace ::lg::levels;                 \
        return ::lg::SiteOptions(opts_);              \
    }()

// 呼び出し位置の静的な情報と番号の置き場所（eco_log_opt_・eco_log_tags_ を先に定義しておく）。
#define ECO_LOG_DEFINE_SITE_(sampling_, samplingValue_, fmt_)                                                   \
    static_assert(::lg::detail::countPlaceholders(fmt_) >= 0,                                                  \
                  "ECOCppLogger: 書式の { } の形がおかしい（閉じていない・入れ子・対のない }）");                \
    static_assert(::lg::detail::countPlaceholders(fmt_) == eco_log_tags_::count,                               \
                  "ECOCppLogger: 書式の {} の数と引数の数が合いません");                                        \
    static constexpr ::lg::Site eco_log_site_{eco_log_opt_.level,                                              \
                                              eco_log_opt_.type,                                               \
                                              (fmt_),                                                          \
                                              __FILE__,                                                        \
                                              ::std::source_location::current().function_name(),              \
                                              __LINE__,                                                        \
                                              eco_log_tags_::values,                                           \
                                              static_cast<::uint8_t>(eco_log_tags_::count),                    \
                                              (sampling_),                                                     \
                                              static_cast<::uint64_t>(samplingValue_),                         \
                                              eco_log_tags_::tagCount,                                         \
                                              eco_log_tags_::structNames,                                      \
                                              eco_log_tags_::structCount};                                     \
    static constinit ::lg::SiteIdSlot eco_log_id_ { 0 }

// 重さの確認（コンパイル時と実行時）。この後に { … } を続ける。
#define ECO_LOG_IF_ENABLED_(opts_)                                                         \
    static constexpr ::lg::SiteOptions eco_log_opt_ = ECO_LOG_OPTIONS_(opts_);             \
    if constexpr (static_cast<int>(eco_log_opt_.level) >= ECO_LOG_COMPILE_LEVEL)           \
        if (::lg::detail::isEnabled(eco_log_opt_.level))

// ECO_LOG_FIELDS の「項目ごとに」（16 個まで）。
#define ECO_LOG_NARGS_IMPL_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, n_, ...) n_
#define ECO_LOG_NARGS_(...) \
    ECO_LOG_EXPAND_(ECO_LOG_NARGS_IMPL_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))
#define ECO_LOG_FE_1(m, a) m(a)
#define ECO_LOG_FE_2(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_1(m, __VA_ARGS__))
#define ECO_LOG_FE_3(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_2(m, __VA_ARGS__))
#define ECO_LOG_FE_4(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_3(m, __VA_ARGS__))
#define ECO_LOG_FE_5(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_4(m, __VA_ARGS__))
#define ECO_LOG_FE_6(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_5(m, __VA_ARGS__))
#define ECO_LOG_FE_7(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_6(m, __VA_ARGS__))
#define ECO_LOG_FE_8(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_7(m, __VA_ARGS__))
#define ECO_LOG_FE_9(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_8(m, __VA_ARGS__))
#define ECO_LOG_FE_10(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_9(m, __VA_ARGS__))
#define ECO_LOG_FE_11(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_10(m, __VA_ARGS__))
#define ECO_LOG_FE_12(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_11(m, __VA_ARGS__))
#define ECO_LOG_FE_13(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_12(m, __VA_ARGS__))
#define ECO_LOG_FE_14(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_13(m, __VA_ARGS__))
#define ECO_LOG_FE_15(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_14(m, __VA_ARGS__))
#define ECO_LOG_FE_16(m, a, ...) m(a), ECO_LOG_EXPAND_(ECO_LOG_FE_15(m, __VA_ARGS__))
#define ECO_LOG_FOR_EACH_(m, ...) \
    ECO_LOG_EXPAND_(ECO_LOG_CAT_(ECO_LOG_FE_, ECO_LOG_NARGS_(__VA_ARGS__))(m, __VA_ARGS__))
#define ECO_LOG_FIELD_(name_) eco_log_object_.name_

// ---- 有効・無効にかかわらず使えるもの ------------------------------------------------------------------

// 型の定数を作る（名前空間で。よく使う型の書き間違いを防ぐ）。ECO_LOG(Warn | Physics, …) のように使う。
#define ECO_LOG_TYPE(var_, name_) inline constexpr ::lg::LogType var_ { name_ }

// 自分の型をログの引数に渡せるようにする（グローバルの名前空間で。項目は 16 個まで、public のメンバー）。
//   ECO_LOG_FIELDS(game::Vec3, x, y, z);
//   ECO_LOG_INFO("pos={pos}", position);   →  "pos={x=1, y=2, z=3}"、JSON では "args":{"pos":{"x":1,"y":2,"z":3}}
#define ECO_LOG_FIELDS(type_, ...)                                                               \
    template <>                                                                                  \
    struct lg::detail::Fields<type_> {                                                           \
        static constexpr const char* names = #__VA_ARGS__;                                       \
        static constexpr auto tie(const type_& eco_log_object_) noexcept {                       \
            return ::std::tie(ECO_LOG_FOR_EACH_(ECO_LOG_FIELD_, __VA_ARGS__));                    \
        }                                                                                        \
    }

#if ECO_LOG_ENABLE

// ---- ログ ---------------------------------------------------------------------------------------------

#define ECO_LOG(opts_, fmt_, ...)                                                       \
    do {                                                                                \
        ECO_LOG_IF_ENABLED_(opts_) {                                                    \
            using eco_log_tags_ = decltype(::lg::detail::tagList(__VA_ARGS__));         \
            ECO_LOG_DEFINE_SITE_(::lg::Sampling::None, 0, fmt_);                        \
            ::lg::detail::Emitter{eco_log_site_, eco_log_id_}(__VA_ARGS__);             \
        }                                                                               \
    } while (false)

// 条件が真のときだけ（条件は重さを確かめた後に評価する）。
#define ECO_LOG_IF(opts_, cond_, fmt_, ...)                                             \
    do {                                                                                \
        ECO_LOG_IF_ENABLED_(opts_) {                                                    \
            if (cond_) {                                                                \
                using eco_log_tags_ = decltype(::lg::detail::tagList(__VA_ARGS__));     \
                ECO_LOG_DEFINE_SITE_(::lg::Sampling::None, 0, fmt_);                    \
                ::lg::detail::Emitter{eco_log_site_, eco_log_id_}(__VA_ARGS__);         \
            }                                                                           \
        }                                                                               \
    } while (false)

// ---- 間引き：毎フレーム何万回も通る所向け（出力に "sample" と何回目か・省いた数が付く） ----------------------
// 書かない回は引数を評価しない。

// n 回に1回（スレッドごとに数える：1回目・n+1回目 …）。数は自分のスレッドのバッファの中にあり、ファイバーでも正しい。
#define ECO_LOG_EVERY_N(opts_, n_, fmt_, ...)                                                           \
    do {                                                                                                \
        ECO_LOG_IF_ENABLED_(opts_) {                                                                    \
            using eco_log_tags_ = decltype(::lg::detail::tagList(__VA_ARGS__));                         \
            ECO_LOG_DEFINE_SITE_(::lg::Sampling::EveryN, n_, fmt_);                                     \
            const ::uint64_t eco_log_n_ = ::lg::detail::countPass(eco_log_site_, eco_log_id_);          \
            if (eco_log_n_ % static_cast<::uint64_t>(n_) == 0) {                                        \
                ::lg::detail::SampledEmitter{eco_log_site_, eco_log_id_, eco_log_n_ + 1}(__VA_ARGS__);  \
            }                                                                                           \
        }                                                                                               \
    } while (false)

// 最初の n 回だけ（すべてのスレッドで合わせて）。
#define ECO_LOG_FIRST_N(opts_, n_, fmt_, ...)                                                           \
    do {                                                                                                \
        ECO_LOG_IF_ENABLED_(opts_) {                                                                    \
            static constinit ::lg::detail::FirstCounter eco_log_counter_;                               \
            if (const ::uint64_t eco_log_n_ = eco_log_counter_.next(static_cast<::uint64_t>(n_))) {     \
                using eco_log_tags_ = decltype(::lg::detail::tagList(__VA_ARGS__));                     \
                ECO_LOG_DEFINE_SITE_(::lg::Sampling::FirstN, n_, fmt_);                                 \
                ::lg::detail::SampledEmitter{eco_log_site_, eco_log_id_, eco_log_n_}(__VA_ARGS__);      \
            }                                                                                           \
        }                                                                                               \
    } while (false)

// 1回だけ。
#define ECO_LOG_ONCE(opts_, ...) ECO_LOG_EXPAND_(ECO_LOG_FIRST_N(opts_, 1, __VA_ARGS__))

// ms ミリ秒に1回まで（すべてのスレッドで合わせて）。省いた回数が出力の "skipped" に出る。
#define ECO_LOG_EVERY_MS(opts_, ms_, fmt_, ...)                                                             \
    do {                                                                                                    \
        ECO_LOG_IF_ENABLED_(opts_) {                                                                        \
            static constinit ::lg::detail::RateLimiter eco_log_limiter_;                                    \
            ::uint64_t eco_log_skipped_ = 0;                                                                \
            if (eco_log_limiter_.allow(static_cast<::uint64_t>(ms_), eco_log_skipped_)) {                   \
                using eco_log_tags_ = decltype(::lg::detail::tagList(__VA_ARGS__));                         \
                ECO_LOG_DEFINE_SITE_(::lg::Sampling::EveryMs, ms_, fmt_);                                   \
                ::lg::detail::SampledEmitter{eco_log_site_, eco_log_id_, eco_log_skipped_}(__VA_ARGS__);    \
            }                                                                                               \
        }                                                                                                   \
    } while (false)

// 値が変わったときだけ（最初の1回も書く）。name は文字列リテラル、value は 数値・bool・char・enum・ポインター。
//   ECO_LOG_ON_CHANGE(Info, "player.state", state);  →  "player.state = 0"、"player.state: 0 -> 2" …
// 前の値は呼び出し位置・スレッドごとに覚える（自分のスレッドのバッファの中。ファイバーでも正しい）。
#define ECO_LOG_ON_CHANGE(opts_, name_, value_)                                                              \
    do {                                                                                                     \
        ECO_LOG_IF_ENABLED_(opts_) {                                                                         \
            const auto eco_log_value_ = ::lg::detail::changeValue(value_);                                   \
            using eco_log_tags_ = decltype(::lg::detail::tagList(eco_log_value_));                           \
            ECO_LOG_DEFINE_SITE_(::lg::Sampling::OnChange, 0, name_ " = {value}");                           \
            auto eco_log_previous_ = eco_log_value_;                                                         \
            ::uint64_t eco_log_change_ = 0;                                                                  \
            if (::lg::detail::observeChange(eco_log_site_, eco_log_id_, eco_log_value_, eco_log_change_,     \
                                            eco_log_previous_)) {                                            \
                if (eco_log_change_ == 0) {                                                                  \
                    ::lg::detail::SampledEmitter{eco_log_site_, eco_log_id_, 0}(eco_log_value_);             \
                } else {                                                                                     \
                    using eco_log_change_tags_ = decltype(::lg::detail::tagList(eco_log_value_, eco_log_value_)); \
                    ECO_LOG_ON_CHANGE_EMIT_(name_, eco_log_change_tags_);                                    \
                }                                                                                            \
            }                                                                                                \
        }                                                                                                    \
    } while (false)

// 変化の行（別の呼び出し位置として定義する）。
#define ECO_LOG_ON_CHANGE_EMIT_(name_, tags_)                                                             \
    do {                                                                                                  \
        using eco_log_tags_ = tags_;                                                                      \
        ECO_LOG_DEFINE_SITE_(::lg::Sampling::OnChange, 0, name_ ": {from} -> {to}");                      \
        ::lg::detail::SampledEmitter{eco_log_site_, eco_log_id_, eco_log_change_}(eco_log_previous_,      \
                                                                                  eco_log_value_);        \
    } while (false)

// ---- まとめ書き：毎フレーム何十万回も回るループの中で、同じ書式のログを続けて残す ------------------------------
//
//   ECO_LOG_BATCH(candidates, Debug | Physics, "candidate {id} distance {d:.2}", int, float);
//   for (int i = 0; i < n; ++i) {
//       candidates.add(i, distance[i]);   // 頭も時刻も付けず、値だけを手元に元の大きさで並べる
//   }                                     // 一杯（8KB）になったとき・flush()・スコープの終わりに、まとめて1件として書く
//
// 書式・呼び出し位置・時刻は塊ごとに1回だけ。見るときは、塊の件数・かかった時間と、1件ずつの値が出る。
#define ECO_LOG_BATCH(var_, opts_, fmt_, ...)                                                                      \
    static constexpr ::lg::SiteOptions ECO_LOG_CAT_(eco_batch_opt_, __LINE__) = ECO_LOG_OPTIONS_(opts_);           \
    static_assert(::lg::detail::countPlaceholders(fmt_) == ::lg::detail::BatchTags<__VA_ARGS__>::count,            \
                  "ECOCppLogger: 書式の {} の数と ECO_LOG_BATCH の型の数が合いません");                              \
    static constexpr ::lg::Site ECO_LOG_CAT_(eco_batch_site_, __LINE__){                                           \
        ECO_LOG_CAT_(eco_batch_opt_, __LINE__).level,                                                              \
        ECO_LOG_CAT_(eco_batch_opt_, __LINE__).type,                                                               \
        (fmt_),                                                                                                    \
        __FILE__,                                                                                                  \
        ::std::source_location::current().function_name(),                                                        \
        __LINE__,                                                                                                  \
        ::lg::detail::BatchTags<__VA_ARGS__>::values,                                                              \
        static_cast<::uint8_t>(::lg::detail::BatchTags<__VA_ARGS__>::count),                                       \
        ::lg::Sampling::None,                                                                                      \
        0,                                                                                                         \
        static_cast<::uint16_t>(::lg::detail::BatchTags<__VA_ARGS__>::count)};                                     \
    static constinit ::lg::SiteIdSlot ECO_LOG_CAT_(eco_batch_id_, __LINE__){0};                                   \
    ::lg::Batch<__VA_ARGS__> var_ {                                                                                \
        ECO_LOG_CAT_(eco_batch_site_, __LINE__), ECO_LOG_CAT_(eco_batch_id_, __LINE__),                            \
            static_cast<int>(ECO_LOG_CAT_(eco_batch_opt_, __LINE__).level) >= ECO_LOG_COMPILE_LEVEL                \
    }

// フレームの区切り（メインループの頭で1回）。
#define ECO_FRAME() ::lg::nextFrame()

#else  // ECO_LOG_ENABLE

#define ECO_LOG(...) ((void)0)
#define ECO_LOG_IF(...) ((void)0)
#define ECO_LOG_EVERY_N(...) ((void)0)
#define ECO_LOG_FIRST_N(...) ((void)0)
#define ECO_LOG_ONCE(...) ((void)0)
#define ECO_LOG_EVERY_MS(...) ((void)0)
#define ECO_LOG_ON_CHANGE(...) ((void)0)
// add() は何もしない（add に渡した引数は評価される）。
#define ECO_LOG_BATCH(var_, ...) [[maybe_unused]] ::lg::NullBatch var_
#define ECO_FRAME() ((void)0)

#endif  // ECO_LOG_ENABLE

// ---- 集計：毎回は書かず、期間（Config::statIntervalMs）ごとに 回数・平均・最小・最大・最後 を1行で出す ----------
// 重さに関係なく数える。ECO_LOG_STATS=0 で消える（既定は ECO_LOG_ENABLE と同じ）。name は文字列リテラル。

#ifndef ECO_LOG_STATS
#define ECO_LOG_STATS ECO_LOG_ENABLE
#endif

#if ECO_LOG_STATS

#define ECO_LOG_STAT_SITE_(name_, kind_, capacity_)     static constinit ::lg::StatSite eco_stat_site_ { (name_), __FILE__, __LINE__, (kind_), (capacity_) }

// 値を集計する（約2ns）。
#define ECO_STAT(name_, value_)                                                           do {                                                                                      ECO_LOG_STAT_SITE_(name_, ::lg::StatKind::Value, 0);                                  ::lg::detail::addStat(eco_stat_site_, static_cast<double>(value_));               } while (false)

// 通った回数を数える（出力は回数と1秒あたり）。
#define ECO_STAT_COUNT(name_)                                                             do {                                                                                      ECO_LOG_STAT_SITE_(name_, ::lg::StatKind::Count, 0);                                  ::lg::detail::addStat(eco_stat_site_, 1.0);                                       } while (false)

// この行からスコープの終わりまでの時間を集計する（出力はマイクロ秒）。
#define ECO_STAT_TIME(name_)                                                                                        static constinit ::lg::StatSite ECO_LOG_CAT_(eco_stat_time_site_, __LINE__){(name_), __FILE__, __LINE__,                                                                                    ::lg::StatKind::Time, 0};          ::lg::detail::StatTimer ECO_LOG_CAT_(eco_stat_timer_, __LINE__) { ECO_LOG_CAT_(eco_stat_time_site_, __LINE__) }

// 値を集計し（ECO_STAT と同じ）、さらに最近の capacity 個をスレッドごとに覚えておく。ファイルには普段は書かず、
// lg::dumpHistory("理由") を呼んだとき・落ちたときに、何ms前か・フレーム番号と一緒に書き出す。
#define ECO_STAT_HISTORY(name_, capacity_, value_)                                        do {                                                                                      ECO_LOG_STAT_SITE_(name_, ::lg::StatKind::Value, (capacity_));                        ::lg::detail::addHistory(eco_stat_site_, static_cast<double>(value_));            } while (false)

#else  // ECO_LOG_STATS

#define ECO_STAT(...) ((void)0)
#define ECO_STAT_COUNT(...) ((void)0)
#define ECO_STAT_TIME(...) ((void)0)
#define ECO_STAT_HISTORY(...) ((void)0)

#endif  // ECO_LOG_STATS

#define ECO_LOG_TRACE(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Trace, __VA_ARGS__))
#define ECO_LOG_DEBUG(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Debug, __VA_ARGS__))
#define ECO_LOG_INFO(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Info, __VA_ARGS__))
#define ECO_LOG_WARN(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Warn, __VA_ARGS__))
#define ECO_LOG_ERROR(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Error, __VA_ARGS__))
#define ECO_LOG_FATAL(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Fatal, __VA_ARGS__))
