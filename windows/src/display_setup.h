/*
 * 虚拟副屏按需创建：启动时启用 VDD 设备并应用用户选择的分辨率，退出时禁用。
 * 设备保持安装但默认禁用，因此未运行本应用时不会在开机后出现虚拟副屏。
 */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ResolutionMode {
    int width = 0;
    int height = 0;
    int refresh = 60;
};

enum class ResolutionDialogResult {
    Cancelled,
    Start,
    RemoveDisplay,
};

bool IsProcessElevated();
bool IsVddInstalled();
bool IsVddEnabled();
bool RemoveVirtualDisplay(std::wstring* error);

std::vector<ResolutionMode> LoadResolutionModes();
ResolutionDialogResult ShowResolutionDialog(HWND owner,
                                            const std::vector<ResolutionMode>& modes,
                                            int defaultIndex, bool displayRegistered,
                                            int* selectedIndex);

bool ParseResolution(const std::string& text, ResolutionMode* mode);
int FindResolutionIndex(const std::vector<ResolutionMode>& modes, int width, int height);

class VirtualDisplay {
public:
    VirtualDisplay() = default;
    ~VirtualDisplay();
    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    bool Start(const ResolutionMode& mode, std::wstring* deviceName, std::wstring* error);
    void Stop();
    bool Active() const { return m_active; }

private:
    bool m_active = false;
};
