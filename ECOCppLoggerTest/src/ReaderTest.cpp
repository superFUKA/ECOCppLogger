#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "ECOCppLogger/Reader.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::readLines;
using lgtest::readText;
using lgtest::Session;
using lgtest::TestConfig;

TEST(Reader, WritesHumanReadableText) {
    TestConfig test;
    std::string file;
    {
        Session session(test.config);
        lg::setThreadName("main");
        ECO_FRAME();
        ECO_LOG(Warn | "physics", "hp が範囲外 hp={hp} max={max}", -5, 100);
        file = session.file();
    }
    const auto lines = readLines(file.c_str(), lg::ViewFormat::Text);
    const std::string* line = findContaining(lines, "hp が範囲外");
    ASSERT_NE(line, nullptr);
    EXPECT_EQ((*line)[2], ':') << *line;  // 13:20:34.729352 …
    EXPECT_NE(line->find(" WARN  #physics [main] ReaderTest.cpp:"), std::string::npos) << *line;
    EXPECT_NE(line->find(": hp が範囲外 hp=-5 max=100"), std::string::npos) << *line;
    EXPECT_EQ(countContaining(lines, " INFO  event=session.start"), 1u);
}

TEST(Reader, FiltersByLevelThreadTypeTextAndFrame) {
    TestConfig test;
    std::string file;
    {
        Session session(test.config);
        lg::setThreadName("main");
        ECO_FRAME();
        const uint64_t frame = lg::frame();
        ECO_LOG_INFO("info line {f}", frame);
        ECO_LOG(Error | "physics", "error line");
        ECO_FRAME();
        ECO_LOG_INFO("next frame");
        std::thread([] {
            lg::setThreadName("worker");
            ECO_LOG_ERROR("worker error");
        }).join();
        file = session.file();
    }
    lg::ViewFilter errors;
    errors.level = lg::Level::Error;
    const std::string errorText = readText(file.c_str(), lg::ViewFormat::Text, errors);
    EXPECT_EQ(errorText.find("info line"), std::string::npos);
    EXPECT_NE(errorText.find("error line"), std::string::npos);
    EXPECT_NE(errorText.find("worker error"), std::string::npos);

    lg::ViewFilter worker;
    worker.threads = "worker";
    const std::string workerText = readText(file.c_str(), lg::ViewFormat::Text, worker);
    EXPECT_EQ(workerText.find("error line"), std::string::npos);
    EXPECT_NE(workerText.find("worker error"), std::string::npos);

    lg::ViewFilter physics;
    physics.types = "physics";
    const std::string physicsText = readText(file.c_str(), lg::ViewFormat::Text, physics);
    EXPECT_NE(physicsText.find("error line"), std::string::npos);
    EXPECT_EQ(physicsText.find("info line"), std::string::npos);
    EXPECT_EQ(physicsText.find("session.start"), std::string::npos);  // event も型として外れる

    lg::ViewFilter text;
    text.contains = "info";
    const std::string found = readText(file.c_str(), lg::ViewFormat::Text, text);
    EXPECT_NE(found.find("info line"), std::string::npos);
    EXPECT_EQ(found.find("error line"), std::string::npos);
}

TEST(Reader, OmitsTimeWhenNotRecorded) {
    TestConfig test;
    test.config.recordTime = false;
    std::string file;
    {
        Session session(test.config);
        for (int i = 0; i < 100; ++i) {
            ECO_LOG_INFO("no time {i}", i);
        }
        file = session.file();
    }
    const auto json = readLines(file.c_str());
    const std::string* line = findContaining(json, "\"msg\":\"no time 0\"");
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->rfind("{\"lv\":\"info\"", 0), 0u) << *line;
    size_t next = 0;
    for (const auto& each : json) {
        if (each.find("\"msg\":\"no time ") != std::string::npos) {
            EXPECT_NE(each.find("\"msg\":\"no time " + std::to_string(next) + "\""), std::string::npos) << each;
            ++next;
        }
    }
    EXPECT_EQ(next, 100u);
    const auto text = readLines(file.c_str(), lg::ViewFormat::Text);
    const std::string* textLine = findContaining(text, "no time 0");
    ASSERT_NE(textLine, nullptr);
    EXPECT_EQ(textLine->rfind("--:--:--.------ INFO ", 0), 0u) << *textLine;
}

TEST(Reader, ReadsAFileWhileItIsBeingWritten) {
    TestConfig test;
    Session session(test.config);
    for (int i = 0; i < 100; ++i) {
        ECO_LOG_INFO("live {i}", i);
    }
    lg::writeOut();
    std::ifstream input(lgtest::pathOf(session.file()), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    lg::LogReader reader(lg::ViewFormat::Json);
    std::string out;
    for (size_t offset = 0; offset < bytes.size(); offset += 7) {  // 書きかけで切れていても読める
        reader.feed(bytes.data() + offset, std::min<size_t>(7, bytes.size() - offset));
        size_t size = 0;
        const char* text = reader.take(size);
        out.append(text, size);
    }
    EXPECT_FALSE(reader.invalid());
    EXPECT_EQ(countContaining(lgtest::splitLines(out), "\"msg\":\"live "), 100u);
}

TEST(Reader, RejectsOtherFiles) {
    lg::LogReader reader(lg::ViewFormat::Json);
    const std::string text = "{\"not\":\"ecolog\"}\n";
    reader.feed(text.data(), text.size());
    EXPECT_TRUE(reader.invalid());
}

TEST(Reader, FindsTheLatestLog) {
    TestConfig test;
    Session session(test.config);
    const char* latest = lg::latestLog(test.directory.c_str());
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(std::string(latest), session.file());
}
