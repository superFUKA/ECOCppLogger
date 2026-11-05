#include <gtest/gtest.h>

#include <cstdint>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

using lgtest::countContaining;
using lgtest::findContaining;
using lgtest::Session;
using lgtest::TestConfig;

namespace {

ECO_LOG_TYPE(Physics, "physics");

enum class Kind : uint8_t { A = 1, B = 2 };

}  // namespace

TEST(Batch, KeepsNativeSizes) {
    using Batch = lg::Batch<int8_t, uint16_t, int, float, double, bool, char, Kind, const void*>;
    EXPECT_EQ(Batch::kElementSize, 1u + 2u + 4u + 4u + 8u + 1u + 1u + 1u + 8u);
}

TEST(Batch, WritesOneRecordWithAllRows) {
    TestConfig test;
    std::string file;
    {
        Session session(test.config);
        lg::setThreadName("main");
        {
            ECO_LOG_BATCH(candidates, Debug | Physics, "candidate {id} distance {d:.2}", int, float);
            for (int i = 0; i < 5; ++i) {
                candidates.add(i, static_cast<float>(i) * 1.5f);
            }
        }  // スコープの終わりで書く
        file = session.file();
        const auto lines = session.lines();
        const std::string* line = findContaining(lines, "\"batch\":5");
        ASSERT_NE(line, nullptr);
        EXPECT_NE(line->find("\"lv\":\"debug\",\"type\":\"physics\""), std::string::npos) << *line;
        EXPECT_NE(line->find("\"msg\":\"candidate {id} distance {d:.2}\""), std::string::npos) << *line;
        EXPECT_NE(line->find("\"rows\":[{\"id\":0,\"d\":0},{\"id\":1,\"d\":1.5},"), std::string::npos) << *line;
        EXPECT_NE(line->find("\"us\":"), std::string::npos) << *line;
    }
    const auto text = lgtest::readLines(file.c_str(), lg::ViewFormat::Text);
    EXPECT_EQ(countContaining(text, "[batch 5 rows, "), 1u);
    EXPECT_EQ(countContaining(text, "    candidate 4 distance 6.00"), 1u);

    lg::ViewFilter limited;
    limited.batchRows = 2;
    const auto shortText = lgtest::readLines(file.c_str(), lg::ViewFormat::Text, limited);
    EXPECT_EQ(countContaining(shortText, "    candidate "), 2u);
    EXPECT_EQ(countContaining(shortText, "... 3 more rows"), 1u);
}

TEST(Batch, FlushesWhenFull) {
    TestConfig test;
    Session session(test.config);
    using Rows = lg::Batch<uint64_t>;
    {
        ECO_LOG_BATCH(values, Info, "v={}", uint64_t);
        for (uint64_t i = 0; i < Rows::kCapacity * 2 + 3; ++i) {
            values.add(i);
        }
        values.flush();
        EXPECT_EQ(values.size(), 0u);
    }
    const auto lines = session.lines();
    EXPECT_EQ(countContaining(lines, "\"batch\":" + std::to_string(Rows::kCapacity)), 2u);
    EXPECT_EQ(countContaining(lines, "\"batch\":3,"), 1u);
    EXPECT_EQ(countContaining(lines, "\"rows\":[\"v=0\""), 1u);  // 名前がなければ本文の並び
}

TEST(Batch, DoesNothingBelowTheLevel) {
    TestConfig test;
    Session session(test.config);
    lg::setLevel(lg::Level::Info);
    {
        ECO_LOG_BATCH(hidden, Debug, "hidden {}", int);
        EXPECT_FALSE(hidden.enabled());
        hidden.add(1);
        EXPECT_EQ(hidden.size(), 0u);
    }
    EXPECT_EQ(countContaining(session.lines(), "hidden"), 0u);
}
