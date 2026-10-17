#include "monitor.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

#include "log.h"

struct EnumCtx {
    std::vector<DisplayInfo>* out;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam)
{
    auto* ctx = reinterpret_cast<EnumCtx*>(lparam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi) != FALSE) {
        DisplayInfo info;
        info.rect = mi.rcMonitor;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        info.device = mi.szDevice;
        ctx->out->push_back(std::move(info));
    }
    return TRUE;
}

static bool RectLess(const DisplayInfo& a, const DisplayInfo& b)
{
    if (a.rect.top != b.rect.top) {
        return a.rect.top < b.rect.top;
    }
    return a.rect.left < b.rect.left;
}

bool EnumDisplays(std::vector<DisplayInfo>* out)
{
    out->clear();
    EnumCtx ctx{out};
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
    std::sort(out->begin(), out->end(), RectLess);
    return !out->empty();
}

static bool ParseSize(const std::string& match, int* width, int* height)
{
    int w = 0;
    int h = 0;
    if (sscanf_s(match.c_str(), "%dx%d", &w, &h) != 2) {
        return false;
    }
    if (w <= 0 || h <= 0) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

static std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) {
        return std::wstring();
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (need <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), need);
    out.resize(static_cast<size_t>(need - 1));
    return out;
}

bool FindMonitorTarget(const std::string& match, MonitorTarget* target,
                       std::vector<DisplayInfo>* all)
{
    if (!EnumDisplays(all)) {
        return false;
    }

    int index = -1;
    int wantW = 0;
    int wantH = 0;
    std::wstring wantName;
    if (match.rfind("index:", 0) == 0) {
        index = static_cast<int>(std::strtol(match.c_str() + 6, nullptr, 10));
    } else if (match.rfind("name:", 0) == 0) {
        wantName = Utf8ToWide(match.substr(5));
    } else if (!ParseSize(match, &wantW, &wantH)) {
        LogWarn("invalid monitor_match '%s'", match.c_str());
        return false;
    }

    const DisplayInfo* chosen = nullptr;
    if (index >= 0) {
        if (index < static_cast<int>(all->size())) {
            chosen = &(*all)[static_cast<size_t>(index)];
        }
    } else if (!wantName.empty()) {
        for (const auto& info : *all) {
            if (_wcsicmp(info.device.c_str(), wantName.c_str()) == 0) {
                chosen = &info;
                break;
            }
        }
    } else {
        for (const auto& info : *all) {
            if (info.primary) {
                continue;
            }
            const int w = info.rect.right - info.rect.left;
            const int h = info.rect.bottom - info.rect.top;
            if (w == wantW && h == wantH) {
                chosen = &info;
                break;
            }
        }
        if (chosen == nullptr) {
            for (const auto& info : *all) {
                const int w = info.rect.right - info.rect.left;
                const int h = info.rect.bottom - info.rect.top;
                if (w == wantW && h == wantH) {
                    chosen = &info;
                    break;
                }
            }
        }
    }

    if (chosen == nullptr) {
        return false;
    }
    target->rect = chosen->rect;
    target->primary = chosen->primary;
    target->device = chosen->device;
    target->width = chosen->rect.right - chosen->rect.left;
    target->height = chosen->rect.bottom - chosen->rect.top;
    return target->width > 0 && target->height > 0;
}

void LogDisplays(const std::vector<DisplayInfo>& infos)
{
    LogInfo("monitors: %d found", static_cast<int>(infos.size()));
    for (size_t i = 0; i < infos.size(); ++i) {
        const DisplayInfo& info = infos[i];
        LogInfo("display[%d] %ls %dx%d at (%d,%d)%s", static_cast<int>(i), info.device.c_str(),
                info.rect.right - info.rect.left, info.rect.bottom - info.rect.top,
                info.rect.left, info.rect.top, info.primary ? " primary" : "");
    }
}
