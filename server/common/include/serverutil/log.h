#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>

namespace serverutil {

// Line-buffered stdout/stderr logging with a timestamp and service tag. Docker
// captures stdout, so this is all the log plumbing a dedicated server needs.

inline void logLine(FILE* out, const char* level, const char* tag, const char* format,
        va_list args) {
    char timestamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(timestamp, sizeof timestamp, "%H:%M:%S", &tm);

    std::fprintf(out, "%s [%s] %-5s ", timestamp, tag, level);
    std::vfprintf(out, format, args);
    std::fputc('\n', out);
    std::fflush(out);
}

class Log {
public:
    explicit Log(std::string tag) : mTag(std::move(tag)) {}

    void info(const char* format, ...) const {
        va_list args;
        va_start(args, format);
        logLine(stdout, "INFO", mTag.c_str(), format, args);
        va_end(args);
    }

    void warn(const char* format, ...) const {
        va_list args;
        va_start(args, format);
        logLine(stderr, "WARN", mTag.c_str(), format, args);
        va_end(args);
    }

    void error(const char* format, ...) const {
        va_list args;
        va_start(args, format);
        logLine(stderr, "ERROR", mTag.c_str(), format, args);
        va_end(args);
    }

private:
    std::string mTag;
};

}  // namespace serverutil
