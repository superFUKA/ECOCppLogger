#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "ECOCppLogger/Logger.h"
#include "TestSession.h"

using eco::log::Level;
using eco::log::test::countContaining;
using eco::log::test::findContaining;
using eco::log::test::readLines;
using eco::log::test::Session;
using eco::log::test::testConfig;

namespace {

std::vector<std::filesystem::path> logFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".ecolog") {
            files.push_back(entry.path());
        }
    }
    return files;
}

}  // namespace

TEST(Logger, WritesSessionStartAndEnd) {
    auto config = testConfig();
    config.stem = "session";
    std::filesystem::path file;
    {
        Session session(config);
        ECO_LOG_INFO("hello");
        file = eco::log::currentFile();
        EXPECT_EQ(file.parent_path(), config.directory);
        EXPECT_EQ(file.filename().string().rfind("session_", 0), 0u);
        EXPECT_EQ(file.extension(), ".ecolog");
    }
    EXPECT_FALSE(eco::log::isRunning());
    const auto lines = readLines(file);
    ASSERT_GE(lines.size(), 3u);
    EXPECT_NE(lines.front().find("\"event\":\"session.start\""), std::string::npos);
    EXPECT_NE(lines.front().find("\"about\":"), std::string::npos);
    EXPECT_NE(lines.back().find("\"event\":\"session.end\",\"crashed\":false,\"records\":1,\"dropped\":0"),
              std::string::npos)
        << lines.back();
}

TEST(Logger, StopWritesEverythingWithoutFlush) {
    auto config = testConfig();
    config.pollIntervalMs = 60'000;  // 止めるまで裏のスレッドは見に行かない
    std::filesystem::path file;
    {
        Session session(config);
        for (int i = 0; i < 1000; ++i) {
            ECO_LOG_INFO("record {}", i);
        }
        file = eco::log::currentFile();
    }
    EXPECT_EQ(countContaining(readLines(file), "\"msg\":\"record "), 1000u);
}

TEST(Logger, KeepsRecordsWrittenBeforeStart) {
    eco::log::stop();
    ECO_LOG_INFO("before start");
    Session session;
    EXPECT_EQ(countContaining(session.lines(), "\"msg\":\"before start\""), 1u);
}

TEST(Logger, RotatesFilesAndKeepsTheNewest) {
    auto config = testConfig();
    config.maxFileBytes = 8 * 1024;
    config.maxFiles = 3;
    {
        Session session(config);
        for (int i = 0; i < 2000; ++i) {
            ECO_LOG_INFO("rotation {}", i);
            if (i % 100 == 0) {
                eco::log::flush();
            }
        }
    }
    const auto files = logFiles(config.directory);
    EXPECT_EQ(files.size(), 3u);
    for (const auto& file : files) {
        const auto lines = readLines(file);
        ASSERT_FALSE(lines.empty());
        EXPECT_NE(lines.front().find("\"event\":\"session.start\""), std::string::npos);
    }
}

TEST(Logger, StartTwiceFails) {
    Session session;
    EXPECT_TRUE(eco::log::isRunning());
    EXPECT_FALSE(eco::log::start(testConfig()));
}

// ---- 落ちたとき：子のプロセスで落とし、残ったファイルを親で読む ---------------------------------------------

namespace {

eco::log::Config crashConfig() {
    auto config = testConfig();
    config.installCrashHandler = true;
    config.pollIntervalMs = 60'000;  // 裏のスレッドは書き出さない：落ちたときの書き出しだけで残ることを確かめる
    config.stem = "crash";
    return config;
}

std::vector<std::string> crashLog(const eco::log::Config& config) {
    const auto files = logFiles(config.directory);
    EXPECT_EQ(files.size(), 1u);
    return files.empty() ? std::vector<std::string>{} : readLines(files.front());
}

[[noreturn]] void abortAfterLogging() {
    ECO_LOG_INFO("before abort {}", 7);
    ECO_STAT("crash.stat", 3);
    for (int i = 1; i <= 20; ++i) {
        ECO_HISTORY_N("crash.history", 4, i * 10);
    }
    std::abort();
}

[[noreturn]] void throwAfterLogging() {
    ECO_LOG_INFO("before throw");
    throw std::runtime_error("boom");
}

}  // namespace

TEST(LoggerDeathTest, AbortWritesPendingRecordsAndStack) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto config = crashConfig();
    std::error_code error;
    std::filesystem::remove_all(config.directory, error);
    EXPECT_DEATH(
        {
            eco::log::start(config);
            abortAfterLogging();
        },
        "");
    const auto lines = crashLog(config);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"before abort 7\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"stat\":\"crash.stat\""), 1u);
    // 落ちたときは、覚えていた最近の値（最後の 4 個）も書き出す
    const std::string* history = findContaining(lines, "\"history\":\"crash.history\"");
    ASSERT_NE(history, nullptr);
    EXPECT_NE(history->find("\"reason\":\"crash\""), std::string::npos) << *history;
    EXPECT_NE(history->find(",170],["), std::string::npos) << *history;
    EXPECT_NE(history->find(",200]]"), std::string::npos) << *history;
    EXPECT_EQ(history->find(",160]"), std::string::npos) << *history;
    const std::string* crash = findContaining(lines, "\"event\":\"crash\"");
    ASSERT_NE(crash, nullptr);
    EXPECT_NE(crash->find("\"stack\":["), std::string::npos) << *crash;
    EXPECT_NE(crash->find("abortAfterLogging"), std::string::npos) << *crash;
    EXPECT_EQ(countContaining(lines, "\"event\":\"session.end\",\"crashed\":true"), 1u);
}

TEST(LoggerDeathTest, UncaughtExceptionWritesWhat) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto config = crashConfig();
    std::error_code error;
    std::filesystem::remove_all(config.directory, error);
    EXPECT_DEATH(
        {
            eco::log::start(config);
            try {
                throwAfterLogging();
            } catch (...) {
                std::terminate();
            }
        },
        "");
    const auto lines = crashLog(config);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"before throw\""), 1u);
    const std::string* crash = findContaining(lines, "\"event\":\"crash\"");
    ASSERT_NE(crash, nullptr);
    EXPECT_NE(crash->find("\"exception\":\"boom\""), std::string::npos) << *crash;
}

// 大量にログを出している最中に、範囲外アクセスの例外（vector::at の std::out_of_range）が捕まらずに落ちる。
TEST(LoggerDeathTest, OutOfRangeDuringHeavyLogging) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto config = crashConfig();
    std::error_code error;
    std::filesystem::remove_all(config.directory, error);
    EXPECT_DEATH(
        {
            eco::log::start(config);
            std::atomic<bool> stopLogging{false};
            std::vector<std::thread> loggers;
            for (int t = 0; t < 4; ++t) {
                loggers.emplace_back([t, &stopLogging] {
                    for (int i = 0; !stopLogging.load(); ++i) {
                        ECO_LOG_DEBUG("heavy {} {}", t, i);
                        ECO_STAT("heavy.value", i);
                    }
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::thread([] {
                std::vector<int> values(10);
                const std::size_t index = 10;
                ECO_LOG_INFO("about to read index {} of {}", index, values.size());
                volatile int value = values.at(index);  // 捕まえない
                (void)value;
            }).join();
        },
        "");
    const auto lines = crashLog(config);
    EXPECT_GT(countContaining(lines, "\"msg\":\"heavy "), 1000u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"about to read index 10 of 10\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"stat\":\"heavy.value\""), 1u);
    const std::string* crash = findContaining(lines, "\"event\":\"crash\"");
    ASSERT_NE(crash, nullptr);
    EXPECT_NE(crash->find("\"exception\":\"invalid vector subscript\""), std::string::npos) << *crash;
    EXPECT_NE(crash->find("vector<int,std::allocator<int> >::at"), std::string::npos) << *crash;
    // 集計の最小・最大は、その期間に書いた値の範囲の中にある
    const std::string* stat = findContaining(lines, "\"stat\":\"heavy.value\"");
    ASSERT_NE(stat, nullptr);
    EXPECT_NE(stat->find("\"min\":0,"), std::string::npos) << *stat;
}

TEST(LoggerDeathTest, AccessViolationWritesPendingRecords) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto config = crashConfig();
    std::error_code error;
    std::filesystem::remove_all(config.directory, error);
    // テストの本体は GoogleTest が SEH の例外を捕まえるので、捕まえない別のスレッドで落とす。
    EXPECT_DEATH(
        {
            eco::log::start(config);
            std::thread([] {
                ECO_LOG_INFO("before access violation");
                volatile int* pointer = nullptr;
                *pointer = 1;
            }).join();
        },
        "");
    const auto lines = crashLog(config);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"before access violation\""), 1u);
    const std::string* crash = findContaining(lines, "\"event\":\"crash\"");
    ASSERT_NE(crash, nullptr);
#if defined(_WIN32)
    EXPECT_NE(crash->find("\"name\":\"access violation\",\"access\":\"write 0x0\""), std::string::npos) << *crash;
    EXPECT_NE(crash->find("LoggerTest.cpp:"), std::string::npos) << *crash;  // 落ちた行がスタックに出る
#endif
}
