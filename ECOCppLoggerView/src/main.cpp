// ECOCppLoggerView：.ecolog（動いている間に2進で書いたログ）を、人向けの文章か LLM 向けの JSON Lines にする。
//
//   ECOCppLoggerView <ファイル.ecolog | フォルダー> [オプション]
//     フォルダーなら、その中で一番新しい .ecolog を読む
//   --json                 LLM 向けの JSON Lines（1行目の session.start に各キーの意味 about が付く）
//   --level <重さ>          trace / debug / info / warn / error / fatal 以上だけ
//   --type <型,…>           この型だけ（log＝型なし、batch、stat、history、scope、assert、event）
//   --exclude-type <型,…>   この型を出さない
//   --thread <名前,…>       このスレッドだけ
//   --scope <名前,…>        このスコープ（ECO_SCOPE の名前の最初の語）の中だけ
//   --frames <A-B>          フレーム A〜B だけ（A- や -B も書ける）
//   --grep <文字列>         この文字列を含む行だけ
//   --batch-rows <件数>     まとめ書きの塊ごとに出す件数（既定は全部）
//   --follow               書き足されるのを待って出し続ける（次のファイルへ替わっても追う。Ctrl+C で終わる）
//   --out <ファイル>        標準出力でなくファイルへ
//   --full-path            src をソースのフルパスで

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ECOCppLogger/Level.h"
#include "ECOCppLogger/Reader.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace {

void usage(std::FILE* to) {
    std::fputs(
        "使い方: ECOCppLoggerView <ファイル.ecolog | フォルダー> [オプション]\n"
        "  --json                 LLM 向けの JSON Lines（既定は人向けの文章）\n"
        "  --level <重さ>          trace / debug / info / warn / error / fatal 以上だけ\n"
        "  --type <型,…>           この型だけ（log＝型なし、batch、stat、history、scope、assert、event）\n"
        "  --exclude-type <型,…>   この型を出さない\n"
        "  --thread <名前,…>       このスレッドだけ\n"
        "  --scope <名前,…>        このスコープの中だけ\n"
        "  --frames <A-B>          フレーム A〜B だけ（A- や -B も書ける）\n"
        "  --grep <文字列>         この文字列を含む行だけ\n"
        "  --batch-rows <件数>     まとめ書きの塊ごとに出す件数\n"
        "  --follow               書き足されるのを待って出し続ける\n"
        "  --out <ファイル>        標準出力でなくファイルへ\n"
        "  --full-path            src をソースのフルパスで\n",
        to);
}

std::vector<std::string> utf8Arguments(int argc, char** argv) {
    std::vector<std::string> arguments;
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        arguments.push_back(std::move(text));
    }
    LocalFree(wide);
#else
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
#endif
    return arguments;
}

std::filesystem::path pathOf(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8Of(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool parseLevel(std::string_view text, lg::Level& level) {
    for (lg::Level candidate : {lg::Level::Trace, lg::Level::Debug, lg::Level::Info, lg::Level::Warn, lg::Level::Error,
                                lg::Level::Fatal}) {
        if (lg::levelName(candidate) == text) {
            level = candidate;
            return true;
        }
    }
    return false;
}

bool parseFrames(std::string_view text, uint64_t& first, uint64_t& last) {
    const size_t dash = text.find('-');
    const std::string from(text.substr(0, dash));
    const std::string to(dash == std::string_view::npos ? text : text.substr(dash + 1));
    char* end = nullptr;
    if (!from.empty()) {
        first = std::strtoull(from.c_str(), &end, 10);
        if (*end != '\0') return false;
    }
    if (dash == std::string_view::npos) {
        last = first;
    } else if (!to.empty()) {
        last = std::strtoull(to.c_str(), &end, 10);
        if (*end != '\0') return false;
    }
    return first <= last;
}

// 1つのファイルを読み、書けた分を出す。follow なら、書き足し・次のファイルへの切り替えを待ち続ける。
int view(std::filesystem::path input, const std::filesystem::path& directory, lg::ViewFormat format,
         const lg::ViewFilter& filter, bool follow, std::FILE* out) {
    std::vector<char> buffer(size_t{1} << 20);
    for (;;) {
        std::ifstream file(input, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "開けません：%s\n", utf8Of(input).c_str());
            return 1;
        }
        lg::LogReader reader(format, filter);
        auto pump = [&] {
            bool any = false;
            while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0) {
                reader.feed(buffer.data(), static_cast<size_t>(file.gcount()));
                any = true;
                if (file.eof()) break;
            }
            file.clear();
            size_t size = 0;
            const char* text = reader.take(size);
            std::fwrite(text, 1, size, out);
            std::fflush(out);
            return any;
        };
        pump();
        if (reader.invalid()) {
            std::fprintf(stderr, ".ecolog ではありません：%s\n", utf8Of(input).c_str());
            return 1;
        }
        if (!follow) return 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (pump()) continue;
            // 書き足されていなければ、次のファイル（_part2 …・次の実行）ができたかを見る
            if (directory.empty()) continue;
            if (const char* latest = lg::latestLog(utf8Of(directory).c_str())) {
                const auto next = pathOf(latest);
                if (next != input) {
                    input = next;
                    break;
                }
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    const std::vector<std::string> arguments = utf8Arguments(argc, argv);
    std::string input;
    std::string output;
    lg::ViewFormat format = lg::ViewFormat::Text;
    lg::ViewFilter filter;
    bool follow = false;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        const auto value = [&]() -> const char* { return i + 1 < arguments.size() ? arguments[++i].c_str() : nullptr; };
        auto need = [&](const char* text) {
            if (!text) {
                std::fprintf(stderr, "%.*s の後に値を書いてください\n", static_cast<int>(argument.size()), argument.data());
                std::exit(2);
            }
            return text;
        };
        if (argument == "--json") {
            format = lg::ViewFormat::Json;
        } else if (argument == "--follow") {
            follow = true;
        } else if (argument == "--full-path") {
            filter.fullPath = true;
        } else if (argument == "--level") {
            if (!parseLevel(need(value()), filter.level)) {
                std::fputs("--level には trace / debug / info / warn / error / fatal を書いてください\n", stderr);
                return 2;
            }
        } else if (argument == "--type") {
            filter.types = need(value());
        } else if (argument == "--exclude-type") {
            filter.excludeTypes = need(value());
        } else if (argument == "--thread") {
            filter.threads = need(value());
        } else if (argument == "--scope") {
            filter.scopes = need(value());
        } else if (argument == "--frames") {
            if (!parseFrames(need(value()), filter.firstFrame, filter.lastFrame)) {
                std::fputs("--frames は 10-20・10-・-20・15 のように書いてください\n", stderr);
                return 2;
            }
        } else if (argument == "--grep") {
            filter.contains = need(value());
        } else if (argument == "--batch-rows") {
            filter.batchRows = static_cast<uint32_t>(std::strtoul(need(value()), nullptr, 10));
        } else if (argument == "--out") {
            output = need(value());
        } else if (argument == "--help" || argument == "-h") {
            usage(stdout);
            return 0;
        } else if (input.empty() && !argument.starts_with("--")) {
            input = argument;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (input.empty()) {
        usage(stderr);
        return 2;
    }

    std::filesystem::path path = pathOf(input);
    std::filesystem::path directory;
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        directory = path;
        const char* latest = lg::latestLog(input.c_str());
        if (!latest) {
            std::fprintf(stderr, ".ecolog がありません：%s\n", input.c_str());
            return 1;
        }
        path = pathOf(latest);
    } else {
        directory = path.parent_path();
    }

    std::FILE* out = stdout;
    if (!output.empty()) {
#if defined(_WIN32)
        out = _wfopen(pathOf(output).c_str(), L"wb");
#else
        out = std::fopen(output.c_str(), "wb");
#endif
        if (!out) {
            std::fprintf(stderr, "書けません：%s\n", output.c_str());
            return 1;
        }
    }
    const int result = view(path, directory, format, filter, follow, out);
    if (out != stdout) std::fclose(out);
    return result;
}
