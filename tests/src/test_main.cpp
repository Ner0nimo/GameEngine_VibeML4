#include "ignis/core/log.h"

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ignis::Logger::instance().set_level(ignis::LogLevel::Warn);
    return RUN_ALL_TESTS();
}
