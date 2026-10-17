#include "log.h"

#include <cstdarg>
#include <cstdio>

static void LogEvent(const char* level, const char* fmt, va_list args)
{
    char buf[2048];
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) {
        return;
    }
    std::printf("win %s %s\n", level, buf);
    std::fflush(stdout);
}

void LogInit()
{
    // 每行日志后都会 fflush，无需 setvbuf；UCRT 对 (_IOLBF, 0) 会按非法参数快失败。
}

void LogInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogEvent("INFO", fmt, args);
    va_end(args);
}

void LogWarn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogEvent("WARN", fmt, args);
    va_end(args);
}

void LogError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogEvent("ERR", fmt, args);
    va_end(args);
}

void LogStats(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    std::printf("STATS %s\n", buf);
    std::fflush(stdout);
}
