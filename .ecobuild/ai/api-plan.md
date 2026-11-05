# ECOCppLogger 本番の API（ユーザーと決めたこと。2026-11-05）

研究（タスク #1、research ブランチ）の結果を元に、develop で作り直す。

## 決まったこと
- 名前空間は `lg`（`log` は C の log() とぶつかる）。マクロの頭は `ECO_LOG_` のまま。
- 使う側に見える所では `std::` をなるべく使わない（uint64_t・bool・const char*・ポインター＋個数）。中では使ってよい。
  設定の文字列は const char*（UTF-8）、型の並びはカンマ区切りの文字列 "physics,ai"。
- 関数は名前空間のふつうの関数（中の状態は1つ：シングルトン的。デバッグ用なので問題なし）。
- 書き出しは1つだけ：`lg::writeOut()` を呼んだスレッドが書き出す。裏のスレッドは作らない。
  ほかに書くのは stop()・落ちたとき。集計・dumpHistory も次の writeOut で書く。
- バッファーはスレッドごと（チャンネルにまとめない）。一杯に近いときは軽いログから捨てる（importantReserve）。
  「どのバッファーに書くか」は1か所の関数に閉じ込め、将来チャンネルを足せるようにしておく。
- ファイバー対策：自分のスレッドのバッファーは TLS スロットを gs から直接読む（__readgsqword(0x1480+index*8)、
  index<64。取れなければ非インライン関数で読む）。測定：今と同じ 3.5ns（非インラインは +2.3ns）。
  EVERY_N・ON_CHANGE のスレッドごとのカウンターもバッファーの中に置き、同じ方法で読む。
- スコープ：記録に番号を付け、`lg::onFiberSwitch(fiberId)`（FiberPool が呼ぶ）で「このスレッドは今ファイバーX」の印を書く。
  読む側がファイバーごとの流れとスコープを組み立てる。
- フレームの区切り（ECO_FRAME）は残す。
- マクロで有効・無効を簡単に切り替える：ECO_LOG_ENABLE（0 で全部消える）＋ ECO_LOG_COMPILE_LEVEL・ECO_LOG_STATS・
  ECO_LOG_SCOPES・ECO_LOG_ASSERTS・ECO_LOG_RECORD_TIME。
- OS のエラーコードは専用の仕組みを作らず、普通のログで残す。

## 入れる機能
1. 最下層：スレッドごとのバッファー（頭 8 バイト＋時刻8［任意］＋間引き8［任意］）、.ecolog（2進）、書き出し先（Destination）、読み取り
2. ログ：ECO_LOG_TRACE..FATAL、ECO_LOG(Info, …)、型は `Warn | Physics`（ECO_LOG_TYPE で定数）、
   名前付きの値 `{hp}`・書式指定（.3・x・08）、ECO_LOG_FIELDS（自分の型）、
   ECO_LOG_IF・EVERY_N・FIRST_N・ONCE・EVERY_MS・ON_CHANGE、ECO_LOG_BATCH（値は元の大きさで）
3. スコープ：ECO_SCOPE("name {x}", x)
4. 計測：ECO_STAT・ECO_STAT_COUNT・ECO_STAT_TIME・ECO_STAT_HISTORY、lg::dumpHistory
5. アサート：ECO_ASSERT・ECO_CHECK（式として使える）・ECO_VERIFY、assertAction（Break/Abort/Continue）
6. 落ちたとき：全スレッドのスタック・レジスター・DLL の一覧・minidump・例外の連なり（今の分に加えて）
7. 始めに1回：実行の ID・pid・実行ファイル・コマンドライン・作業フォルダー・ビルド設定・コンパイラー・OS・PC 名・CPU・
   メモリー・GPU・アプリの名前/版/コミット（Config::app）、lg::setInfo。スレッドごとに OS のスレッド ID・名前。
8. 見る：ECOCppLoggerView（--json --level --type --exclude-type --thread --scope --frames --grep --batch-rows --follow --out）、
   lg::readLog・lg::LogReader

## 研究で残った課題
- 文字列を渡すログが 12→18ns に遅くなった原因
- 時刻を読む処理（約10ns）を軽くする
- Linux の fs からの TLS 読み・スタック
