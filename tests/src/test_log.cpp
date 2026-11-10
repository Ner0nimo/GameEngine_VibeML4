#include <gtest/gtest.h>

#include "ignis/core/log.h"

#include <string>
#include <vector>

using namespace ignis;

TEST(Log, ListenerReceivesFilteredMessages) {
    Logger& log = Logger::instance();
    const LogLevel old = log.level();
    log.set_level(LogLevel::Trace);

    std::vector<std::pair<LogLevel, std::string>> got;
    log.set_listener([&](LogLevel l, const std::string& m) { got.emplace_back(l, m); });

    IGNIS_INFO("hello {}", 42);
    IGNIS_WARN("careful");
    IGNIS_ERROR("bad");

    log.set_listener(nullptr);
    log.set_level(old);

    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].first, LogLevel::Info);
    EXPECT_EQ(got[0].second, "hello 42");
    EXPECT_EQ(got[1].first, LogLevel::Warn);
    EXPECT_EQ(got[1].second, "careful");
    EXPECT_EQ(got[2].first, LogLevel::Error);
    EXPECT_EQ(got[2].second, "bad");
}

TEST(Log, ListenerRespectsLevelFilter) {
    Logger& log = Logger::instance();
    const LogLevel old = log.level();
    log.set_level(LogLevel::Error);

    u32 count = 0;
    log.set_listener([&](LogLevel, const std::string&) { ++count; });

    IGNIS_INFO("dropped");
    IGNIS_ERROR("kept");

    log.set_listener(nullptr);
    log.set_level(old);

    EXPECT_EQ(count, 1u);
}
