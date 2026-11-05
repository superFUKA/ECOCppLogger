#pragma once

// テスト用：テストごとのフォルダーにログを書き、読んだ行を返す。

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "ECOCppLogger/ECOCppLogger.h"
#include "ECOCppLogger/Reader.h"

namespace lgtest {

inline std::string testDirectory() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const auto path = std::filesystem::temp_directory_path() / "ECOCppLoggerTest" /
                      (std::string(info->test_suite_name()) + "." + info->name());
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

inline std::filesystem::path pathOf(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// テストの間、Config の文字列を持っておく。
struct TestConfig {
    std::string directory = testDirectory();
    lg::Config config;

    TestConfig() {
        config.directory = directory.c_str();
        config.crash.handler = false;
        config.threadBufferBytes = uint64_t{4} << 20;
    }
};

inline std::string readText(const char* path, lg::ViewFormat format = lg::ViewFormat::Json,
                            const lg::ViewFilter& filter = {}) {
    std::string text;
    EXPECT_TRUE(lg::readLog(
        path, format, filter,
        [](const char* data, size_t size, void* user) { static_cast<std::string*>(user)->append(data, size); }, &text))
        << path;
    return text;
}

inline std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        lines.push_back(line);
    }
    return lines;
}

inline std::vector<std::string> readLines(const char* path, lg::ViewFormat format = lg::ViewFormat::Json,
                                          const lg::ViewFilter& filter = {}) {
    return splitLines(readText(path, format, filter));
}

inline size_t countContaining(const std::vector<std::string>& lines, std::string_view text) {
    return static_cast<size_t>(std::count_if(lines.begin(), lines.end(), [&](const std::string& line) {
        return line.find(text) != std::string::npos;
    }));
}

inline const std::string* findContaining(const std::vector<std::string>& lines, std::string_view text) {
    for (const auto& line : lines) {
        if (line.find(text) != std::string::npos) return &line;
    }
    return nullptr;
}

// start() から stop() まで。lines() は writeOut() してから今のファイルを読む。
class Session {
public:
    explicit Session(const lg::Config& config) {
        std::error_code error;
        std::filesystem::remove_all(pathOf(config.directory), error);
        EXPECT_TRUE(lg::start(config));
        file_ = lg::currentFile();
    }
    ~Session() {
        lg::stop();
        lg::setLevel(lg::Level::Trace);
    }

    const std::string& file() const { return file_; }

    std::vector<std::string> lines(lg::ViewFormat format = lg::ViewFormat::Json, const lg::ViewFilter& filter = {}) {
        lg::writeOut();
        return readLines(file_.c_str(), format, filter);
    }

private:
    std::string file_;
};

}  // namespace lgtest
