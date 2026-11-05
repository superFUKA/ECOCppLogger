#pragma once

// 設定。使う側に見える所では std を使わない（文字列は UTF-8 の const char*、並びはポインター＋個数かカンマ区切り）。
// 文字列は start() の中で写すので、呼んだ後は消してよい。

#include <stdint.h>

#include "ECOCppLogger/Level.h"

namespace lg {

// 書き出し先：ログを分けて別のフォルダーに集める（例：人が読む少ないログ・ゲームのログ・全部）。
// 分けるのは書き出すときで、ログを書く側は変わらない。呼び出し位置ごとに1回だけ判定し、合う記録を文字列にせずに写す。
struct Destination {
    const char* name = nullptr;         // directory の下のフォルダー名（空なら directory そのもの）
    Level level = Level::Trace;         // これより軽いログは書かない
    // 型で絞る（カンマ区切り。例 "physics,ai"）。types を書けばその型だけ、excludeTypes の型は書かない。
    // 型を付けないログは log、まとめ書きは自分の型に加えて batch、集計は stat、最近の値は history、スコープは scope。
    // 始め・終わり・落ちた・捨てた は、どの書き出し先にも書く。
    const char* types = nullptr;
    const char* excludeTypes = nullptr;
};

// 落ちたとき（未処理の例外・std::terminate・abort・アクセス違反 等）に書き出すもの。
struct CrashOptions {
    bool handler = true;          // 落ちたときの書き出しを入れる（残っているログ・集計・落ちたスレッドのスタック 等）
    bool allThreadStacks = true;  // 全スレッドのスタック
    bool registers = true;        // 落ちたスレッドのレジスター
    bool modules = true;          // 読み込まれていた DLL の一覧（位置・大きさ・版）
    bool minidump = true;         // minidump ファイル（デバッガーで開ける）も残す
};

// アサートが失敗したときの動き（ログ・スタック・最近の値を書き出した後）。
enum class AssertAction : uint8_t {
    Break,     // デバッガーがあれば止める。なければ Abort
    Abort,     // 落とす
    Continue,  // 続ける
};

// 始めに1回だけ書くアプリの情報（自動で集める環境の情報に添える）。
struct AppInfo {
    const char* name = nullptr;
    const char* version = nullptr;
    const char* commit = nullptr;  // 例：ビルドのときに -DMY_COMMIT=\"...\" で渡したもの
};

struct Config {
    // ---- 出力先 ----
    const char* directory = "logs";               // ファイル名は <stem>_<YYYYMMDD-HHMMSS>_<pid>.ecolog
    const char* stem = nullptr;                   // 空なら実行ファイルの名前
    const Destination* destinations = nullptr;    // 書き出し先（なければ1つ：全部を directory へ）
    uint32_t destinationCount = 0;
    uint64_t maxFileBytes = uint64_t{256} << 20;  // 超えたら次のファイル（_part2 …）へ
    uint32_t maxFiles = 8;                        // この実行で残すファイルの数（書き出し先ごと。古いものから消す）

    // ---- 何を残すか ----
    Level level = Level::Trace;  // これより軽いログは記録しない（実行中に lg::setLevel で変えられる）
    bool recordTime = true;      // ログごとの時刻（false で1回約10ns軽い。表示はスレッドの中の順番だけになる）
    bool recordFrame = true;     // フレームの区切り（ECO_FRAME）の印
    uint32_t statIntervalMs = 1000;  // 集計を出す間隔（writeOut() のときに、この間隔が来ていれば出す）

    // ---- スレッドごとのバッファ ----
    uint64_t threadBufferBytes = uint64_t{1} << 20;  // 2 のべき乗に切り上げ。writeOut() の間にたまる量より大きく
    uint32_t importantReservePercent = 10;           // 空きがこの割合を切ったら、Warn 未満のログは捨てる（数える）

    // ---- 標準エラーへの表示（人がその場で見る用。文字列を作るので重い） ----
    Level consoleLevel = Level::Off;

    CrashOptions crash;
    AssertAction assertAction = AssertAction::Break;
    AppInfo app;
    bool fullPath = false;  // src をソースのフルパスで
};

}  // namespace lg
