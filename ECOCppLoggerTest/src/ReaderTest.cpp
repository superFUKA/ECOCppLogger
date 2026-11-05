#include <gtest/gtest.h>

#include <fstream>
#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "ECOCppLogger/Reader.h"
#include "TestSession.h"

using eco::log::Level;
using eco::log::ViewFormat;
using eco::log::test::countContaining;
using eco::log::test::findContaining;
using eco::log::test::readLines;
using eco::log::test::Session;
using eco::log::test::testConfig;

TEST(Reader, WritesHumanReadableText) {
    std::filesystem::path file;
    {
        Session session;
        eco::log::setThreadName("main");
        ECO_FRAME();
        ECO_LOG_WARN("hp が範囲外 hp={} max={}", -5, 100);
        file = eco::log::currentFile();
    }
    const auto lines = readLines(file, ViewFormat::Text);
    const std::string* line = findContaining(lines, "hp が範囲外");
    ASSERT_NE(line, nullptr);
    // 13:20:34.729352 f1 WARN  [main] ReaderTest.cpp:24 Reader_WritesHumanReadableText_Test::TestBody: hp が範囲外 …
    EXPECT_EQ((*line)[2], ':') << *line;
    EXPECT_NE(line->find(" WARN  [main] ReaderTest.cpp:"), std::string::npos) << *line;
    EXPECT_NE(line->find(": hp が範囲外 hp=-5 max=100"), std::string::npos) << *line;
    EXPECT_EQ(countContaining(lines, " INFO  event=session.start"), 1u);
}

TEST(Reader, FiltersByLevelThreadAndText) {
    std::filesystem::path file;
    {
        Session session;
        eco::log::setThreadName("main");
        ECO_LOG_INFO("info line");
        ECO_LOG_ERROR("error line");
        std::thread([] {
            eco::log::setThreadName("worker");
            ECO_LOG_ERROR("worker error");
        }).join();
        file = eco::log::currentFile();
    }
    eco::log::ViewFilter filter;
    filter.level = Level::Error;
    const std::string errors = eco::log::readLog(file, ViewFormat::Text, filter);
    EXPECT_EQ(errors.find("info line"), std::string::npos);
    EXPECT_NE(errors.find("error line"), std::string::npos);
    EXPECT_NE(errors.find("worker error"), std::string::npos);

    filter.thread = "worker";
    const std::string worker = eco::log::readLog(file, ViewFormat::Text, filter);
    EXPECT_EQ(worker.find("error line"), std::string::npos);
    EXPECT_NE(worker.find("worker error"), std::string::npos);

    eco::log::ViewFilter text;
    text.contains = "info";
    const std::string found = eco::log::readLog(file, ViewFormat::Text, text);
    EXPECT_NE(found.find("info line"), std::string::npos);
    EXPECT_EQ(found.find("error line"), std::string::npos);
}

namespace {
ECO_LOG_TYPE(AiLog, "ai");
}

TEST(Reader, FiltersByLogType) {
    std::filesystem::path file;
    {
        Session session;
        ECO_LOG_INFO("plain");
        ECO_LOG_TYPED("physics", Level::Warn, "penetration depth={}", 0.25);
        ECO_LOG_TYPED(AiLog, Level::Debug, "path {} steps", 12);
        ECO_STAT("typed.stat", 1);
        eco::log::flushStats();
        file = eco::log::currentFile();
    }
    const auto all = readLines(file);
    const std::string* physics = findContaining(all, "penetration");
    ASSERT_NE(physics, nullptr);
    EXPECT_NE(physics->find("\"lv\":\"warn\",\"type\":\"physics\",\"th\":"), std::string::npos) << *physics;
    const std::string* plain = findContaining(all, "\"msg\":\"plain\"");
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->find("\"type\""), std::string::npos) << *plain;  // 型なしは書かない

    eco::log::ViewFilter only;
    only.types = {"physics", "ai"};
    const std::string typed = eco::log::readLog(file, ViewFormat::Text, only);
    EXPECT_NE(typed.find("WARN  #physics ["), std::string::npos) << typed;
    EXPECT_NE(typed.find("DEBUG #ai ["), std::string::npos) << typed;
    EXPECT_EQ(typed.find("plain"), std::string::npos);
    EXPECT_EQ(typed.find("typed.stat"), std::string::npos);
    EXPECT_EQ(typed.find("session.start"), std::string::npos);

    eco::log::ViewFilter exclude;
    exclude.excludeTypes = {"ai", "event"};
    const std::string rest = eco::log::readLog(file, ViewFormat::Text, exclude);
    EXPECT_NE(rest.find("plain"), std::string::npos);
    EXPECT_NE(rest.find("penetration"), std::string::npos);
    EXPECT_NE(rest.find("typed.stat"), std::string::npos);
    EXPECT_EQ(rest.find("path 12 steps"), std::string::npos);
    EXPECT_EQ(rest.find("session.start"), std::string::npos);
}

TEST(Reader, OmitsTimeWhenNotRecorded) {
    auto config = testConfig();
    config.recordTime = false;
    config.recordFrame = false;
    std::filesystem::path file;
    {
        Session session(config);
        ECO_FRAME();
        for (int i = 0; i < 100; ++i) {
            ECO_LOG_INFO("no time {}", i);
        }
        file = eco::log::currentFile();
    }
    const auto json = readLines(file);
    const std::string* line = findContaining(json, "\"msg\":\"no time 0\"");
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->rfind("{\"lv\":\"info\"", 0), 0u) << *line;  // "t" も "f" もない
    // 同じスレッドの中の順番は保たれる
    std::size_t next = 0;
    for (const auto& each : json) {
        if (each.find("\"msg\":\"no time ") != std::string::npos) {
            EXPECT_NE(each.find("\"msg\":\"no time " + std::to_string(next) + "\""), std::string::npos) << each;
            ++next;
        }
    }
    EXPECT_EQ(next, 100u);
    const auto text = readLines(file, ViewFormat::Text);
    const std::string* textLine = findContaining(text, "no time 0");
    ASSERT_NE(textLine, nullptr);
    EXPECT_EQ(textLine->rfind("--:--:--.------ INFO ", 0), 0u) << *textLine;
}

TEST(Reader, ReadsAFileWhileItIsBeingWritten) {
    Session session;
    for (int i = 0; i < 100; ++i) {
        ECO_LOG_INFO("live {}", i);
    }
    eco::log::flush();
    // 動いている最中のファイルを、少しずつ（書きかけで切れていても）読む
    std::ifstream input(eco::log::currentFile(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    eco::log::LogReader reader(ViewFormat::Json);
    std::string out;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 7) {
        reader.feed(bytes.data() + offset, std::min<std::size_t>(7, bytes.size() - offset));
        reader.take(out);
    }
    EXPECT_FALSE(reader.invalid());
    std::size_t count = 0;
    for (std::size_t at = out.find("\"msg\":\"live "); at != std::string::npos; at = out.find("\"msg\":\"live ", at + 1)) {
        ++count;
    }
    EXPECT_EQ(count, 100u);
}

TEST(Reader, RejectsOtherFiles) {
    eco::log::LogReader reader(ViewFormat::Json);
    const std::string text = "{\"not\":\"ecolog\"}\n";
    reader.feed(text.data(), text.size());
    EXPECT_TRUE(reader.invalid());
}
