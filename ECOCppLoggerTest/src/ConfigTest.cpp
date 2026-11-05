#include <gtest/gtest.h>

#include "ECOCppLogger/Config.h"

TEST(Config, DefaultsWriteEverythingToLogs) {
    const lg::Config config;
    EXPECT_STREQ(config.directory, "logs");
    EXPECT_EQ(config.destinationCount, 0u);
    EXPECT_EQ(config.level, lg::Level::Trace);
    EXPECT_TRUE(config.recordTime);
    EXPECT_EQ(config.consoleLevel, lg::Level::Off);
    EXPECT_TRUE(config.crash.handler);
    EXPECT_EQ(config.assertAction, lg::AssertAction::Break);
}

TEST(Config, DestinationsUsePlainTypes) {
    const lg::Destination destinations[] = {
        {.name = "human", .level = lg::Level::Info, .excludeTypes = "batch,stat"},
        {.name = "all"},
    };
    lg::Config config;
    config.destinations = destinations;
    config.destinationCount = 2;
    EXPECT_STREQ(config.destinations[0].excludeTypes, "batch,stat");
    EXPECT_EQ(config.destinations[1].types, nullptr);
}
