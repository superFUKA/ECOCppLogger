#pragma once

// スタック：呼び出しのアドレスを取り、関数名・ソースの位置（分かれば）の JSON の並びにする。
// アサート・lg::writeStack・落ちたときに使う。名前を引く dbghelp は1つのスレッドずつしか使えないので、中でロックする。

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace lg::detail {

// 今のスレッドのスタック（skip 個は飛ばす）。取れた数を返す。
size_t captureStack(uint64_t* addresses, size_t capacity, size_t skip) noexcept;

// アドレスの並び → ["Game::update (Game.cpp:42)", …]
void appendStackJson(std::string& out, const uint64_t* addresses, size_t count);

// アドレス1つ → "Game::update (Game.cpp:42)"（名前が分からなければ "module+0x1234"）。
std::string describeAddress(uint64_t address);

// 名前を引く準備（何度呼んでもよい）。落ちる前に済ませておくと、落ちたときに速い。
void initializeSymbols() noexcept;

}  // namespace lg::detail
