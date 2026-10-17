/*
 * 显示器枚举与目标屏定位（按 config.ini 的 monitor_match）。
 * 定位规则见 docs/plan-windows.md §4.2。
 */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct DisplayInfo {
    RECT rect{};
    bool primary = false;
    std::wstring device;
};

struct MonitorTarget {
    RECT rect{};
    bool primary = false;
    std::wstring device;
    int width = 0;
    int height = 0;
};

bool EnumDisplays(std::vector<DisplayInfo>* out);
bool FindMonitorTarget(const std::string& match, MonitorTarget* target,
                       std::vector<DisplayInfo>* all);
void LogDisplays(const std::vector<DisplayInfo>& infos);
