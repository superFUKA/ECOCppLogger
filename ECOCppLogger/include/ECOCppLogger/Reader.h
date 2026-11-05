#pragma once

// .ecolog（動いている間に書いた2進のログ）を、見るときに文章・JSON Lines に直す。
// 動いている最中のファイルも読める（feed で続きを渡す。書きかけの終わりは次に回す）。

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ECOCppLogger/Level.h"

namespace eco::log {

enum class ViewFormat {
    Text,  // 人向け：13:20:34.729352 f120 WARN  [main] Player.cpp:42 Game::Player::update: hp が範囲外 …
    Json,  // LLM 向け：1行に1件の JSON（1行目の session.start に各キーの意味 about が付く）
};

struct ViewFilter {
    Level level = Level::Trace;  // これより軽いログは出さない（集計・最近の値は info として扱う）
    std::string thread;          // 空でなければ、このスレッドのログだけ（集計等の行は出す）
    std::string contains;        // 空でなければ、この文字列を含む行だけ
    bool fullPath = false;       // src にソースのフルパスを出す
    std::size_t batchRows = 0;   // 文章で、まとめ書きの塊ごとに出す件数（0 なら全部。JSON は常に全部）

    // ログの型で絞り込む。types が空でなければ、その型だけ。excludeTypes の型は出さない。
    // 型を付けないログは "log"。ログ以外の行は "stat"（集計）・"history"（最近の値）・"event"（始め・終わり・落ちた・捨てた）。
    std::vector<std::string> types;
    std::vector<std::string> excludeTypes;
};

class LogReader {
public:
    explicit LogReader(ViewFormat format, ViewFilter filter = {});
    ~LogReader();
    LogReader(const LogReader&) = delete;
    LogReader& operator=(const LogReader&) = delete;

    // ファイルの続きのバイト列を渡す。
    void feed(const void* data, std::size_t size);

    // ここまでに読めたものを時刻の順に out へ足す（足したものは中から消える）。
    void take(std::string& out);

    // ファイルの頭が .ecolog でなかった
    bool invalid() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// ファイル全体を読んで文章・JSON Lines にする。
std::string readLog(const std::filesystem::path& file, ViewFormat format, const ViewFilter& filter = {});

// フォルダーの中で一番新しい .ecolog（なければ空）。
std::filesystem::path latestLog(const std::filesystem::path& directory);

}  // namespace eco::log
