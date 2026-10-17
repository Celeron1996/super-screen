/*
 * 统一日志输出：事件行 "<module> <LEVEL> <message>"，STATS 行独立前缀。
 * 见 docs/PLAN.md §8.2。
 */
#pragma once

void LogInit();
void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);
void LogStats(const char* fmt, ...);
