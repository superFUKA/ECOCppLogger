#pragma once

// 始めに1回だけ集める、動いている環境の情報（LLM・人が「どこで・何で動いたか」を知るため）。

#include <string>

#include "ECOCppLogger/Config.h"

namespace lg::detail {

// "environment" の行の中身（"lv":"info","event":"environment",…）。
// アプリ・実行ファイル・コマンドライン・作業フォルダー・ビルド設定・コンパイラー・OS・PC 名・CPU・メモリー・GPU。
std::string collectEnvironment(const AppInfo& app);

// 実行ごとの ID（UUID の形）。
std::string newRunId();

}  // namespace lg::detail
