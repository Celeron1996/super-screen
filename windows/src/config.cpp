#include "config.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "log.h"

static std::string ReadIniString(const wchar_t* path, const wchar_t* section, const wchar_t* key,
                                 const wchar_t* def)
{
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(section, key, def, buf,
                             static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), path);
    const int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return std::string();
    }
    std::vector<char> tmp(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, tmp.data(), need, nullptr, nullptr);
    return std::string(tmp.data());
}

static int ReadIniInt(const wchar_t* path, const wchar_t* section, const wchar_t* key, int def)
{
    return static_cast<int>(GetPrivateProfileIntW(section, key, static_cast<UINT>(def), path));
}

bool LoadConfig(const wchar_t* iniPath, Config* cfg)
{
    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES) {
        LogInfo("config.ini not found, using built-in defaults");
    } else {
        cfg->serverIp = ReadIniString(iniPath, L"server", L"ip", L"192.168.1.14");
        cfg->serverPort = ReadIniInt(iniPath, L"server", L"port", 5901);
        cfg->fpsCap = ReadIniInt(iniPath, L"display", L"fps_cap", 20);
        cfg->fullRefreshSec = ReadIniInt(iniPath, L"display", L"full_refresh_sec", 5);
        const std::string format = ReadIniString(iniPath, L"display", L"format", L"bgra32");
        cfg->format = (_stricmp(format.c_str(), "rgb565") == 0) ? 1 : 0;
        cfg->captureMode = ReadIniString(iniPath, L"capture", L"mode", L"gdi");
        cfg->monitorMatch = ReadIniString(iniPath, L"capture", L"monitor_match", L"1024x600");
        cfg->tileSize = ReadIniInt(iniPath, L"tile", L"size", 64);
        cfg->lz4 = ReadIniInt(iniPath, L"tile", L"lz4", 1) != 0;
        cfg->compositeCursor = ReadIniInt(iniPath, L"cursor", L"composite", 1) != 0;
    }

    if (cfg->serverPort < 1 || cfg->serverPort > 65535) {
        LogWarn("config port %d invalid, using 5901", cfg->serverPort);
        cfg->serverPort = 5901;
    }
    if (cfg->fpsCap < 1) {
        cfg->fpsCap = 1;
    }
    if (cfg->fpsCap > 240) {
        cfg->fpsCap = 240;
    }
    if (cfg->fullRefreshSec < 1) {
        cfg->fullRefreshSec = 1;
    }
    if (cfg->fullRefreshSec > 60) {
        cfg->fullRefreshSec = 60;
    }
    if (cfg->format != 0 && cfg->format != 1) {
        cfg->format = 0;
    }
    if (cfg->tileSize != 64) {
        LogWarn("tile size %d unsupported, forcing 64", cfg->tileSize);
        cfg->tileSize = 64;
    }
    if (_stricmp(cfg->captureMode.c_str(), "gdi") != 0 &&
        _stricmp(cfg->captureMode.c_str(), "dxgi") != 0) {
        LogWarn("unknown capture mode '%s', using gdi", cfg->captureMode.c_str());
        cfg->captureMode = "gdi";
    }

    LogInfo("config server=%s:%d fps_cap=%d full_refresh=%ds format=%s monitor_match=%s "
            "tile=%d lz4=%d cursor=%d",
            cfg->serverIp.c_str(), cfg->serverPort, cfg->fpsCap, cfg->fullRefreshSec,
            cfg->format == 1 ? "rgb565" : "bgra32", cfg->monitorMatch.c_str(), cfg->tileSize,
            cfg->lz4 ? 1 : 0, cfg->compositeCursor ? 1 : 0);
    return true;
}
