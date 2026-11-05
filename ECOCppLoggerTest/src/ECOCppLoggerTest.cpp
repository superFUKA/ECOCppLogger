#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using eco::log::Level;
using eco::log::test::countContaining;
using eco::log::test::findContaining;
using eco::log::test::Session;

namespace {

enum class Color { Red, Green };

void logFromNamedFunction() {
    ECO_LOG_INFO("from function");
}

}  // namespace

TEST(ECOCppLogger, FormatsEachArgumentType) {
    Session session;
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
    EXPECT_NE(line->find("\"lv\":\"info\""), std::string::npos);
    EXPECT_NE(line->find("\"src\":\"ECOCppLoggerTest.cpp:"), std::string::npos);
    EXPECT_NE(line->find("\"t\":\"20"), std::string::npos);
}

TEST(ECOCppLogger, WritesFunctionName) {
    Session session;
    logFromNamedFunction();
    [] { ECO_LOG_INFO("from lambda"); }();
    const auto lines = session.lines();
    const std::string* line = findContaining(lines, "from function");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("logFromNamedFunction\""), std::string::npos) << *line;
    const std::string* lambda = findContaining(lines, "from lambda");
    ASSERT_NE(lambda, nullptr);
    EXPECT_NE(lambda->find("operator"), std::string::npos) << *lambda;  // ラムダでも空にならない
}

TEST(ECOCppLogger, EscapesJsonAndBraces) {
    Session session;
    ECO_LOG_INFO("{{literal}} {}", "quote\" newline\n backslash\\ tab\t");
    const auto lines = session.lines();
    const std::string_view expected = R"("msg":"{literal} quote\" newline\n backslash\\ tab\t")";
    EXPECT_EQ(countContaining(lines, expected), 1u);
}

TEST(ECOCppLogger, KeepsUtf8) {
    Session session;
    ECO_LOG_WARN("体力が {} になった（{}）", 0, "敵");
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"msg\":\"体力が 0 になった（敵）\""), 1u);
}

TEST(ECOCppLogger, SkipsLevelsBelowTheThreshold) {
    Session session;
    eco::log::setLevel(Level::Warn);
    bool evaluated = false;
    ECO_LOG_INFO("hidden {}", (evaluated = true));
    ECO_LOG_WARN("shown");
    ECO_LOG_ERROR("shown too");
    const auto lines = session.lines();
    EXPECT_FALSE(evaluated);  // 重さが足りなければ引数も評価しない
    EXPECT_EQ(countContaining(lines, "hidden"), 0u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"shown\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"lv\":\"error\""), 1u);
}

TEST(ECOCppLogger, LogIfChecksCondition) {
    Session session;
    for (int i = 0; i < 10; ++i) {
        ECO_LOG_IF(Level::Info, i % 5 == 0, "multiple of five {}", i);
    }
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "multiple of five"), 2u);
}

TEST(ECOCppLogger, EveryNLogsOncePerN) {
    Session session;
    for (int i = 0; i < 10000; ++i) {
        ECO_LOG_EVERY_N(Level::Debug, 1000, "step {}", i);
    }
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"sample\":\"every 1000\""), 10u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"step 0\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"step 9000\",\"sample\":\"every 1000\",\"n\":9001"), 1u);
}

TEST(ECOCppLogger, FirstNAndOnce) {
    Session session;
    for (int i = 0; i < 100; ++i) {
        ECO_LOG_FIRST_N(Level::Info, 3, "first {}", i);
        ECO_LOG_ONCE(Level::Info, "once {}", i);
    }
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"msg\":\"first "), 3u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"first 2\",\"sample\":\"first 3\",\"n\":3"), 1u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"once "), 1u);
}

TEST(ECOCppLogger, EveryMsLimitsRateAndCountsSkipped) {
    Session session;
    const auto start = std::chrono::steady_clock::now();
    int calls = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(130)) {
        ECO_LOG_EVERY_MS(Level::Info, 50, "tick {}", calls);
        ++calls;
    }
    const auto lines = session.lines();
    const std::size_t logged = countContaining(lines, "\"sample\":\"every 50ms\"");
    EXPECT_GE(logged, 2u);
    EXPECT_LE(logged, 4u);
    EXPECT_EQ(countContaining(lines, "\"skipped\":0}"), 1u);  // 最初の1回だけ省いた数が 0
}

TEST(ECOCppLogger, AggregatesStats) {
    Session session;
    for (int i = 1; i <= 100; ++i) {
        ECO_STAT("test.value", i);
        if (i % 2 == 0) {
            ECO_COUNT("test.even");
        }
        ECO_SCOPE_TIME("test.scope");
    }
    const auto lines = session.linesWithStats();
    const std::string* value = findContaining(lines, "\"stat\":\"test.value\"");
    ASSERT_NE(value, nullptr);
    EXPECT_NE(value->find("\"n\":100,\"sum\":5050,\"avg\":50.5,\"min\":1,\"max\":100,\"last\":100,\"total\":100"),
              std::string::npos)
        << *value;
    const std::string* even = findContaining(lines, "\"stat\":\"test.even\"");
    ASSERT_NE(even, nullptr);
    EXPECT_NE(even->find("\"n\":50,\"total\":50"), std::string::npos) << *even;
    const std::string* scope = findContaining(lines, "\"stat\":\"test.scope\"");
    ASSERT_NE(scope, nullptr);
    EXPECT_NE(scope->find("\"unit\":\"us\""), std::string::npos) << *scope;
}

TEST(ECOCppLogger, StatsReportEachPeriodSeparately) {
    Session session;
    const auto add = [](int value) { ECO_STAT("test.period", value); };  // 同じ呼び出し位置
    for (int i = 1; i <= 10; ++i) {
        add(i);
    }
    eco::log::flushStats();
    for (int i = 100; i <= 101; ++i) {
        add(i);
    }
    const auto lines = session.linesWithStats();
    EXPECT_EQ(countContaining(lines, "\"stat\":\"test.period\""), 2u);
    EXPECT_EQ(countContaining(lines, "\"n\":2,\"sum\":201,\"avg\":100.5,\"min\":100,\"max\":101,\"last\":101,\"total\":12"),
              1u);
}

TEST(ECOCppLogger, StatsMergeAcrossThreads) {
    Session session;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 1000; ++i) {
                ECO_STAT("test.threads", t * 1000 + i);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto lines = session.linesWithStats();
    const std::string* line = findContaining(lines, "\"stat\":\"test.threads\"");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("\"n\":4000,"), std::string::npos) << *line;
    EXPECT_NE(line->find("\"min\":0,\"max\":3999,"), std::string::npos) << *line;
    EXPECT_NE(line->find("\"threads\":4"), std::string::npos) << *line;
}

TEST(ECOCppLogger, ManyThreadsKeepEveryRecordInOrder) {
    Session session;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            eco::log::setThreadName("worker" + std::to_string(t));
            for (int i = 0; i < kPerThread; ++i) {
                ECO_LOG_DEBUG("w{} {}", t, i);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"event\":\"dropped\""), 0u);
    std::vector<int> next(kThreads, 0);
    std::size_t records = 0;
    for (const auto& line : lines) {
        const auto position = line.find("\"msg\":\"w");
        if (position == std::string::npos) {
            continue;
        }
        int thread = 0;
        int index = 0;
        const char* cursor = line.c_str() + position + 8;  // "msg":"w の後
        cursor = std::from_chars(cursor, line.c_str() + line.size(), thread).ptr + 1;
        std::from_chars(cursor, line.c_str() + line.size(), index);
        ASSERT_LT(thread, kThreads);
        ASSERT_EQ(index, next[thread]) << "スレッドの中の順番が崩れた";
        ++next[thread];
        ++records;
        EXPECT_NE(line.find("\"th\":\"worker" + std::to_string(thread) + "\""), std::string::npos);
    }
    EXPECT_EQ(records, static_cast<std::size_t>(kThreads * kPerThread));
}

TEST(ECOCppLogger, RecordsFrameNumber) {
    Session session;
    const auto before = eco::log::frame();
    ECO_FRAME();
    ECO_FRAME();
    ECO_LOG_INFO("in frame");
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"f\":" + std::to_string(before + 2) + ","), 1u);
}

TEST(ECOCppLogger, CountsDroppedRecordsWhenBufferIsFull) {
    auto config = eco::log::test::testConfig();
    config.threadBufferBytes = 4096;
    {
        Session first(config);  // これから作るスレッドのバッファを 4096 バイトにする
    }
    // 裏のスレッドが止まっている間に書くと、バッファが一杯になって捨てられる。
    std::thread writer([] {
        for (int i = 0; i < 1000; ++i) {
            ECO_LOG_INFO("fill {}", i);
        }
    });
    writer.join();
    Session second(config);
    const auto lines = second.lines();
    const std::size_t kept = countContaining(lines, "\"msg\":\"fill ");
    const std::string* dropped = findContaining(lines, "\"event\":\"dropped\"");
    ASSERT_NE(dropped, nullptr);
    const auto position = dropped->find("\"count\":");
    ASSERT_NE(position, std::string::npos);
    const std::size_t count = std::stoul(dropped->substr(position + 8));
    EXPECT_GT(kept, 0u);
    EXPECT_EQ(kept + count, 1000u);
}
