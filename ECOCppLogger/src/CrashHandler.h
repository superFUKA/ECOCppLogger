#pragma once

// 落ちたとき（例外が捕まらなかった・std::terminate・abort・アクセス違反 等）に、
// 残っている記録・集計と、落ちた所のスタックを書き出す。

#include <string>
#include <string_view>

namespace eco::log::detail {

void installCrashHandler();

// Logger.cpp が実装する：落ちた理由と、crash の行に足す JSON（"," で始まる "key":value の並び）を受け取って書き出す。
void emergencyFlush(std::string_view reason, const std::string& extraJson) noexcept;

// 今のスタック（呼んだ所から）を JSON の配列で返す。skip は飛ばす上の段の数。
std::string currentStackJson(int skip);

}  // namespace eco::log::detail
