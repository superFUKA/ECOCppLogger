#pragma once

// .ecolog（動いている間に2進で書いたログ）を、見るときに人向けの文章・LLM 向けの JSON Lines に直す。
// 動いている最中のファイルも読める（feed で続きを渡す。書きかけの終わりは次に回す）。

#include <stddef.h>
#include <stdint.h>

#include "ECOCppLogger/Level.h"

namespace lg {

enum class ViewFormat : uint8_t {
    Text,  // 人向け：13:20:34.729352 f120 WARN  #physics [main] Player.cpp:42 Player::update: 貫通 depth=0.25
    Json,  // LLM 向け：1行に1件の JSON（1行目の session.start に各キーの意味 about が付く）
};

// 見るときの絞り込み（文字列の並びはカンマ区切り）。
struct ViewFilter {
    Level level = Level::Trace;          // これより軽いログは出さない（集計・最近の値は info として扱う）
    const char* threads = nullptr;       // このスレッドのログだけ（集計等の行は出す）
    // 型で絞る：types の型だけ、excludeTypes の型は出さない。型なしは log、まとめ書きは batch、集計は stat、
    // 最近の値は history、スコープは scope、始め・終わり・落ちた・捨てた は event。
    const char* types = nullptr;
    const char* excludeTypes = nullptr;
    const char* contains = nullptr;      // この文字列を含む行だけ
    const char* scopes = nullptr;        // このスコープ（ECO_SCOPE の名前の最初の語）の中の行だけ
    uint64_t firstFrame = 0;             // フレームの範囲（firstFrame〜lastFrame。0〜UINT64_MAX なら全部）
    uint64_t lastFrame = UINT64_MAX;
    uint32_t batchRows = 0;              // まとめ書きの塊ごとに出す件数（0 なら全部）
    bool fullPath = false;               // src をソースのフルパスで
};

class LogReader {
public:
    explicit LogReader(ViewFormat format, const ViewFilter& filter = {});
    ~LogReader();
    LogReader(const LogReader&) = delete;
    LogReader& operator=(const LogReader&) = delete;

    // ファイルの続きのバイト列を渡す。
    void feed(const void* data, size_t size);

    // ここまでに読めたものを時刻の順に並べた文字列（UTF-8）。次に take・feed を呼ぶまで有効。
    const char* take(size_t& size);

    bool invalid() const noexcept;  // ファイルの頭が .ecolog でない・版が合わない

private:
    struct State;
    State* state_;
};

// ファイル全体を読んで、文字列を sink(文字列, 大きさ, user) へ渡す（何回かに分けて渡すことがある）。読めなければ false。
using TextSink = void (*)(const char* text, size_t size, void* user);
bool readLog(const char* path, ViewFormat format, const ViewFilter& filter, TextSink sink, void* user);

// フォルダーの中で一番新しい .ecolog（UTF-8。なければ nullptr）。次に呼ぶまで有効。
const char* latestLog(const char* directory);

}  // namespace lg
