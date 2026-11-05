#pragma once

// テスト用：テストごとのフォルダーにログを書き、書き出した行を読む。

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "ECOCppLogger/ECOCppLogger.h"
#include "ECOCppLogger/Reader.h"

namespace eco::log::test {

inline std::filesystem::path testDirectory() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::filesystem::temp_directory_path() / "ECOCppLoggerTest" /
           (std::string(info->test_suite_name()) + "." + info->name());
}

inline Config testConfig() {
    Config config;
    config.directory = testDirectory();
    config.consoleLevel = Level::Off;
    config.installCrashHandler = false;
    config.threadBufferBytes = std::size_t{4} << 20;
    return config;
}

// .ecolog を読んで JSON Lines（LLM 向けの形）にし、1行ずつに分ける。
inline std::vector<std::string> readLines(const std::filesystem::path& path,
                                          ViewFormat format = ViewFormat::Json) {
    std::vector<std::string> lines;
    std::istringstream input(readLog(path, format));
    for (std::string line; std::getline(input, line);) {
        lines.push_back(line);
    }
    return lines;
}

inline std::size_t countContaining(const std::vector<std::string>& lines, std::string_view text) {
    return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(), [&](const std::string& line) {
        return line.find(text) != std::string::npos;
    }));
}

inline const std::string* findContaining(const std::vector<std::string>& lines, std::string_view text) {
    for (const auto& line : lines) {
        if (line.find(text) != std::string::npos) {
            return &line;
        }
    }
    return nullptr;
}

// start() から stop() まで。lines() は flush() してから今のファイルを読む。
class Session {
public:
    explicit Session(Config config = testConfig()) {
        std::error_code error;
        std::filesystem::remove_all(config.directory, error);
        setLevel(config.level);
        EXPECT_TRUE(start(config));
    }
    ~Session() {
        stop();
        setLevel(Level::Trace);
    }

    std::vector<std::string> lines() {
        flush();
        return readLines(currentFile());
    }

    std::vector<std::string> linesWithStats() {
        flushStats();
        return readLines(currentFile());
    }
};

}  // namespace eco::log::test
