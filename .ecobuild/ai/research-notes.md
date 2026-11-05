# 高速ログの研究メモ（タスク #1、research ブランチ）

## 参考にした方式
- NanoLog（Stanford）／fmtlog／Quill：呼び出し位置の静的な情報＋生の引数だけをスレッドごとのリングバッファへ。整形は裏のスレッドか、後で。
- glog の LOG_EVERY_N・LOG_FIRST_N：間引き。
- 集計（回数・合計・最小・最大）でホットパスの情報を残す：プロファイラーのカウンターの考え方。

## 試作の形（ECOCppLogger/）
- `Record.h`：Site（static constexpr。書式・ファイル・行・関数・引数の型）。引数の型はコンパイル時に決まり記録には書かない。
  書式の {} と引数の数が違えばコンパイルエラー。
- `ThreadBuffer.h`：スレッドごとの SPSC リングバッファ（ロックなし。一杯なら待たずに捨て、数える）＋集計の置き場所（持ち主だけが書く relaxed の load/store）。
- `Logger.cpp`：裏のスレッド。全スレッドから取り出し→時刻で並べ直し→JSON Lines。集計は期間ごとに1行。ファイルの分割。
- `CrashHandler.cpp`：SEH の未処理例外・std::terminate・SIGABRT 等で、残っている記録・集計・スタック（dbghelp）を書き出す。
- 出力の1行目（session.start）に、各キーの意味（about）を書いた：LLM がそのまま読める。

## 測定（Release、このPC）
| 呼び出し | 1回 |
|---|---|
| 重さで切られる ECO_LOG_DEBUG | 0.2 ns |
| ECO_LOG_INFO（整数2＋小数2） | 17〜18 ns（p99 20ns、p99.9 29ns） |
| ECO_LOG_INFO 文字列16文字 | 13〜14 ns |
| ECO_LOG_EVERY_N（スレッドごとに数える） | 1.7 ns（共有 atomic のときは 5.9 ns） |
| ECO_STAT / ECO_COUNT | 2 ns |
| ECO_SCOPE_TIME | 25〜28 ns（rdtsc 2回） |
| 比較：snprintf で整形だけ | 295〜312 ns |

- 毎フレーム5万回：集計だけ +0.06ms/frame、集計＋間引き +0.075ms/frame、全部残す +0.37ms/frame（しかも裏が追いつかず捨てる）。
- 裏のスレッドの書き出し：1.7 → 3.3 M行/秒（呼び出し位置の src/fn と、秒までの時刻を使い回した）。

## 分かったこと・方針の候補
1. ホットパスは「生の値のコピー」だけにする方式で十分速い（整形の約 1/20）。採用。
2. 律速は裏のスレッドの整形（約 3M行/秒）。毎フレーム数万回の所は、全部残すのではなく ECO_STAT／ECO_LOG_EVERY_N を使う設計が正しい。
   さらに速くするなら：ファイルには2進で書き、読むときに JSON にする（NanoLog 方式）。ただし LLM に渡す前に変換が要る。→ 本番で選ぶ。
3. 捨てたら `event:dropped` の行に数が出る（待たないので、ログのせいで止まらない）。
4. 落ちたときも、裏のスレッドが書いていない分まで残せた（テストで確認：abort・terminate・アクセス違反）。

## 2進の形に変えた（ユーザーの方針：見るまでは最も軽く保存、見るときは別の仕組み）
- 裏のスレッドはリングバッファの中身を .ecolog に書き写すだけ（初めての呼び出し位置だけ SiteDef を書く。キーは Site のアドレス）。
- 集計・最近の値・捨てた数・落ちた・始め・終わり は Event（tick＋JSON の中身）。めったにないので文字列のまま。
- 見る：ECOCppLoggerView（文章／--json、--level --thread --grep --follow --out）、ライブラリの readLog・LogReader。
- 書き出し：3.3M行/秒 → 22〜30M行/秒。書く側のコストは同じ（約15ns）。ファイルは大きくなる（1件 40バイトの頭＋引数）。
- Config::recordTime／recordFrame（ECO_LOG_RECORD_TIME=0 でコンパイルで消す）。時刻なしはスレッドの中の順番だけ。
- ECO_LOG_ON_CHANGE 約0.6ns（変わらない回）、ECO_HISTORY 約11ns。

## 本番（develop）で決める・直すこと
- 1件の頭（40バイト）を小さくする（時刻・フレームなしの設定なら頭も詰める、varint 等）。
- バッファをメモリーマップドファイルに置き、強制終了でも残す。
- ECOCppLoggerView の大きなファイル：今は全部読んでから並べる。時刻の窓ごとに並べて流す形に。
- 共有ライブラリ（DLL）にしたとき：g_level 等のデータと thread_local は DLL の境をまたげない → 静的ライブラリ前提にするか、関数で包む。
- 2進のファイル＋変換の道具を作るか（上の 2）。
- 書式の {} の中の指定（{:.3f} 等）に対応するか。
- 使う側の型を渡す口（ADL の関数で数値・文字列に直す）。
- 集計で同じ名前の呼び出し位置が複数あると、期間の中ではまとめるが total は位置ごと。
- ECO_LOG_FIRST_N・EVERY_MS は全スレッドで共有（atomic）。
- Linux のスタック（backtrace）。
