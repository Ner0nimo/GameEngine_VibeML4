#include "ignis/core/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ignis {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_file(const std::string& path) {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.close();
    }
    if (!path.empty()) {
        file_.open(path, std::ios::app);
    }
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (static_cast<u8>(level) < static_cast<u8>(level_)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream time_oss;
    time_oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    const std::string time_str = time_oss.str();

    std::string line = std::format("[{}] [{}] {}", time_str, to_string(level), msg);

    Listener listener;
    {
        std::lock_guard lock(mutex_);
        std::fputs(line.c_str(), stderr);
        std::fputc('\n', stderr);
        if (file_.is_open()) {
            file_ << line << '\n';
            file_.flush();
        }
        listener = listener_;
    }
    // Invoke outside the lock so a listener may safely call back into log().
    if (listener) listener(level, msg);
}

} // namespace ignis
