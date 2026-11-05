#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::Session;
using lgtest::TestConfig;

namespace {

ECO_LOG_TYPE(Physics, "physics");

enum class Color { Red, Green };

void logFromNamedFunction() {
    ECO_LOG_INFO("from function");
}

}  // namespace

TEST(ECOCppLogger, FormatsEachArgumentType) {
    TestConfig test;
    Session session(test.config);
    const std::string text = "abc";
    const std::string_view view = "view";
    const void* pointer = reinterpret_cast<const void*>(0x1234);
    ECO_LOG_INFO("i={} u={} d={} b={} c={} lit={} sv={} str={} e={} neg={} p={}", 42, 7u, 1.5, true, 'x', "lit", view,
                 text, Color::Green, -3, pointer);
    const auto lines = session.lines();
    const std::string* line = findContaining(lines, "\"msg\":\"i=");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("\"msg\":\"i=42 u=7 d=1.5 b=true c=x lit=lit sv=view str=abc e=1 neg=-3 p=0x1234\""),
              std::string::npos)
        << *line;
    EXPECT_EQ(line->find("\"args\""), std::string::npos);  // 名前がなければ args は付けない
    EXPECT_NE(line->find("\"src\":\"ECOCppLoggerTest.cpp:"), std::string::npos);
}

TEST(ECOCppLogger, NamedValuesBecomeArgs) {
    TestConfig test;
    Session session(test.config);
    ECO_LOG_INFO("player {id} hp={hp} name={name} alive={alive}", 7, -5, "goblin", false);
    const auto lines = session.lines();
    const std::string* line = findContaining(lines, "\"msg\":\"player 7");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("\"msg\":\"player 7 hp=-5 name=goblin alive=false\""), std::string::npos) << *line;
    EXPECT_NE(line->find("\"args\":{\"id\":7,\"hp\":-5,\"name\":\"goblin\",\"alive\":false}"), std::string::npos)
        << *line;
}

TEST(ECOCppLogger, FormatSpecs) {
    TestConfig test;
    Session session(test.config);
    ECO_LOG_INFO("ratio={ratio:.3} id={id:x} ID={:X} code={code:08} pad={:5}", 0.123456, 255, 255, 42, 7);
    const auto lines = session.lines();
    const std::string* line = findContaining(lines, "\"msg\":\"ratio=");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("\"msg\":\"ratio=0.123 id=ff ID=FF code=00000042 pad=    7\""), std::string::npos) << *line;
    // JSON の値は書式を付けない元の値
    EXPECT_NE(line->find("\"args\":{\"ratio\":0.123456,\"id\":255,\"code\":42}"), std::string::npos) << *line;
}

TEST(ECOCppLogger, WritesFunctionName) {
    TestConfig test;
    Session session(test.config);
    logFromNamedFunction();
    [] { ECO_LOG_INFO("from lambda"); }();
    const auto lines = session.lines();
    const std::string* line = findContaining(lines, "from function");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("logFromNamedFunction\""), std::string::npos) << *line;
    const std::string* lambda = findContaining(lines, "from lambda");
    ASSERT_NE(lambda, nullptr);
    EXPECT_NE(lambda->find("operator"), std::string::npos) << *lambda;
}

TEST(ECOCppLogger, TypesWithLevel) {
    TestConfig test;
    Session session(test.config);
    ECO_LOG(Warn | Physics, "penetration depth={depth}", 0.25);
    ECO_LOG(Debug | "ai", "path {steps} steps", 12);
    ECO_LOG(Info, "plain");
    const auto lines = session.lines();
    const std::string* physics = findContaining(lines, "penetration");
    ASSERT_NE(physics, nullptr);
    EXPECT_NE(physics->find("\"lv\":\"warn\",\"type\":\"physics\""), std::string::npos) << *physics;
    const std::string* ai = findContaining(lines, "path 12 steps");
    ASSERT_NE(ai, nullptr);
    EXPECT_NE(ai->find("\"lv\":\"debug\",\"type\":\"ai\""), std::string::npos) << *ai;
    const std::string* plain = findContaining(lines, "\"msg\":\"plain\"");
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->find("\"type\""), std::string::npos) << *plain;
}

TEST(ECOCppLogger, EscapesJsonAndBraces) {
    TestConfig test;
    Session session(test.config);
    ECO_LOG_INFO("{{literal}} {}", "quote\" newline\n backslash\\ tab\t");
    const auto lines = session.lines();
    const std::string_view expected = R"("msg":"{literal} quote\" newline\n backslash\\ tab\t")";
    EXPECT_EQ(countContaining(lines, expected), 1u);
}

TEST(ECOCppLogger, KeepsUtf8) {
    TestConfig test;
    Session session(test.config);
    ECO_LOG_WARN("体力が {hp} になった（{by}）", 0, "敵");
    EXPECT_EQ(countContaining(session.lines(), "\"msg\":\"体力が 0 になった（敵）\""), 1u);
}

TEST(ECOCppLogger, SkipsLevelsBelowTheThreshold) {
    TestConfig test;
    Session session(test.config);
    lg::setLevel(lg::Level::Warn);
    bool evaluated = false;
    ECO_LOG_INFO("hidden {}", (evaluated = true));
    ECO_LOG_WARN("shown");
    const auto lines = session.lines();
    EXPECT_FALSE(evaluated);  // 重さが足りなければ引数も評価しない
    EXPECT_EQ(countContaining(lines, "hidden"), 0u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"shown\""), 1u);
}
