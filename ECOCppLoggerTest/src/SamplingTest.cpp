#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::Session;
using lgtest::TestConfig;

namespace {

int g_evaluated = 0;

int counted(int value) {
    ++g_evaluated;
    return value;
}

enum class State { Idle, Run, Jump };

}  // namespace

TEST(Sampling, LogIfWritesOnlyWhenTrue) {
    TestConfig test;
    Session session(test.config);
    for (int i = 0; i < 10; ++i) {
        ECO_LOG_IF(Info, i % 5 == 0, "if {i}", i);
    }
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"msg\":\"if "), 2u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"if 5\""), 1u);
}

TEST(Sampling, EveryNCountsPerThreadAndSkipsArguments) {
    TestConfig test;
    Session session(test.config);
    g_evaluated = 0;
    for (int i = 0; i < 1000; ++i) {
        ECO_LOG_EVERY_N(Debug, 100, "every {i}", counted(i));
    }
    EXPECT_EQ(g_evaluated, 10);  // 書かない回は引数を評価しない
    std::thread([] {
        for (int i = 0; i < 250; ++i) {
            ECO_LOG_EVERY_N(Debug, 100, "worker every {i}", i);
        }
    }).join();
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"msg\":\"every "), 10u);
    const std::string* second = findContaining(lines, "\"msg\":\"every 100\"");
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second->find("\"sample\":\"every 100\",\"n\":101"), std::string::npos) << *second;
    EXPECT_EQ(countContaining(lines, "\"msg\":\"worker every "), 3u);  // 0・100・200（スレッドごとに数える）
}

TEST(Sampling, FirstNAndOnce) {
    TestConfig test;
    Session session(test.config);
    for (int i = 0; i < 10; ++i) {
        ECO_LOG_FIRST_N(Warn, 3, "first {i}", i);
        ECO_LOG_ONCE(Info, "once {i}", i);
    }
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"msg\":\"first "), 3u);
    EXPECT_EQ(countContaining(lines, "\"sample\":\"first 3\",\"n\":3"), 1u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"once "), 1u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"once 0\""), 1u);
}

TEST(Sampling, EveryMsReportsSkipped) {
    TestConfig test;
    Session session(test.config);
    const auto start = std::chrono::steady_clock::now();
    int calls = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(120)) {
        ECO_LOG_EVERY_MS(Info, 50, "tick {calls}", calls);
        ++calls;
    }
    const auto lines = session.lines();
    const size_t written = countContaining(lines, "\"msg\":\"tick ");
    EXPECT_GE(written, 2u);
    EXPECT_LE(written, 4u);
    const std::string* second = findContaining(lines, "\"sample\":\"every 50ms\",\"skipped\":");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(countContaining(lines, "\"skipped\":0}"), 1u);  // 最初の1回だけ省いた数が 0
}

TEST(Sampling, OnChangeWritesFirstValueAndChanges) {
    TestConfig test;
    Session session(test.config);
    const State states[] = {State::Idle, State::Idle, State::Run, State::Run, State::Run, State::Jump, State::Idle};
    for (State state : states) {
        ECO_LOG_ON_CHANGE(Info, "player.state", state);
    }
    double ratio = 0.5;
    for (int i = 0; i < 3; ++i) {
        ECO_LOG_ON_CHANGE(Debug, "ratio", ratio);
    }
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"msg\":\"player.state"), 4u);
    const std::string* first = findContaining(lines, "\"msg\":\"player.state = 0\"");
    ASSERT_NE(first, nullptr);
    EXPECT_NE(first->find("\"args\":{\"value\":0},\"sample\":\"on change\",\"change\":0"), std::string::npos) << *first;
    const std::string* change = findContaining(lines, "\"msg\":\"player.state: 1 -> 2\"");
    ASSERT_NE(change, nullptr);
    EXPECT_NE(change->find("\"args\":{\"from\":1,\"to\":2}"), std::string::npos) << *change;
    EXPECT_NE(change->find("\"change\":2"), std::string::npos) << *change;
    EXPECT_EQ(countContaining(lines, "\"msg\":\"ratio"), 1u);
}

TEST(Sampling, OnChangeRemembersPerThread) {
    TestConfig test;
    Session session(test.config);
    auto run = [] {
        for (int value : {1, 1, 2}) {
            ECO_LOG_ON_CHANGE(Info, "shared", value);
        }
    };
    run();
    std::thread(run).join();
    EXPECT_EQ(countContaining(session.lines(), "\"msg\":\"shared"), 4u);  // スレッドごとに 最初の値＋変化1回
}

TEST(Sampling, TextShowsSampling) {
    TestConfig test;
    std::string file;
    {
        Session session(test.config);
        for (int i = 0; i < 3; ++i) {
            ECO_LOG_EVERY_N(Info, 2, "text every {i}", i);
        }
        file = session.file();
    }
    const auto lines = lgtest::readLines(file.c_str(), lg::ViewFormat::Text);
    EXPECT_EQ(countContaining(lines, ": text every 2  (every 2, #3)"), 1u);
}
