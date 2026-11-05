#include <gtest/gtest.h>

#include "ECOCppLogger/Level.h"

using eco::log::Level;

TEST(Level, NamesAreLowerCase) {
    EXPECT_EQ(eco::log::levelName(Level::Trace), "trace");
    EXPECT_EQ(eco::log::levelName(Level::Info), "info");
    EXPECT_EQ(eco::log::levelName(Level::Fatal), "fatal");
}

TEST(Level, OrderedFromLightToHeavy) {
    EXPECT_LT(Level::Trace, Level::Debug);
    EXPECT_LT(Level::Warn, Level::Error);
    EXPECT_LT(Level::Fatal, Level::Off);
}
