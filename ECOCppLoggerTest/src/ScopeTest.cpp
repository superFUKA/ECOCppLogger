#include <gtest/gtest.h>

#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::Session;
using lgtest::TestConfig;

namespace {

void solve(int body) {
    ECO_SCOPE("Solve body={body}", body);
    ECO_LOG_WARN("penetration");
}

void physics() {
    ECO_SCOPE("Physics");
    solve(3);
    ECO_LOG_INFO("physics done");
}

void update() {
    ECO_SCOPE("Update");
    physics();
}

}  // namespace

TEST(Scope, LogsCarryTheScopePath) {
    TestConfig test;
    std::string file;
    {
        Session session(test.config);
        update();
        ECO_LOG_INFO("outside");
        file = session.file();
    }
    const auto lines = lgtest::readLines(file.c_str());
    const std::string* inner = findContaining(lines, "\"msg\":\"penetration\"");
    ASSERT_NE(inner, nullptr);
    EXPECT_NE(inner->find("\"scope\":\"Update > Physics > Solve body=3\""), std::string::npos) << *inner;
    const std::string* done = findContaining(lines, "\"msg\":\"physics done\"");
    ASSERT_NE(done, nullptr);
    EXPECT_NE(done->find("\"scope\":\"Update > Physics\""), std::string::npos) << *done;
    const std::string* outside = findContaining(lines, "\"msg\":\"outside\"");
    ASSERT_NE(outside, nullptr);
    EXPECT_EQ(outside->find("\"scope\""), std::string::npos) << *outside;

    const std::string* enter = findContaining(lines, "\"msg\":\"Solve body=3\",\"args\":{\"body\":3},\"scope_event\":\"enter\"");
    ASSERT_NE(enter, nullptr);
    EXPECT_NE(enter->find("\"lv\":\"debug\",\"type\":\"scope\""), std::string::npos) << *enter;
    EXPECT_NE(enter->find("\"scope\":\"Update > Physics\""), std::string::npos) << *enter;
    const std::string* exit = findContaining(lines, "\"msg\":\"Update\",\"scope_event\":\"exit\",\"us\":");
    ASSERT_NE(exit, nullptr);
    EXPECT_EQ(exit->find("\"scope\":\""), std::string::npos) << *exit;

    const auto text = lgtest::readLines(file.c_str(), lg::ViewFormat::Text);
    EXPECT_EQ(countContaining(text, "{Update > Physics > Solve body=3} ScopeTest.cpp:"), 1u);
    EXPECT_EQ(countContaining(text, ": enter Physics"), 1u);
    EXPECT_EQ(countContaining(text, ": exit Physics ("), 1u);
}

TEST(Scope, FiltersByScopeAndHidesScopeLines) {
    TestConfig test;
    std::string file;
    {
        Session session(test.config);
        update();
        {
            ECO_SCOPE("Render");
            ECO_LOG_INFO("draw");
        }
        file = session.file();
    }
    lg::ViewFilter physics;
    physics.scopes = "Physics";
    const auto lines = lgtest::readLines(file.c_str(), lg::ViewFormat::Json, physics);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"penetration\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"draw\""), 0u);

    lg::ViewFilter noScopeLines;
    noScopeLines.excludeTypes = "scope";
    const auto quiet = lgtest::readLines(file.c_str(), lg::ViewFormat::Json, noScopeLines);
    EXPECT_EQ(countContaining(quiet, "\"scope_event\""), 0u);
    const std::string* draw = findContaining(quiet, "\"msg\":\"draw\"");
    ASSERT_NE(draw, nullptr);
    EXPECT_NE(draw->find("\"scope\":\"Render\""), std::string::npos) << *draw;  // 行は消しても積み重ねは付く
}

TEST(Scope, NothingWhenTheLevelIsCut) {
    TestConfig test;
    Session session(test.config);
    lg::setLevel(lg::Level::Info);
    update();
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"scope_event\""), 0u);
    const std::string* inner = findContaining(lines, "\"msg\":\"penetration\"");
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->find("\"scope\""), std::string::npos) << *inner;
}

TEST(Scope, ThreadsHaveTheirOwnStacks) {
    TestConfig test;
    Session session(test.config);
    ECO_SCOPE("Main");
    std::thread([] {
        ECO_SCOPE("Worker");
        ECO_LOG_INFO("in worker");
    }).join();
    ECO_LOG_INFO("in main");
    const auto lines = session.lines();
    const std::string* worker = findContaining(lines, "\"msg\":\"in worker\"");
    ASSERT_NE(worker, nullptr);
    EXPECT_NE(worker->find("\"scope\":\"Worker\""), std::string::npos) << *worker;
    const std::string* main = findContaining(lines, "\"msg\":\"in main\"");
    ASSERT_NE(main, nullptr);
    EXPECT_NE(main->find("\"scope\":\"Main\""), std::string::npos) << *main;
}
