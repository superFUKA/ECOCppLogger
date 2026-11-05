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
    EXPECT_EQ(g_evaluated, 0);  // 引数も評価しない
    EXPECT_EQ(lgtest::countContaining(session.lines(), "disabled"), 0u);
}
