#include <gtest/gtest.h>

#include <string_view>

#include "ECOCppLogger/Level.h"

using lg::Level;

TEST(Level, NamesAreLowerCase) {
    EXPECT_EQ(std::string_view(lg::levelName(Level::Trace)), "trace");
    EXPECT_EQ(std::string_view(lg::levelName(Level::Warn)), "warn");
    EXPECT_EQ(std::string_view(lg::levelName(Level::Fatal)), "fatal");
}

TEST(Level, OrderedFromLightToHeavy) {
    EXPECT_LT(Level::Trace, Level::Debug);
    EXPECT_LT(Level::Warn, Level::Error);
    EXPECT_LT(Level::Fatal, Level::Off);
}

TEST(Level, ShortNamesForMacros) {
    using namespace lg::levels;
    EXPECT_EQ(Warn, Level::Warn);
    EXPECT_EQ(Info, Level::Info);
}
