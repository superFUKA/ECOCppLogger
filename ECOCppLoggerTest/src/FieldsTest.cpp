#include <gtest/gtest.h>

#include <string>

#include "ECOCppLogger/ECOCppLogger.h"
#include "TestSession.h"

namespace game {

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Player {
    int id;
    std::string name;
    Vec3 position;
};

}  // namespace game

ECO_LOG_FIELDS(game::Vec3, x, y, z);
ECO_LOG_FIELDS(game::Player, id, name, position);

TEST(Fields, WritesRegisteredTypes) {
    lgtest::TestConfig test;
    std::string file;
    {
        lgtest::Session session(test.config);
        const game::Player player{7, "goblin", {1.0f, 2.5f, -3.0f}};
        ECO_LOG_INFO("spawn {player} at frame {f}", player, 12);
        file = session.file();
        const auto lines = session.lines();
        const std::string* line = lgtest::findContaining(lines, "\"msg\":\"spawn ");
        ASSERT_NE(line, nullptr);
        EXPECT_NE(line->find("\"msg\":\"spawn {id=7, name=goblin, position={x=1, y=2.5, z=-3}} at frame 12\""),
                  std::string::npos)
            << *line;
        EXPECT_NE(line->find("\"args\":{\"player\":{\"id\":7,\"name\":\"goblin\",\"position\":{\"x\":1,\"y\":2.5,\"z\":-3}},"
                             "\"f\":12}"),
                  std::string::npos)
            << *line;
    }
    const auto text = lgtest::readLines(file.c_str(), lg::ViewFormat::Text);
    EXPECT_EQ(lgtest::countContaining(text, "spawn {id=7, name=goblin, position={x=1, y=2.5, z=-3}}"), 1u);
}
