#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::readLines;
using lgtest::Session;
using lgtest::TestConfig;

namespace {

std::vector<std::filesystem::path> logFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().extension() == ".ecolog") files.push_back(entry.path());
    }
    return files;
}

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace

TEST(Logger, WritesSessionStartAndEnd) {
    TestConfig test;
    test.config.stem = "session";
    std::string file;
    {
        Session session(test.config);
        ECO_LOG_INFO("hello");
        file = session.file();
        EXPECT_EQ(lgtest::pathOf(file).filename().string().rfind("session_", 0), 0u);
        EXPECT_EQ(lgtest::pathOf(file).extension(), ".ecolog");
    }
    EXPECT_FALSE(lg::isRunning());
    const auto lines = readLines(file.c_str());
    ASSERT_GE(lines.size(), 3u);
    EXPECT_NE(lines.front().find("\"event\":\"session.start\""), std::string::npos);
    EXPECT_NE(lines.front().find("\"about\":"), std::string::npos);
    EXPECT_NE(lines.back().find("\"event\":\"session.end\",\"crashed\":false,\"records\":1,\"dropped\":0"),
              std::string::npos)
        << lines.back();
}

TEST(Logger, WritesOnlyWhenWriteOutIsCalled) {
    TestConfig test;
    Session session(test.config);
    for (int i = 0; i < 100; ++i) {
        ECO_LOG_INFO("frame log {i}", i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(countContaining(readLines(session.file().c_str()), "frame log"), 0u);  // 裏で書くことはない
    lg::writeOut();  // 呼んだスレッドで、その場で書く
    EXPECT_EQ(countContaining(readLines(session.file().c_str()), "\"msg\":\"frame log "), 100u);
}

TEST(Logger, WritesLogsOfOtherThreads) {
    TestConfig test;
    Session session(test.config);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            lg::setThreadName(("worker" + std::to_string(t)).c_str());
            for (int i = 0; i < 1000; ++i) {
                ECO_LOG_DEBUG("w{t} {i}", t, i);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"msg\":\"w"), 4000u);
    EXPECT_EQ(countContaining(lines, "\"th\":\"worker3\",\"src\""), 1000u);
    EXPECT_EQ(countContaining(lines, "\"event\":\"dropped\""), 0u);
}

TEST(Logger, KeepsRecordsWrittenBeforeStart) {
    lg::stop();
    ECO_LOG_INFO("before start");
    TestConfig test;
    Session session(test.config);
    EXPECT_EQ(countContaining(session.lines(), "\"msg\":\"before start\""), 1u);
}

TEST(Logger, DropsLightLogsFirstWhenBufferIsNearlyFull) {
    TestConfig test;
    test.config.threadBufferBytes = 4096;
    test.config.importantReservePercent = 50;
    Session session(test.config);
    std::thread([] {  // 新しいスレッド：4096 バイトのバッファ
        for (int i = 0; i < 1000; ++i) {
            ECO_LOG_DEBUG("noise {i}", i);
        }
        ECO_LOG_ERROR("important");  // 軽いログで一杯でも、取っておいた分に入る
    }).join();
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"msg\":\"important\""), 1u);
    const std::string* dropped = findContaining(lines, "\"event\":\"dropped\"");
    ASSERT_NE(dropped, nullptr);
    EXPECT_GT(countContaining(lines, "\"msg\":\"noise "), 0u);
}

TEST(Logger, RecordsFrameNumbers) {
    TestConfig test;
    Session session(test.config);
    const auto before = lg::frame();
    ECO_FRAME();
    ECO_LOG_INFO("in frame 1");
    ECO_FRAME();
    ECO_LOG_INFO("in frame 2");
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"f\":" + std::to_string(before + 1) + ",\"lv\":\"info\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"f\":" + std::to_string(before + 2) + ",\"lv\":\"info\""), 1u);
}

TEST(Logger, RotatesFilesAndKeepsTheNewest) {
    TestConfig test;
    test.config.maxFileBytes = 8 * 1024;
    test.config.maxFiles = 3;
    {
        Session session(test.config);
        for (int i = 0; i < 3000; ++i) {
            ECO_LOG_INFO("rotation {i}", i);
            if (i % 100 == 0) lg::writeOut();
        }
    }
    const auto files = logFiles(lgtest::pathOf(test.directory));
    EXPECT_EQ(files.size(), 3u);
    for (const auto& file : files) {
        const auto lines = readLines(utf8(file).c_str());
        ASSERT_FALSE(lines.empty());
        EXPECT_NE(lines.front().find("\"event\":\"session.start\""), std::string::npos);
    }
}

namespace {
ECO_LOG_TYPE(Gameplay, "gameplay");
}

TEST(Logger, DestinationsSplitLogsIntoSeparateFolders) {
    TestConfig test;
    const lg::Destination destinations[] = {
        {.name = "human", .level = lg::Level::Info},
        {.name = "game", .types = "gameplay"},
        {.name = "all"},
    };
    test.config.destinations = destinations;
    test.config.destinationCount = 3;
    std::string human, game, all;
    {
        Session session(test.config);
        ECO_LOG_DEBUG("debug detail");
        ECO_LOG_INFO("player joined");
        ECO_LOG(Info | Gameplay, "score {score}", 100);
        human = lg::currentFile("human");
        game = lg::currentFile("game");
        all = lg::currentFile("all");
    }
    EXPECT_EQ(lgtest::pathOf(human).parent_path(), lgtest::pathOf(test.directory) / "human");
    const auto humanLines = readLines(human.c_str());
    EXPECT_EQ(countContaining(humanLines, "player joined"), 1u);
    EXPECT_EQ(countContaining(humanLines, "score 100"), 1u);
    EXPECT_EQ(countContaining(humanLines, "debug detail"), 0u);
    EXPECT_EQ(countContaining(humanLines, "\"event\":\"session.end\""), 1u);
    const auto gameLines = readLines(game.c_str());
    EXPECT_EQ(countContaining(gameLines, "score 100"), 1u);
    EXPECT_EQ(countContaining(gameLines, "player joined"), 0u);
    const auto allLines = readLines(all.c_str());
    EXPECT_EQ(countContaining(allLines, "debug detail"), 1u);
    EXPECT_EQ(countContaining(allLines, "score 100"), 1u);
}

TEST(Logger, StartTwiceFails) {
    TestConfig test;
    Session session(test.config);
    EXPECT_TRUE(lg::isRunning());
    EXPECT_FALSE(lg::start(test.config));
}
