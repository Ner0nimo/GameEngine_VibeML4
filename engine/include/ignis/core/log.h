#pragma once

#include "ignis/core/types.h"

#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace ignis {

enum class LogLevel : u8 { Trace = 0, Debug, Info, Warn, Error, Fatal };

// Minimal thread-safe logger. Writes to stderr and optionally to a file.
// Macros below keep call sites terse: IGNIS_INFO("x={}", x);
class Logger {
public:
    static Logger& instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel level) noexcept { level_ = level; }
    LogLevel level() const noexcept { return level_; }

    // Open an additional file sink. Empty path disables it.
    void set_file(const std::string& path);

    void log(LogLevel level, const std::string& msg);

    template <typename... Args>
        requires(sizeof...(Args) > 0)
    void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        log(level, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    Logger() = default;

    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
    std::ofstream file_;
};

inline const char* to_string(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?";
}

} // namespace ignis

#define IGNIS_TRACE(...) ::ignis::Logger::instance().log(::ignis::LogLevel::Trace, __VA_ARGS__)
#define IGNIS_DEBUG(...) ::ignis::Logger::instance().log(::ignis::LogLevel::Debug, __VA_ARGS__)
#define IGNIS_INFO(...)  ::ignis::Logger::instance().log(::ignis::LogLevel::Info,  __VA_ARGS__)
#define IGNIS_WARN(...)  ::ignis::Logger::instance().log(::ignis::LogLevel::Warn,  __VA_ARGS__)
#define IGNIS_ERROR(...) ::ignis::Logger::instance().log(::ignis::LogLevel::Error, __VA_ARGS__)
#define IGNIS_FATAL(...) ::ignis::Logger::instance().log(::ignis::LogLevel::Fatal, __VA_ARGS__)
