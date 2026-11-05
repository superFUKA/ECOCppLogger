// ECO_LOG_ENABLE=0：すべてのマクロが消える（引数も評価しない）ことを確かめる。このファイルだけ 0 にする。
#define ECO_LOG_ENABLE 0

#include <gtest/gtest.h>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

namespace {

int g_evaluated = 0;

int sideEffect() {
    return ++g_evaluated;
}

ECO_LOG_TYPE(Physics, "physics");  // 型の定数は消えずに使える

}  // namespace

TEST(Disabled, MacrosLeaveNoCode) {
    lgtest::TestConfig test;
    lgtest::Session session(test.config);
    ECO_LOG_INFO("disabled {}", sideEffect());
    ECO_LOG(Warn | Physics, "disabled typed {}", sideEffect());
    ECO_FRAME();
    ECO_LOG_IF(Info, sideEffect() > 0, "if {}", sideEffect());
    ECO_LOG_EVERY_N(Info, 2, "every {}", sideEffect());
    ECO_LOG_FIRST_N(Info, 2, "first {}", sideEffect());
    ECO_LOG_ONCE(Info, "once {}", sideEffect());
    ECO_LOG_EVERY_MS(Info, 10, "ms {}", sideEffect());
    ECO_LOG_ON_CHANGE(Info, "value", sideEffect());
    ECO_LOG_BATCH(batch, Info, "disabled batch {}", int);
    batch.add(1);
    EXPECT_FALSE(batch.enabled());
    ECO_STAT("disabled.stat", sideEffect());
    ECO_STAT_COUNT("disabled.count");
    ECO_STAT_TIME("disabled.time");
    ECO_STAT_HISTORY("disabled.history", 4, sideEffect());
    ECO_SCOPE("disabled scope {}", sideEffect());
    ECO_ASSERT(sideEffect() < 0, "x {}", sideEffect());
    EXPECT_FALSE(ECO_CHECK(false));
    ECO_VERIFY(sideEffect() > 0);
    EXPECT_EQ(g_evaluated, 1);  // ECO_VERIFY の式だけ評価する
    g_evaluated = 0;
    EXPECT_EQ(g_evaluated, 0);  // 引数も評価しない
    EXPECT_EQ(lgtest::countContaining(session.lines(), "\"msg\":\"disabled"), 0u);
}
