# ECOCppLogger

C＋＋用の高速なログ生成

コードのどこからでも残せて、LLM にも人にも渡せるログ。ログを書く側は文字列を作らず、自分のスレッドのバッファに
引数をそのまま写すだけ（1回 約12〜16ns、時刻なしなら約5ns）。ファイル（`.ecolog`、2進）へ書くのは `lg::writeOut()` を
呼んだスレッドだけで、裏のスレッドはありません。見るときに `ECOCppLoggerView` で文章か JSON Lines にします。

## 使い方

```cpp
#include "ECOCppLogger/ECOCppLogger.h"

ECO_LOG_TYPE(Physics, "physics");          // 型（見るときの絞り込みに使う）

int main() {
    lg::Config config;
    config.app = {.name = "MyGame", .version = "1.0.0"};
    lg::start(config);                      // 始めに環境の情報（OS・CPU・GPU 等）と実行の ID を書く

    for (;;) {
        ECO_FRAME();                        // フレームの区切り
        ECO_LOG_INFO("player {id} hp={hp}", id, hp);           // JSON では "args":{"id":…,"hp":…}
        ECO_LOG(Warn | Physics, "貫通 depth={d:.3}", depth);  // 書式指定：.3・x・08
        lg::writeOut();                     // ループの終わりに、このスレッドでファイルへ
    }
}                                           // 終わり・落ちたときは自動で書き出す
```

| 書き方 | 用途 |
|---|---|
| `ECO_LOG_TRACE`〜`ECO_LOG_FATAL`、`ECO_LOG(重さ \| 型, 書式, …)` | ふつうのログ |
| `ECO_LOG_IF`・`ECO_LOG_EVERY_N`・`ECO_LOG_FIRST_N`・`ECO_LOG_ONCE`・`ECO_LOG_EVERY_MS`・`ECO_LOG_ON_CHANGE` | 毎フレーム何万回も通る所（間引き。約1ns） |
| `ECO_LOG_BATCH(var, 重さ, 書式, 型…)` ＋ `var.add(…)` | ループの中の同じ形のログを1件にまとめる（add 約1ns） |
| `ECO_STAT`・`ECO_STAT_COUNT`・`ECO_STAT_TIME`・`ECO_STAT_HISTORY` ＋ `lg::dumpHistory` | 集計（期間ごとに1行。約2ns）・最近の値 |
| `ECO_SCOPE("名前 {x}", x)` ＋ `lg::onFiberSwitch` | 処理の入れ子を行に付ける（ファイバーでも続く） |
| `ECO_ASSERT`・`ECO_CHECK`（式）・`ECO_VERIFY` | アサート（ログ・スタック・最近の値を残す） |
| `ECO_LOG_FIELDS(型, 項目…)` | 自分の型を引数に渡せるようにする |
| `lg::setInfo`・`lg::writeStack`・`lg::runId` | 情報を足す・今のスタックを残す・実行の ID |

有効・無効はマクロで切り替えます：`ECO_LOG_ENABLE=0`（全部消える。引数も評価しない）、`ECO_LOG_COMPILE_LEVEL`、
`ECO_LOG_STATS`・`ECO_LOG_SCOPES`・`ECO_LOG_ASSERTS`・`ECO_LOG_RECORD_TIME`。

落ちたとき（例外・terminate・abort・アクセス違反）は、残っているログ・集計・最近の値に加えて、落ちた理由・例外の連なり・
スタック・レジスター・全スレッドのスタック・DLL の一覧を書き、minidump を残します。

## 見る

```sh
ECOCppLoggerView logs                      # 一番新しいログを人向けの文章で
ECOCppLoggerView logs --json               # LLM 向け（1行目に各キーの意味 about が付く）
ECOCppLoggerView logs --level warn --type physics --thread main --scope Physics --frames 100-200 --grep hp
ECOCppLoggerView logs --follow             # 動いている最中のログを見続ける
```

プログラムから読むなら `lg::readLog`・`lg::LogReader`（`ECOCppLogger/Reader.h`）。

## 性能（Release、`ECOCppLoggerBench`）

| 測ったもの | 1回 |
|---|---|
| `ECO_LOG_INFO`（整数2つ） | 13.9ns（時刻なし 5.4ns） |
| 比較：`snprintf` で整形だけ | 266ns |
| 重さで切られたログ・成功したアサート | 0.2ns |
| `ECO_LOG_EVERY_N`・`ON_CHANGE`（同じ値）・`BATCH` の add | 1.0〜1.3ns |
| `ECO_STAT` | 1.7ns |
| 8スレッド同時 | 20.3ns |
| `lg::writeOut()` の書き出し | 3000万件/秒（1件 48B） |

## ビルド

CMake 3.24以上とC++20のコンパイラでビルドできます（依存先は構成時に取得されます）。

```sh
cmake -S .ecobuild/.cppbuild -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

---

[ECOBuild](https://github.com/superFUKA/ECOBuild) で管理しています。
