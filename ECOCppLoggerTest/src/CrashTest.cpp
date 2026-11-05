// 落ちたときの書き出しを、別のプロセス（gtest の death test）で確かめる。
// gtest はテストの本体で起きた SEH の例外を捕まえるので、落とすのは別のスレッドで。

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;

namespace {

struct CrashConfig : lgtest::TestConfig {
    CrashConfig() { config.crash.handler = true; }
};

std::atomic<bool> g_forever{false};

void startSleeper() {
    std::thread([] {
        lg::setThreadName("sleeper");
        ECO_LOG_INFO("sleeper started");
        while (!g_forever.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void crashByWrite() {
    volatile int* pointer = nullptr;
    *pointer = 1;
}

std::vector<std::string> latestLines(const std::string& directory) {
    const char* latest = lg::latestLog(directory.c_str());
    EXPECT_NE(latest, nullptr);
    return latest ? lgtest::readLines(latest) : std::vector<std::string>{};
}

}  // namespace

TEST(CrashDeathTest, AccessViolationWritesEverything) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    CrashConfig test;
    EXPECT_DEATH(
        {
            lg::start(test.config);
            lg::setThreadName("main");
            startSleeper();
            ECO_LOG_INFO("before crash");
            ECO_STAT_HISTORY("crash.speed", 4, 12.5);
            std::thread([] {
                lg::setThreadName("crasher");
                ECO_LOG_WARN("about to crash");
                crashByWrite();
            }).join();
        },
        "");
    const auto lines = latestLines(test.directory);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"before crash\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"about to crash\""), 1u);
    const std::string* crash = findContaining(lines, "\"event\":\"crash\"");
    ASSERT_NE(crash, nullptr);
    EXPECT_NE(crash->find("\"lv\":\"fatal\""), std::string::npos);
    EXPECT_NE(crash->find("\"name\":\"access violation\",\"access\":\"write\",\"address\":\"0x0\""), std::string::npos)
        << *crash;
    EXPECT_NE(crash->find("\"th\":\"crasher\""), std::string::npos);
    EXPECT_NE(crash->find("crashByWrite"), std::string::npos) << "落ちた所の関数がスタックにある";
    EXPECT_NE(crash->find("\"registers\":{\"rip\":"), std::string::npos);
    EXPECT_NE(crash->find("\"th\":\"sleeper\",\"stack\":["), std::string::npos) << "ほかのスレッドのスタック";
    EXPECT_NE(crash->find("\"name\":\"ntdll.dll\""), std::string::npos) << "DLL の一覧";
    const size_t dump = crash->find("\"minidump\":\"");
    ASSERT_NE(dump, std::string::npos);
    const size_t begin = dump + 12;
    std::string path = crash->substr(begin, crash->find('"', begin) - begin);
    for (size_t at = 0; (at = path.find("\\\\", at)) != std::string::npos; ++at) path.erase(at, 1);
    EXPECT_TRUE(std::filesystem::exists(lgtest::pathOf(path))) << path;
    EXPECT_EQ(countContaining(lines, "\"lv\":\"history\",\"name\":\"crash.speed\""), 1u);
    EXPECT_EQ(countContaining(lines, "\"event\":\"session.end\",\"crashed\":true"), 1u);
}

TEST(CrashDeathTest, UncaughtNestedExceptionsAreListed) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    CrashConfig test;
    test.config.crash.minidump = false;
    test.config.crash.modules = false;
    EXPECT_DEATH(
        {
            lg::start(test.config);
            std::thread([] {
                ECO_LOG_INFO("loading");
                try {
                    throw std::runtime_error("file not found");
                } catch (...) {
                    std::throw_with_nested(std::logic_error("level load failed"));
                }
            }).join();
        },
        "");
    const auto lines = latestLines(test.directory);
    EXPECT_EQ(countContaining(lines, "\"msg\":\"loading\""), 1u);
    const std::string* crash = findContaining(lines, "\"event\":\"crash\"");
    ASSERT_NE(crash, nullptr);
    EXPECT_NE(crash->find("\"what\":\"level load failed\""), std::string::npos) << *crash;
    EXPECT_NE(crash->find("\"what\":\"file not found\""), std::string::npos) << *crash;
    EXPECT_NE(crash->find("\"type\":\"std::runtime_error\""), std::string::npos) << *crash;
}
