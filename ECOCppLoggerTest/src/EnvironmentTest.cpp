#include <gtest/gtest.h>

#include <regex>
#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::Session;
using lgtest::TestConfig;

TEST(Environment, WritesTheEnvironmentOnce) {
    TestConfig test;
    test.config.app = {.name = "TestGame", .version = "1.2.3", .commit = "abc1234"};
    Session session(test.config);
    const auto lines = session.lines();
    const std::string* environment = findContaining(lines, "\"event\":\"environment\"");
    ASSERT_NE(environment, nullptr);
    EXPECT_NE(environment->find("\"app\":{\"name\":\"TestGame\",\"version\":\"1.2.3\",\"commit\":\"abc1234\"}"),
              std::string::npos)
        << *environment;
    for (const char* key : {"\"args\":", "\"cwd\":", "\"build\":", "\"compiler\":", "\"os\":\"Windows ", "\"host\":",
                            "\"cpu\":", "\"cores\":", "\"memory_mb\":"}) {
        EXPECT_NE(environment->find(key), std::string::npos) << key << " in " << *environment;
    }
    EXPECT_EQ(countContaining(lines, "\"event\":\"environment\""), 1u);
}

TEST(Environment, RunIdIsAUuidInSessionStart) {
    TestConfig test;
    Session session(test.config);
    const std::string run = lg::runId();
    EXPECT_TRUE(std::regex_match(run, std::regex("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")))
        << run;
    EXPECT_EQ(countContaining(session.lines(), "\"event\":\"session.start\",\"run\":\"" + run + "\""), 1u);
}

TEST(Environment, SetInfoIsWrittenNowAndInLaterFiles) {
    TestConfig test;
    test.config.maxFileBytes = 4 * 1024;
    test.config.maxFiles = 10;
    lg::setInfo("before", "start");  // start() の前でもよい
    Session session(test.config);
    lg::setInfo("level", "forest_01");
    lg::setInfo("level", "forest_02");  // 置き換える
    for (int i = 0; i < 2000; ++i) {
        ECO_LOG_INFO("fill {i}", i);
        if (i % 100 == 0) lg::writeOut();
    }
    lg::writeOut();
    const std::string current = lg::currentFile();  // 今のファイル（最初のファイルではない）
    EXPECT_NE(current, session.file());
    const auto lines = lgtest::readLines(current.c_str());
    EXPECT_EQ(countContaining(lines, "\"event\":\"session.start\",\"run\":"), 1u);
    EXPECT_EQ(countContaining(lines, "\"event\":\"info\",\"key\":\"level\",\"value\":\"forest_02\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"key\":\"level\",\"value\":\"forest_01\""), 0u);
    EXPECT_EQ(countContaining(lines, "\"key\":\"before\",\"value\":\"start\""), 1u);
}

TEST(Environment, ThreadLinesMapNamesToOsIds) {
    TestConfig test;
    Session session(test.config);
    std::thread([] {
        lg::setThreadName("loader");
        ECO_LOG_INFO("loading");
    }).join();
    const auto lines = session.lines();
    const std::string* thread = findContaining(lines, "\"event\":\"thread\",\"th\":\"loader\",\"tid\":");
    ASSERT_NE(thread, nullptr);
    const std::string* loading = findContaining(lines, "\"msg\":\"loading\"");
    ASSERT_NE(loading, nullptr);
    EXPECT_LT(thread - lines.data(), loading - lines.data());  // ログより前に出る
}
