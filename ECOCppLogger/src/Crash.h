#pragma once

// 落ちたとき（捕まらなかった例外・std::terminate・abort・アクセス違反 等）に、残っているログ・集計・最近の値と、
// 落ちた理由・例外の連なり・落ちたスレッドのスタックとレジスター・全スレッドのスタック・DLL の一覧を書き、minidump を残す。

#include "ECOCppLogger/Config.h"

namespace lg::detail {

// 入れる（何度呼んでも1回だけ。設定は呼ぶたびに替わる）。
void installCrashHandler(const CrashOptions& options);

// std::set_terminate は MSVC ではスレッドごとに効くので、ログを書くスレッドごとに入れる（バッファを作るとき）。
void installThreadCrashHandler() noexcept;

}  // namespace lg::detail
