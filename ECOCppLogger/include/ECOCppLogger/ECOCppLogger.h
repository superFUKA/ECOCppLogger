#pragma once

// ECOCppLogger：ホットパスを軽くした、LLM に渡すためのログ。
//
//   eco::log::start();                                   // 裏のスレッドを起こす（Config で出力先等）
//   ECO_LOG_INFO("player {} hp={} pos=({}, {})", id, hp, x, y);
//   ECO_LOG_EVERY_N(eco::log::Level::Debug, 1000, "step {}", i);   // 1000回に1回だけ
//   ECO_STAT("collision.candidates", n);                 // 毎回は書かず、回数・合計・最小・最大を集計して定期的に出す
//   ECO_SCOPE_TIME("physics.step");                      // 区間の時間を集計
//   ECO_LOG_ON_CHANGE(eco::log::Level::Info, "player.state", state);   // 値が変わったときだけ
//   ECO_HISTORY("player.hp", hp);                        // 最近の値を覚え、落ちたとき・dumpHistory() で書き出す
//   eco::log::stop();
//
// 書式の {} は引数と同じ数にする（違えばコンパイルエラー）。書式は文字列リテラル。
// 引数に使える型：bool・char・整数・浮動小数点・enum・文字列（const char*・std::string・std::string_view）・ポインター。

#include "ECOCppLogger/Logger.h"

// この重さより軽いログはコンパイルで消す（0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Fatal）。
#ifndef ECO_LOG_COMPILE_LEVEL
#define ECO_LOG_COMPILE_LEVEL 0
#endif

// 0 にすると ECO_STAT・ECO_COUNT・ECO_SCOPE_TIME を消す。
#ifndef ECO_LOG_STATS
#define ECO_LOG_STATS 1
#endif

#define ECO_LOG_CAT_IMPL_(a, b) a##b
#define ECO_LOG_CAT_(a, b) ECO_LOG_CAT_IMPL_(a, b)
// MSVC の従来のプリプロセッサは、ほかのマクロへ渡した __VA_ARGS__ を1つの引数とみなすので、もう一度展開させる。
#define ECO_LOG_EXPAND_(x) x

// 呼び出し位置の静的な情報と、書式・引数の数の確認（eco_log_tags_ を先に定義しておく）。
#define ECO_LOG_DEFINE_SITE_(lvl_, sampling_, samplingValue_, fmt_)                                                      \
    static_assert(::eco::log::detail::countPlaceholders(fmt_) == eco_log_tags_::count,                                 \
                  "ECOCppLogger: 書式の {} の数と引数の数が合いません");                                                \
    static constexpr ::eco::log::Site eco_log_site_ {                                                                  \
        (lvl_), (fmt_), __FILE__, ::std::source_location::current().function_name(), __LINE__, eco_log_tags_::values,   \
            static_cast<::std::uint8_t>(eco_log_tags_::count), static_cast<::std::uint8_t>(sampling_),                 \
            static_cast<::std::uint64_t>(samplingValue_)                                                               \
    }

// ---- ログ ---------------------------------------------------------------------------------------------

#define ECO_LOG(lvl_, fmt_, ...)                                                                        \
    do {                                                                                                \
        if constexpr (static_cast<int>(lvl_) >= ECO_LOG_COMPILE_LEVEL) {                                \
            if (::eco::log::detail::isEnabled(lvl_)) {                                                  \
                using eco_log_tags_ = decltype(::eco::log::detail::tagList(__VA_ARGS__));               \
                ECO_LOG_DEFINE_SITE_(lvl_, ::eco::log::Sampling::None, 0, fmt_);                        \
                ::eco::log::detail::Emitter{eco_log_site_}(__VA_ARGS__);                                \
            }                                                                                           \
        }                                                                                               \
    } while (false)

#define ECO_LOG_TRACE(...) ECO_LOG_EXPAND_(ECO_LOG(::eco::log::Level::Trace, __VA_ARGS__))
#define ECO_LOG_DEBUG(...) ECO_LOG_EXPAND_(ECO_LOG(::eco::log::Level::Debug, __VA_ARGS__))
#define ECO_LOG_INFO(...) ECO_LOG_EXPAND_(ECO_LOG(::eco::log::Level::Info, __VA_ARGS__))
#define ECO_LOG_WARN(...) ECO_LOG_EXPAND_(ECO_LOG(::eco::log::Level::Warn, __VA_ARGS__))
#define ECO_LOG_ERROR(...) ECO_LOG_EXPAND_(ECO_LOG(::eco::log::Level::Error, __VA_ARGS__))
#define ECO_LOG_FATAL(...) ECO_LOG_EXPAND_(ECO_LOG(::eco::log::Level::Fatal, __VA_ARGS__))

// 条件が真のときだけ（条件は重さを確かめた後に評価する）。
#define ECO_LOG_IF(lvl_, cond_, fmt_, ...)                                                              \
    do {                                                                                                \
        if constexpr (static_cast<int>(lvl_) >= ECO_LOG_COMPILE_LEVEL) {                                \
            if (::eco::log::detail::isEnabled(lvl_) && (cond_)) {                                       \
                using eco_log_tags_ = decltype(::eco::log::detail::tagList(__VA_ARGS__));               \
                ECO_LOG_DEFINE_SITE_(lvl_, ::eco::log::Sampling::None, 0, fmt_);                        \
                ::eco::log::detail::Emitter{eco_log_site_}(__VA_ARGS__);                                \
            }                                                                                           \
        }                                                                                               \
    } while (false)

// ---- 間引き：毎フレーム何万回も通る所向け（出力に "sample" と何回目か・省いた数が付く） ----------------------

// n 回に1回（スレッドごとに数える：1回目・n+1回目 …）。n は定数。
// 共有の atomic で数えると、毎回スレッド間でキャッシュラインを取り合う（測定で約 6ns → 約 1ns）。
#define ECO_LOG_EVERY_N(lvl_, n_, fmt_, ...)                                                            \
    do {                                                                                                \
        if constexpr (static_cast<int>(lvl_) >= ECO_LOG_COMPILE_LEVEL) {                                \
            if (::eco::log::detail::isEnabled(lvl_)) {                                                  \
                static thread_local ::std::uint64_t eco_log_counter_ = 0;                               \
                const ::std::uint64_t eco_log_n_ = eco_log_counter_++;                                  \
                if (eco_log_n_ % static_cast<::std::uint64_t>(n_) == 0) {                               \
                    using eco_log_tags_ = decltype(::eco::log::detail::tagList(__VA_ARGS__));           \
                    ECO_LOG_DEFINE_SITE_(lvl_, ::eco::log::Sampling::EveryN, n_, fmt_);                 \
                    ::eco::log::detail::Emitter{eco_log_site_, eco_log_n_ + 1}(__VA_ARGS__);            \
                }                                                                                       \
            }                                                                                           \
        }                                                                                               \
    } while (false)

// 最初の n 回だけ。n は定数。
#define ECO_LOG_FIRST_N(lvl_, n_, fmt_, ...)                                                            \
    do {                                                                                                \
        if constexpr (static_cast<int>(lvl_) >= ECO_LOG_COMPILE_LEVEL) {                                \
            if (::eco::log::detail::isEnabled(lvl_)) {                                                  \
                static ::std::atomic<::std::uint64_t> eco_log_counter_{0};                              \
                if (eco_log_counter_.load(::std::memory_order_relaxed) < static_cast<::std::uint64_t>(n_)) { \
                    const ::std::uint64_t eco_log_n_ =                                                  \
                        eco_log_counter_.fetch_add(1, ::std::memory_order_relaxed);                     \
                    if (eco_log_n_ < static_cast<::std::uint64_t>(n_)) {                                \
                        using eco_log_tags_ = decltype(::eco::log::detail::tagList(__VA_ARGS__));       \
                        ECO_LOG_DEFINE_SITE_(lvl_, ::eco::log::Sampling::FirstN, n_, fmt_);             \
                        ::eco::log::detail::Emitter{eco_log_site_, eco_log_n_ + 1}(__VA_ARGS__);        \
                    }                                                                                   \
                }                                                                                       \
            }                                                                                           \
        }                                                                                               \
    } while (false)

#define ECO_LOG_ONCE(lvl_, ...) ECO_LOG_EXPAND_(ECO_LOG_FIRST_N(lvl_, 1, __VA_ARGS__))

// ms ミリ秒に1回まで。ms は定数。省いた回数が出力の "skipped" に出る。
#define ECO_LOG_EVERY_MS(lvl_, ms_, fmt_, ...)                                                          \
    do {                                                                                                \
        if constexpr (static_cast<int>(lvl_) >= ECO_LOG_COMPILE_LEVEL) {                                \
            if (::eco::log::detail::isEnabled(lvl_)) {                                                  \
                static ::eco::log::detail::RateLimiter eco_log_limiter_;                                \
                ::std::uint64_t eco_log_skipped_ = 0;                                                   \
                if (eco_log_limiter_.allow(static_cast<::std::uint64_t>(ms_), eco_log_skipped_)) {      \
                    using eco_log_tags_ = decltype(::eco::log::detail::tagList(__VA_ARGS__));           \
                    ECO_LOG_DEFINE_SITE_(lvl_, ::eco::log::Sampling::EveryMs, ms_, fmt_);               \
                    ::eco::log::detail::Emitter{eco_log_site_, eco_log_skipped_}(__VA_ARGS__);          \
                }                                                                                       \
            }                                                                                           \
        }                                                                                               \
    } while (false)

// 値が変わったときだけ（最初の1回も書く）。name は文字列リテラル、value は 数値・bool・enum・ポインター。
//   ECO_LOG_ON_CHANGE(Level::Info, "player.state", state);  →  "player.state = 0"、"player.state: 0 -> 2" …
// 前の値は呼び出し位置・スレッドごとに覚える（同じ行を別の物に使い回すと、物が替わるたびに書く）。
#define ECO_LOG_ON_CHANGE(lvl_, name_, value_)                                                                   \
    do {                                                                                                         \
        if constexpr (static_cast<int>(lvl_) >= ECO_LOG_COMPILE_LEVEL) {                                         \
            if (::eco::log::detail::isEnabled(lvl_)) {                                                           \
                const auto eco_log_value_ = ::eco::log::detail::changeValue(value_);                             \
                static thread_local ::eco::log::detail::ChangeTracker<                                           \
                    ::std::remove_const_t<decltype(eco_log_value_)>>                                             \
                    eco_log_tracker_;                                                                            \
                if (!eco_log_tracker_.initialized) {                                                             \
                    eco_log_tracker_.initialized = true;                                                         \
                    eco_log_tracker_.previous = eco_log_value_;                                                  \
                    using eco_log_tags_ = decltype(::eco::log::detail::tagList(eco_log_value_));                 \
                    ECO_LOG_DEFINE_SITE_(lvl_, ::eco::log::Sampling::OnChange, 0, name_ " = {}");                \
                    ::eco::log::detail::Emitter{eco_log_site_, 0}(eco_log_value_);                              \
                } else if (eco_log_tracker_.previous != eco_log_value_) {                                        \
                    const auto eco_log_previous_ = eco_log_tracker_.previous;                                    \
                    eco_log_tracker_.previous = eco_log_value_;                                                  \
                    using eco_log_tags_ =                                                                        \
                        decltype(::eco::log::detail::tagList(eco_log_previous_, eco_log_value_));                \
                    ECO_LOG_DEFINE_SITE_(lvl_, ::eco::log::Sampling::OnChange, 0, name_ ": {} -> {}");           \
                    ::eco::log::detail::Emitter{eco_log_site_, ++eco_log_tracker_.changes}(eco_log_previous_,    \
                                                                                             eco_log_value_);    \
                }                                                                                                \
            }                                                                                                    \
        }                                                                                                        \
    } while (false)

// ---- 集計：毎回は書かず、期間（Config::statIntervalMs）ごとに 回数・合計・平均・最小・最大・最後 を1行で出す ----

#if ECO_LOG_STATS

#define ECO_LOG_STAT_IMPL_(name_, kind_, value_)                                                         \
    do {                                                                                                 \
        static constinit ::eco::log::StatSite eco_stat_site_{(name_), __FILE__, __LINE__, (kind_)};      \
        ::eco::log::detail::addStat(eco_stat_site_, static_cast<double>(value_));                        \
    } while (false)

// 値を集計する（name は文字列リテラル）。
#define ECO_STAT(name_, value_) ECO_LOG_STAT_IMPL_(name_, ::eco::log::StatKind::Value, value_)

// 通った回数を数える。
#define ECO_COUNT(name_) ECO_LOG_STAT_IMPL_(name_, ::eco::log::StatKind::Count, 1)

// この行からスコープの終わりまでの時間を集計する（出力はマイクロ秒）。
#define ECO_SCOPE_TIME(name_)                                                                                       \
    static constinit ::eco::log::StatSite ECO_LOG_CAT_(eco_scope_site_, __LINE__){                                  \
        (name_), __FILE__, __LINE__, ::eco::log::StatKind::Duration};                                               \
    ::eco::log::detail::ScopeTimer ECO_LOG_CAT_(eco_scope_timer_, __LINE__) { ECO_LOG_CAT_(eco_scope_site_, __LINE__) }

// 値を集計し（ECO_STAT と同じ）、さらに最近の capacity 個をメモリーに覚えておく。ファイルには普段は書かず、
// 落ちたとき・eco::log::dumpHistory("理由") を呼んだときに、フレーム番号・何ms前か と一緒に書き出す。
#define ECO_HISTORY_N(name_, capacity_, value_)                                                                \
    do {                                                                                                       \
        static constinit ::eco::log::StatSite eco_stat_site_{(name_), __FILE__, __LINE__,                      \
                                                             ::eco::log::StatKind::History, (capacity_)};      \
        ::eco::log::detail::addHistory(eco_stat_site_, static_cast<double>(value_));                           \
    } while (false)

#define ECO_HISTORY(name_, value_) ECO_HISTORY_N(name_, 256, value_)

#else

#define ECO_HISTORY_N(name_, capacity_, value_) \
    do {                                        \
    } while (false)
#define ECO_HISTORY(name_, value_) \
    do {                           \
    } while (false)

#define ECO_STAT(name_, value_) \
    do {                        \
    } while (false)
#define ECO_COUNT(name_) \
    do {                 \
    } while (false)
#define ECO_SCOPE_TIME(name_) static_assert(true)

#endif

// フレームの区切り（メインループの頭で1回）。
#define ECO_FRAME() ::eco::log::nextFrame()
