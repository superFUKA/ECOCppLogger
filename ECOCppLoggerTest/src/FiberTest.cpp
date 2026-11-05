// ファイバーが中断の前後で別のスレッドへ移っても、ログはそのとき走っているスレッドのバッファに入ることを確かめる。
// （Windows のファイバーで、スレッドA で始めたファイバーを、スレッドB で続きから走らせる）

#include <gtest/gtest.h>

#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

void* g_returnTo = nullptr;  // ファイバーが中断したときに戻る先（今のスレッドのファイバー）
lg::ThreadBuffer* g_bufferBefore = nullptr;
lg::ThreadBuffer* g_bufferAfter = nullptr;

void logTwiceAcrossThreads() {
    ECO_LOG_INFO("before move");
    g_bufferBefore = lg::detail::threadBuffer();
    SwitchToFiber(g_returnTo);  // 中断（次はスレッドB で続きから走る）
    ECO_LOG_INFO("after move");
    g_bufferAfter = lg::detail::threadBuffer();
}

void CALLBACK fiberMain(void*) {
    logTwiceAcrossThreads();
    SwitchToFiber(g_returnTo);
}

}  // namespace

TEST(Fiber, LogsGoToTheThreadTheFiberRunsOn) {
    lgtest::TestConfig test;
    lgtest::Session session(test.config);
    void* fiber = nullptr;
    std::thread([&] {
        lg::setThreadName("threadA");
        g_returnTo = ConvertThreadToFiber(nullptr);
        fiber = CreateFiber(0, fiberMain, nullptr);
        SwitchToFiber(fiber);  // ファイバーを始める → "before move" の後で戻る
        ConvertFiberToThread();
    }).join();
    std::thread([&] {
        lg::setThreadName("threadB");
        g_returnTo = ConvertThreadToFiber(nullptr);
        SwitchToFiber(fiber);  // 同じファイバーを、別のスレッドで続きから
        ConvertFiberToThread();
    }).join();
    DeleteFiber(fiber);
    EXPECT_NE(g_bufferBefore, g_bufferAfter);
    const auto lines = session.lines();
    EXPECT_EQ(lgtest::countContaining(lines, "\"th\":\"threadA\",\"src\":\"FiberTest.cpp:"), 1u);
    const std::string* after = lgtest::findContaining(lines, "after move");
    ASSERT_NE(after, nullptr);
    EXPECT_NE(after->find("\"th\":\"threadB\""), std::string::npos) << *after;
}

namespace {

constexpr uint64_t kJobFiber = 7;

void CALLBACK jobFiberMain(void*) {
    {
        ECO_SCOPE("Job {id}", 42);
        ECO_LOG_INFO("job started");
        SwitchToFiber(g_returnTo);  // 中断（次はスレッドB で続きから走る）
        ECO_LOG_INFO("job resumed");
    }
    SwitchToFiber(g_returnTo);
}

}  // namespace

TEST(Fiber, ScopesFollowTheFiberAcrossThreads) {
    lgtest::TestConfig test;
    lgtest::Session session(test.config);
    void* fiber = nullptr;
    std::thread([&] {
        lg::setThreadName("threadA");
        g_returnTo = ConvertThreadToFiber(nullptr);
        fiber = CreateFiber(0, jobFiberMain, nullptr);
        lg::onFiberSwitch(kJobFiber);  // ファイバーの仕組みが、切り替えるたびに呼ぶ
        SwitchToFiber(fiber);
        lg::onFiberSwitch(0);
        ECO_LOG_INFO("threadA idle");
        ConvertFiberToThread();
    }).join();
    std::thread([&] {
        lg::setThreadName("threadB");
        g_returnTo = ConvertThreadToFiber(nullptr);
        lg::onFiberSwitch(kJobFiber);
        SwitchToFiber(fiber);
        lg::onFiberSwitch(0);
        ConvertFiberToThread();
    }).join();
    DeleteFiber(fiber);
    const auto lines = session.lines();
    const std::string* resumed = lgtest::findContaining(lines, "\"msg\":\"job resumed\"");
    ASSERT_NE(resumed, nullptr);
    EXPECT_NE(resumed->find("\"th\":\"threadB\",\"fiber\":7,\"scope\":\"Job 42\""), std::string::npos) << *resumed;
    const std::string* idle = lgtest::findContaining(lines, "\"msg\":\"threadA idle\"");
    ASSERT_NE(idle, nullptr);
    EXPECT_EQ(idle->find("\"scope\""), std::string::npos) << *idle;  // ファイバーが離れたスレッドには残らない
    const std::string* exit = lgtest::findContaining(lines, "\"scope_event\":\"exit\"");
    ASSERT_NE(exit, nullptr);
    EXPECT_NE(exit->find("\"th\":\"threadB\""), std::string::npos) << *exit;
    EXPECT_NE(exit->find("\"msg\":\"Job 42\""), std::string::npos) << *exit;
}

#endif
