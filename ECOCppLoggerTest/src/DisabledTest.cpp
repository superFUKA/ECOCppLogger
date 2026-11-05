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
    EXPECT_EQ(g_evaluated, 0);  // 引数も評価しない
    EXPECT_EQ(lgtest::countContaining(session.lines(), "disabled"), 0u);
}
