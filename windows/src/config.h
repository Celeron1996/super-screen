/*
 * config.ini 读取与默认值。
 * 配置项定义见 docs/plan-windows.md §5；File 缺失时使用内置默认值。
 */
#pragma once

#include <string>

struct Config {
    std::string serverIp = "192.168.1.14";
    int serverPort = 5901;
    int fpsCap = 20;
    int fullRefreshSec = 5;
    int format = 0;
    bool manageVdd = true;
    std::string virtualResolution = "1024x600";
    std::string captureMode = "gdi";
    std::string monitorMatch = "1024x600";
    int tileSize = 64;
    bool lz4 = true;
    bool compositeCursor = true;
};

bool LoadConfig(const wchar_t* iniPath, Config* cfg);
void SaveVirtualResolution(const wchar_t* iniPath, int width, int height);
