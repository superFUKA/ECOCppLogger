#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "ECOCppLogger/Stat.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::Session;
using lgtest::TestConfig;

TEST(Stat, HistoryRingKeepsTheNewestValues) {
    lg::HistoryRing ring(3);
    for (int i = 1; i <= 5; ++i) {
        ring.push(i, static_cast<uint64_t>(i), 0);
    }
    std::vector<double> values;
    ring.read([&](const lg::HistoryRing::Point& point) { values.push_back(point.value); });
    EXPECT_EQ(values, (std::vector<double>{3, 4, 5}));
}

TEST(Stat, WritesValuesOncePerInterval) {
    TestConfig test;
    std::string file;
    {
        Session session(test.config);
        for (int i = 1; i <= 100; ++i) {
            ECO_STAT("frame.objects", i);
        }
        std::thread([] {
            for (int i = 0; i < 10; ++i) {
                ECO_STAT("frame.objects", 1000);  // 別のスレッドの分も合わせる
            }
        }).join();
        for (int i = 0; i < 7; ++i) {
            ECO_STAT_COUNT("spawn");
        }
        file = session.file();
    }  // stop() で残りを出す
    const auto lines = lgtest::readLines(file.c_str());
    const std::string* objects = findContaining(lines, "\"name\":\"frame.objects\"");
    ASSERT_NE(objects, nullptr);
    EXPECT_NE(objects->find("\"lv\":\"stat\""), std::string::npos) << *objects;
    EXPECT_NE(objects->find("\"kind\":\"value\",\"n\":110,\"avg\":136.818,\"min\":1,\"max\":1000,"), std::string::npos)
        << *objects;
    EXPECT_NE(objects->find("\"src\":\"StatTest.cpp:"), std::string::npos) << *objects;
    const std::string* spawn = findContaining(lines, "\"name\":\"spawn\"");
    ASSERT_NE(spawn, nullptr);
    EXPECT_NE(spawn->find("\"kind\":\"count\",\"n\":7,\"per_s\":"), std::string::npos) << *spawn;
    const auto text = lgtest::readLines(file.c_str(), lg::ViewFormat::Text);
    EXPECT_EQ(countContaining(text, " STAT  name=frame.objects kind=value n=110"), 1u);
}

TEST(Stat, OnlyNewValuesInTheNextInterval) {
    TestConfig test;
    test.config.statIntervalMs = 0;  // writeOut() のたびに出す
    Session session(test.config);
    for (int i = 0; i < 3; ++i) {
        ECO_STAT("interval.value", 10);
    }
    lg::writeOut();
    ECO_STAT("interval.value", 2);
    lg::writeOut();
    lg::writeOut();  // 値がなければ出さない
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"name\":\"interval.value\""), 2u);
    EXPECT_EQ(countContaining(lines, "\"n\":3,\"avg\":10,\"min\":10,\"max\":10"), 1u);
    EXPECT_EQ(countContaining(lines, "\"n\":1,\"avg\":2,\"min\":2,\"max\":2"), 1u);
}

TEST(Stat, KeepsValuesOfFinishedThreads) {
    TestConfig test;
    test.config.statIntervalMs = 60'000;
    Session session(test.config);
    std::thread([] { ECO_STAT("finished.thread", 5); }).join();
    lg::writeOut();  // 終わったスレッドのバッファは片付くが、集計は取っておく
    const std::string file = session.file();
    lg::stop();
    EXPECT_EQ(countContaining(lgtest::readLines(file.c_str()), "\"name\":\"finished.thread\",\"kind\":\"value\",\"n\":1"),
              1u);
}

TEST(Stat, MeasuresScopeTime) {
    TestConfig test;
    std::string file;
    {
        Session session(test.config);
        for (int i = 0; i < 3; ++i) {
            ECO_STAT_TIME("sleep");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        file = session.file();
    }
    const auto lines = lgtest::readLines(file.c_str());
    const std::string* line = findContaining(lines, "\"name\":\"sleep\"");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("\"kind\":\"time\",\"n\":3,\"unit\":\"us\""), std::string::npos) << *line;
    const size_t at = line->find("\"min\":");
    ASSERT_NE(at, std::string::npos);
    EXPECT_GE(std::stod(line->substr(at + 6)), 1500.0) << *line;  // 2ms 眠った（マイクロ秒）
}

TEST(Stat, DumpsRecentHistory) {
    TestConfig test;
    Session session(test.config);
    for (int i = 1; i <= 10; ++i) {
        ECO_FRAME();
        ECO_STAT_HISTORY("player.hp", 4, 100 - i);
    }
    lg::dumpHistory("hp dropped");
    ECO_STAT_HISTORY("player.hp", 4, -1);  // 呼んだ後の値は含めない
    const auto lines = session.lines();
    const std::string* line = findContaining(lines, "\"lv\":\"history\"");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(line->find("\"name\":\"player.hp\",\"th\":"), std::string::npos) << *line;
    EXPECT_NE(line->find("\"reason\":\"hp dropped\""), std::string::npos) << *line;
    size_t points = 0;
    for (size_t at = line->find("\"v\":"); at != std::string::npos; at = line->find("\"v\":", at + 1)) ++points;
    EXPECT_EQ(points, 4u);
    EXPECT_NE(line->find("\"v\":90}]"), std::string::npos) << *line;
    EXPECT_EQ(line->find("\"v\":-1"), std::string::npos) << *line;
}
