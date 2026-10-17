/*
 * util.h — 板端日志、时间与资源采样工具
 * See docs/PLAN.md §8.2（日志格式）与 docs/plan-imx6ull.md §3.5
 */
#ifndef SUPER_SCREEN_UTIL_H
#define SUPER_SCREEN_UTIL_H

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t now_ms_monotonic(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static inline uint64_t now_ms_wall(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

static inline void board_log(const char *level, const char *fmt, ...)
{
    va_list ap;

    printf("board %s ", level);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

#define LOG_INFO(...) board_log("INFO", __VA_ARGS__)
#define LOG_WARN(...) board_log("WARN", __VA_ARGS__)
#define LOG_ERR(...)  board_log("ERR", __VA_ARGS__)

static inline unsigned long board_rss_kb(void)
{
    FILE *f;
    unsigned long size = 0;
    unsigned long resident = 0;
    unsigned long page_kb = (unsigned long)(sysconf(_SC_PAGESIZE) / 1024);

    f = fopen("/proc/self/statm", "r");
    if (f == NULL) {
        return 0;
    }
    if (fscanf(f, "%lu %lu", &size, &resident) != 2) {
        resident = 0;
    }
    fclose(f);
    return resident * page_kb;
}

#endif /* SUPER_SCREEN_UTIL_H */
