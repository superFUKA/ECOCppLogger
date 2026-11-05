// ECOCppLoggerView：.ecolog（動いている間に2進で書いたログ）を、人向けの文章か LLM 向けの JSON Lines にする。
//
//   ECOCppLoggerView <ファイル.ecolog | フォルダー> [オプション]
//     フォルダーなら、その中で一番新しい .ecolog を読む
//   --json            LLM 向けの JSON Lines で出す（既定は人向けの文章）
//   --level <重さ>     trace / debug / info / warn / error / fatal 以上だけ
//   --thread <名前>    このスレッドのログだけ
//   --grep <文字列>    この文字列を含む行だけ
//   --follow          書き足されるのを待って出し続ける（動いている最中のログを見る。Ctrl+C で終わる）
//   --out <ファイル>   標準出力でなくファイルへ書く
//   --full-path       src にソースのフルパスを出す
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ECOCppLogger/Reader.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

void usage() {
    std::fputs(
        "使い方: ECOCppLoggerView <ファイル.ecolog | フォルダー> [--json] [--level <重さ>] [--thread <名前>]\n"
        "                         [--grep <文字列>] [--follow] [--out <ファイル>] [--full-path]\n",
        stderr);
}

bool parseLevel(std::string_view text, eco::log::Level& level) {
    using eco::log::Level;
    for (Level candidate : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Fatal}) {
        if (eco::log::levelName(candidate) == text) {
            level = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::filesystem::path input;
    std::filesystem::path output;
    eco::log::ViewFormat format = eco::log::ViewFormat::Text;
    eco::log::ViewFilter filter;
    bool follow = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (argument == "--json") {
            format = eco::log::ViewFormat::Json;
        } else if (argument == "--follow") {
            follow = true;
        } else if (argument == "--full-path") {
            filter.fullPath = true;
        } else if (argument == "--level") {
            const char* value = next();
            if (!value || !parseLevel(value, filter.level)) {
                std::fputs("--level には trace / debug / info / warn / error / fatal を書いてください\n", stderr);
                return 2;
            }
        } else if (argument == "--thread") {
            const char* value = next();
            if (!value) return usage(), 2;
            filter.thread = value;
        } else if (argument == "--grep") {
            const char* value = next();
            if (!value) return usage(), 2;
            filter.contains = value;
        } else if (argument == "--out") {
            const char* value = next();
            if (!value) return usage(), 2;
            output = std::filesystem::u8path(value);
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        } else if (input.empty()) {
            input = std::filesystem::u8path(argument);
        } else {
            usage();
            return 2;
        }
    }
    if (input.empty()) {
        usage();
        return 2;
    }
    if (std::filesystem::is_directory(input)) {
        const auto latest = eco::log::latestLog(input);
        if (latest.empty()) {
            std::fprintf(stderr, ".ecolog がありません：%s\n", input.u8string().c_str());
            return 1;
        }
        input = latest;
    }

    std::ifstream file(input, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "開けません：%s\n", input.u8string().c_str());
        return 1;
    }
    std::FILE* out = stdout;
    if (!output.empty()) {
#if defined(_WIN32)
        out = _wfopen(output.c_str(), L"wb");
#else
        out = std::fopen(output.c_str(), "wb");
#endif
        if (!out) {
            std::fprintf(stderr, "書けません：%s\n", output.u8string().c_str());
            return 1;
        }
    }

    eco::log::LogReader reader(format, filter);
    std::vector<char> buffer(std::size_t{1} << 20);
    std::string text;
    while (true) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = static_cast<std::size_t>(file.gcount());
        if (read > 0) {
            reader.feed(buffer.data(), read);
            if (reader.invalid()) {
                std::fprintf(stderr, ".ecolog ではありません：%s\n", input.u8string().c_str());
                return 1;
            }
        }
        if (read == buffer.size()) {
            continue;  // まだ続きがある：全部読んでから時刻の順に並べる
        }
        reader.take(text);
        std::fwrite(text.data(), 1, text.size(), out);
        std::fflush(out);
        text.clear();
        if (!follow) {
            break;
        }
        file.clear();  // 終わりに着いた印を消して、書き足されるのを待つ
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (out != stdout) {
        std::fclose(out);
    }
    return 0;
}
