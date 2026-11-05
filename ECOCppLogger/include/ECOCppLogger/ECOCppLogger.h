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
// 引数：bool・char・整数・浮動小数点・enum・文字列（const char*・std::string 等）・ポインター。
// 見る：ECOCppLoggerView <フォルダー>（人向け）、--json（LLM 向け）。

#include <source_location>

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
                                              static_cast<::uint64_t>(samplingValue_)};                        \
    static constinit ::lg::SiteIdSlot eco_log_id_ { 0 }

#if ECO_LOG_ENABLE

// ---- ログ ---------------------------------------------------------------------------------------------

#define ECO_LOG(opts_, fmt_, ...)                                                                       \
    do {                                                                                                \
        static constexpr ::lg::SiteOptions eco_log_opt_ = ECO_LOG_OPTIONS_(opts_);                      \
        if constexpr (static_cast<int>(eco_log_opt_.level) >= ECO_LOG_COMPILE_LEVEL) {                  \
            if (::lg::detail::isEnabled(eco_log_opt_.level)) {                                          \
                using eco_log_tags_ = decltype(::lg::detail::tagList(__VA_ARGS__));                     \
                ECO_LOG_DEFINE_SITE_(::lg::Sampling::None, 0, fmt_);                                    \
                ::lg::detail::Emitter{eco_log_site_, eco_log_id_}(__VA_ARGS__);                         \
            }                                                                                           \
        }                                                                                               \
    } while (false)

// 型の定数を作る（名前空間で。よく使う型の書き間違いを防ぐ）。ECO_LOG(Warn | Physics, …) のように使う。
#define ECO_LOG_TYPE(var_, name_) inline constexpr ::lg::LogType var_ { name_ }

// フレームの区切り（メインループの頭で1回）。
#define ECO_FRAME() ::lg::nextFrame()

#else  // ECO_LOG_ENABLE

#define ECO_LOG(...) ((void)0)
#define ECO_LOG_TYPE(var_, name_) inline constexpr ::lg::LogType var_ { name_ }
#define ECO_FRAME() ((void)0)

#endif  // ECO_LOG_ENABLE

#define ECO_LOG_TRACE(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Trace, __VA_ARGS__))
#define ECO_LOG_DEBUG(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Debug, __VA_ARGS__))
#define ECO_LOG_INFO(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Info, __VA_ARGS__))
#define ECO_LOG_WARN(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Warn, __VA_ARGS__))
#define ECO_LOG_ERROR(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Error, __VA_ARGS__))
#define ECO_LOG_FATAL(...) ECO_LOG_EXPAND_(ECO_LOG(::lg::Level::Fatal, __VA_ARGS__))
