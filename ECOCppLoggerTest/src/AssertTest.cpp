#include <gtest/gtest.h>

#include <vector>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::Session;
using lgtest::TestConfig;

namespace {

struct AssertConfig : TestConfig {
    AssertConfig() { config.assertAction = lg::AssertAction::Continue; }
};

void checkHealth(int hp) {
    ECO_ASSERT(hp >= 0, "hp={hp}", hp);
}

const int* findTexture(const int* texture) {
    if (!ECO_CHECK(texture != nullptr)) {
        return nullptr;
    }
    return texture;
}

}  // namespace

TEST(Assert, FormatKeepsBracesOfTheCondition) {
    constexpr auto format = lg::detail::assertFormat("v == T{1}", "x={x}");
    EXPECT_STREQ(format.data(), "assertion failed: v == T{{1}}: x={x}");
    constexpr auto plain = lg::detail::assertFormat("ok", "");
    EXPECT_STREQ(plain.data(), "assertion failed: ok");
}

TEST(Assert, WritesLogStackAndHistory) {
    AssertConfig test;
    Session session(test.config);
    for (int i = 0; i < 3; ++i) {
        ECO_STAT_HISTORY("assert.hp", 8, 10 - i * 5);
    }
    checkHealth(5);
    checkHealth(-5);
    const auto lines = session.lines();
    const std::string* line = findContaining(lines, "\"msg\":\"assertion failed: hp >= 0: hp=-5\"");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("\"lv\":\"error\",\"type\":\"assert\""), std::string::npos) << *line;
    EXPECT_NE(line->find("checkHealth"), std::string::npos) << *line;
    EXPECT_NE(line->find("\"args\":{\"hp\":-5},\"sample\":\"first 10\",\"n\":1"), std::string::npos) << *line;
    const std::string* stack = findContaining(lines, "\"event\":\"stack\"");
    ASSERT_NE(stack, nullptr);
    EXPECT_NE(stack->find("\"reason\":\"assert AssertTest.cpp:"), std::string::npos) << *stack;
    EXPECT_NE(stack->find("checkHealth"), std::string::npos) << *stack;
    const std::string* history = findContaining(lines, "\"lv\":\"history\",\"name\":\"assert.hp\"");
    ASSERT_NE(history, nullptr);
    EXPECT_NE(history->find("\"reason\":\"assert AssertTest.cpp:"), std::string::npos) << *history;
}

TEST(Assert, CheckIsAnExpressionAndNeverStops) {
    TestConfig test;
    test.config.assertAction = lg::AssertAction::Abort;  // ECO_CHECK は動きに関係なく続ける
    Session session(test.config);
    const int texture = 1;
    EXPECT_EQ(findTexture(&texture), &texture);
    EXPECT_EQ(findTexture(nullptr), nullptr);
    EXPECT_EQ(countContaining(session.lines(), "\"msg\":\"assertion failed: texture != nullptr\""), 1u);
}

TEST(Assert, OnlyTheFirstFailuresAreDetailed) {
    AssertConfig test;
    std::string file;
    {
        Session session(test.config);
        for (int i = 0; i < 25; ++i) {
            ECO_ASSERT(i < 0);
        }
        file = session.file();
    }
    const auto lines = lgtest::readLines(file.c_str());
    EXPECT_EQ(countContaining(lines, "\"msg\":\"assertion failed: i < 0\""), 10u);
    EXPECT_EQ(countContaining(lines, "\"name\":\"assert: i < 0\",\"kind\":\"count\",\"n\":25"), 1u);
    EXPECT_EQ(countContaining(lines, "\"event\":\"stack\""), 1u);  // スタックは最初の1回だけ
}

TEST(Assert, VerifyEvaluatesTheCondition) {
    AssertConfig test;
    Session session(test.config);
    int calls = 0;
    ECO_VERIFY(++calls == 1);
    EXPECT_EQ(calls, 1);
}

TEST(AssertDeathTest, AbortWritesTheLogFirst) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    TestConfig test;
    test.config.assertAction = lg::AssertAction::Abort;
    const std::string directory = test.directory;
    EXPECT_DEATH(
        {
            lg::start(test.config);
            ECO_LOG_INFO("before abort");
            ECO_ASSERT(1 + 1 == 3, "math");
        },
        "");
    const char* latest = lg::latestLog(directory.c_str());
    ASSERT_NE(latest, nullptr);
    const auto lines = lgtest::readLines(latest);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"before abort\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"assertion failed: 1 + 1 == 3: math\""), 1u);
}
