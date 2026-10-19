#include "display_setup.h"

#include <cfgmgr32.h>
#include <devguid.h>
#include <setupapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include "log.h"
#include "monitor.h"
#include "resource.h"

namespace {

const wchar_t* kVddSettingsPath = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
const wchar_t* kVddHardwareId = L"Root\\MttVDD";

const ResolutionMode kFallbackModes[] = {
    {1024, 600, 60},  {800, 480, 60},   {1280, 720, 60},  {1366, 768, 60},
    {1600, 900, 60},  {1920, 1080, 60}, {1920, 1200, 60}, {2560, 1440, 60},
    {3840, 2160, 60},
};

struct ResolutionDialogContext {
    const std::vector<ResolutionMode>* modes = nullptr;
    int defaultIndex = 0;
    int selectedIndex = -1;
};

bool HasVddHardwareId(HDEVINFO set, SP_DEVINFO_DATA* info)
{
    DWORD type = 0;
    DWORD needed = 0;
    BYTE buffer[512] = {};
    if (SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_HARDWAREID, &type, buffer,
                                         static_cast<DWORD>(sizeof(buffer)), &needed) == FALSE) {
        return false;
    }
    if (type != REG_MULTI_SZ || needed < sizeof(wchar_t)) {
        return false;
    }
    const wchar_t* values = reinterpret_cast<const wchar_t*>(buffer);
    const size_t count = needed / sizeof(wchar_t);
    size_t offset = 0;
    while (offset < count && values[offset] != L'\0') {
        if (_wcsicmp(values + offset, kVddHardwareId) == 0) {
            return true;
        }
        offset += std::wcslen(values + offset) + 1;
    }
    return false;
}

bool FindVddDevice(HDEVINFO set, SP_DEVINFO_DATA* info)
{
    for (DWORD index = 0;; ++index) {
        info->cbSize = sizeof(*info);
        if (SetupDiEnumDeviceInfo(set, index, info) == FALSE) {
            return false;
        }
        if (HasVddHardwareId(set, info)) {
            return true;
        }
    }
}

bool ApplyDeviceState(HDEVINFO set, SP_DEVINFO_DATA* info, DWORD state, DWORD scope)
{
    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = state;
    params.Scope = scope;
    params.HwProfile = 0;

    if (SetupDiSetClassInstallParamsW(set, info, &params.ClassInstallHeader, sizeof(params)) ==
        FALSE) {
        LogError("SetupDiSetClassInstallParams failed: %lu", GetLastError());
        return false;
    }
    if (SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, info) == FALSE) {
        const DWORD err = GetLastError();
        if (err != ERROR_SUCCESS) {
            LogError("DIF_PROPERTYCHANGE failed: %lu", err);
            return false;
        }
    }
    return true;
}

bool SetVddEnabled(bool enable, std::wstring* error)
{
    HDEVINFO set =
        SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        *error = L"cannot enumerate display devices";
        return false;
    }

    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    if (!FindVddDevice(set, &info)) {
        SetupDiDestroyDeviceInfoList(set);
        *error = L"VDD device not found (Root\\MttVDD)";
        return false;
    }

    ULONG status = 0;
    ULONG problem = 0;
    const bool haveStatus =
        CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS;
    const bool started = haveStatus && (status & DN_STARTED) != 0 && problem == 0;

    if (enable && started) {
        SetupDiDestroyDeviceInfoList(set);
        LogInfo("VDD device already enabled");
        return true;
    }
    if (!enable && haveStatus && problem == CM_PROB_DISABLED) {
        SetupDiDestroyDeviceInfoList(set);
        LogInfo("VDD device already disabled");
        return true;
    }

    const DWORD state = enable ? DICS_ENABLE : DICS_DISABLE;
    bool ok = ApplyDeviceState(set, &info, state, DICS_FLAG_CONFIGSPECIFIC);
    if (ok) {
        ok = ApplyDeviceState(set, &info, state, DICS_FLAG_GLOBAL);
    }
    if (!ok) {
        SetupDiDestroyDeviceInfoList(set);
        *error = L"failed to change VDD device state";
        return false;
    }

    bool reached = false;
    for (int i = 0; i < 50; ++i) {
        status = 0;
        problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS) {
            if (enable) {
                reached = (status & DN_STARTED) != 0 && problem == 0;
            } else {
                reached = problem == CM_PROB_DISABLED || (status & DN_STARTED) == 0;
            }
            if (reached) {
                break;
            }
        }
        Sleep(100);
    }
    SetupDiDestroyDeviceInfoList(set);

    if (!reached) {
        *error = enable ? L"VDD enable timed out; check the driver in Device Manager"
                        : L"VDD disable timed out; a reboot may be required";
        LogWarn("VDD %s did not settle: status=0x%lx problem=%lu", enable ? "enable" : "disable",
                status, problem);
        return false;
    }

    LogInfo("VDD device %s", enable ? "enabled" : "disabled");
    return true;
}

bool FindVddDisplay(std::wstring* deviceName)
{
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (EnumDisplayDevicesW(nullptr, index, &display, 0) == FALSE) {
            break;
        }
        const bool isVdd = _wcsicmp(display.DeviceID, kVddHardwareId) == 0 ||
                           _wcsicmp(display.DeviceString, L"Virtual Display Driver") == 0;
        if (!isVdd || (display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) {
            continue;
        }
        *deviceName = display.DeviceName;
        return true;
    }
    return false;
}

bool WaitForVddDisplay(std::wstring* deviceName, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 100) {
        if (FindVddDisplay(deviceName)) {
            return true;
        }
        Sleep(100);
    }
    return false;
}

bool IsDisplayOnDesktop(const std::wstring& deviceName)
{
    std::vector<DisplayInfo> displays;
    if (!EnumDisplays(&displays)) {
        return false;
    }
    return std::any_of(displays.begin(), displays.end(), [&](const DisplayInfo& info) {
        return _wcsicmp(info.device.c_str(), deviceName.c_str()) == 0;
    });
}

bool WaitForDisplayOnDesktop(const std::wstring& deviceName, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 100) {
        if (IsDisplayOnDesktop(deviceName)) {
            return true;
        }
        Sleep(100);
    }
    return false;
}

bool RunDisplaySwitchExtend()
{
    wchar_t systemDir[MAX_PATH] = {};
    if (GetSystemDirectoryW(systemDir, MAX_PATH) == 0) {
        LogWarn("GetSystemDirectory failed: %lu", GetLastError());
        return false;
    }

    const std::wstring exe = std::wstring(systemDir) + L"\\DisplaySwitch.exe";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpFile = exe.c_str();
    info.lpParameters = L"/extend";
    info.nShow = SW_HIDE;
    if (ShellExecuteExW(&info) == FALSE) {
        LogWarn("DisplaySwitch /extend failed: %lu", GetLastError());
        return false;
    }
    if (info.hProcess != nullptr) {
        WaitForSingleObject(info.hProcess, 5000);
        CloseHandle(info.hProcess);
    }
    return true;
}

LONG ApplyDisplayMode(const std::wstring& deviceName, const ResolutionMode& mode)
{
    DEVMODEW devmode{};
    devmode.dmSize = sizeof(devmode);
    devmode.dmPelsWidth = static_cast<DWORD>(mode.width);
    devmode.dmPelsHeight = static_cast<DWORD>(mode.height);
    devmode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    if (mode.refresh > 0) {
        devmode.dmDisplayFrequency = static_cast<DWORD>(mode.refresh);
        devmode.dmFields |= DM_DISPLAYFREQUENCY;
    }

    LONG rc = ChangeDisplaySettingsExW(deviceName.c_str(), &devmode, nullptr,
                                       CDS_UPDATEREGISTRY, nullptr);
    if (rc == DISP_CHANGE_BADMODE && mode.refresh > 0) {
        devmode.dmFields &= ~static_cast<DWORD>(DM_DISPLAYFREQUENCY);
        rc = ChangeDisplaySettingsExW(deviceName.c_str(), &devmode, nullptr,
                                      CDS_UPDATEREGISTRY, nullptr);
    }
    return rc;
}

bool CurrentModeMatches(const std::wstring& deviceName, const ResolutionMode& mode)
{
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &current) == FALSE) {
        return false;
    }
    return static_cast<int>(current.dmPelsWidth) == mode.width &&
           static_cast<int>(current.dmPelsHeight) == mode.height;
}

std::wstring DisplayChangeError(LONG code)
{
    switch (code) {
        case DISP_CHANGE_BADMODE:
            return L" (mode not supported by VDD)";
        case DISP_CHANGE_BADFLAGS:
            return L" (bad flags)";
        case DISP_CHANGE_BADPARAM:
            return L" (bad parameter)";
        case DISP_CHANGE_FAILED:
            return L" (display driver rejected the mode)";
        case DISP_CHANGE_NOTUPDATED:
            return L" (registry update failed)";
        case DISP_CHANGE_RESTART:
            return L" (restart required)";
        default:
            return L" (unknown error)";
    }
}

int ParseIntTag(const std::string& line, const char* tag)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t begin = line.find(open);
    if (begin == std::string::npos) {
        return -1;
    }
    const size_t valueBegin = begin + open.size();
    const size_t end = line.find(close, valueBegin);
    if (end == std::string::npos) {
        return -1;
    }
    return std::atoi(line.substr(valueBegin, end - valueBegin).c_str());
}

std::vector<ResolutionMode> ParseSettingsFile()
{
    std::vector<ResolutionMode> modes;
    std::ifstream file(kVddSettingsPath, std::ios::binary);
    if (!file) {
        return modes;
    }

    bool inResolution = false;
    int width = 0;
    int height = 0;
    int refresh = 60;
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("<resolution>") != std::string::npos) {
            inResolution = true;
            width = 0;
            height = 0;
            refresh = 60;
        }
        if (!inResolution) {
            continue;
        }
        const int parsedWidth = ParseIntTag(line, "width");
        const int parsedHeight = ParseIntTag(line, "height");
        const int parsedRefresh = ParseIntTag(line, "refresh_rate");
        if (parsedWidth > 0) {
            width = parsedWidth;
        }
        if (parsedHeight > 0) {
            height = parsedHeight;
        }
        if (parsedRefresh > 0) {
            refresh = parsedRefresh;
        }
        if (line.find("</resolution>") != std::string::npos) {
            if (width > 0 && height > 0) {
                modes.push_back({width, height, refresh});
            }
            inResolution = false;
        }
    }
    return modes;
}

std::wstring ModeLabel(const ResolutionMode& mode)
{
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%d x %d  (%d Hz)", mode.width, mode.height, mode.refresh);
    return buffer;
}

INT_PTR CALLBACK ResolutionDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<ResolutionDialogContext*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));

    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<ResolutionDialogContext*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        HWND combo = GetDlgItem(dialog, IDC_RESOLUTION);
        for (const ResolutionMode& mode : *context->modes) {
            const std::wstring label = ModeLabel(mode);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        int index = context->defaultIndex;
        if (index < 0 || index >= static_cast<int>(context->modes->size())) {
            index = 0;
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        return TRUE;
    }

    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
            case IDOK: {
                const LRESULT selected =
                    SendDlgItemMessageW(dialog, IDC_RESOLUTION, CB_GETCURSEL, 0, 0);
                if (selected == CB_ERR) {
                    return TRUE;
                }
                if (context != nullptr) {
                    context->selectedIndex = static_cast<int>(selected);
                }
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            case IDCANCEL:
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            default:
                break;
        }
    }

    if (message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

bool IsProcessElevated()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok =
        GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

bool IsVddInstalled()
{
    HDEVINFO set =
        SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        return false;
    }
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    const bool found = FindVddDevice(set, &info);
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

std::vector<ResolutionMode> LoadResolutionModes()
{
    std::vector<ResolutionMode> modes = ParseSettingsFile();
    if (modes.empty()) {
        const size_t count = sizeof(kFallbackModes) / sizeof(kFallbackModes[0]);
        modes.assign(kFallbackModes, kFallbackModes + count);
    }

    std::vector<ResolutionMode> unique;
    for (const ResolutionMode& mode : modes) {
        const bool exists =
            std::any_of(unique.begin(), unique.end(), [&](const ResolutionMode& candidate) {
                return candidate.width == mode.width && candidate.height == mode.height &&
                       candidate.refresh == mode.refresh;
            });
        if (!exists) {
            unique.push_back(mode);
        }
    }
    return unique;
}

bool ShowResolutionDialog(HWND owner, const std::vector<ResolutionMode>& modes, int defaultIndex,
                          int* selectedIndex)
{
    if (modes.empty() || selectedIndex == nullptr) {
        return false;
    }

    ResolutionDialogContext context;
    context.modes = &modes;
    context.defaultIndex = defaultIndex;

    const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr),
                                           MAKEINTRESOURCEW(IDD_RESOLUTION), owner,
                                           ResolutionDialogProc,
                                           reinterpret_cast<LPARAM>(&context));
    if (result == -1) {
        LogError("DialogBoxParam failed: %lu", GetLastError());
        return false;
    }
    if (result != IDOK || context.selectedIndex < 0) {
        return false;
    }
    *selectedIndex = context.selectedIndex;
    return true;
}

bool ParseResolution(const std::string& text, ResolutionMode* mode)
{
    const char* cursor = text.c_str();
    char* end = nullptr;
    const long width = std::strtol(cursor, &end, 10);
    if (end == cursor || (*end != 'x' && *end != 'X')) {
        return false;
    }
    cursor = end + 1;
    const long height = std::strtol(cursor, &end, 10);
    if (end == cursor || *end != '\0') {
        return false;
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return false;
    }
    mode->width = static_cast<int>(width);
    mode->height = static_cast<int>(height);
    mode->refresh = 60;
    return true;
}

int FindResolutionIndex(const std::vector<ResolutionMode>& modes, int width, int height)
{
    for (size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].width == width && modes[i].height == height) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

VirtualDisplay::~VirtualDisplay()
{
    Stop();
}

bool VirtualDisplay::Start(const ResolutionMode& mode, std::wstring* deviceName,
                           std::wstring* error)
{
    if (m_active) {
        return true;
    }
    if (!IsProcessElevated()) {
        *error = L"administrator privileges are required";
        return false;
    }

    std::wstring stateError;
    if (!SetVddEnabled(true, &stateError)) {
        *error = stateError;
        return false;
    }
    m_active = true;

    std::wstring name;
    if (!WaitForVddDisplay(&name, 3000)) {
        LogInfo("VDD adapter not attached yet, requesting extend");
        RunDisplaySwitchExtend();
        if (!WaitForVddDisplay(&name, 7000)) {
            *error = L"VDD is enabled but the virtual monitor did not appear";
            Stop();
            return false;
        }
    }

    if (!IsDisplayOnDesktop(name)) {
        LogInfo("VDD %ls is not in desktop topology, requesting extend", name.c_str());
        RunDisplaySwitchExtend();
        if (!WaitForDisplayOnDesktop(name, 5000)) {
            LogWarn("VDD %ls still not enumerated as an extended monitor", name.c_str());
        }
    }

    const LONG rc = ApplyDisplayMode(name, mode);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        *error = L"cannot set virtual display mode " + std::to_wstring(mode.width) + L"x" +
                 std::to_wstring(mode.height) + DisplayChangeError(rc);
        Stop();
        return false;
    }
    if (!CurrentModeMatches(name, mode)) {
        *error = L"virtual display mode did not take effect";
        Stop();
        return false;
    }

    *deviceName = name;
    LogInfo("virtual display %ls active at %dx%d", name.c_str(), mode.width, mode.height);
    return true;
}

void VirtualDisplay::Stop()
{
    if (!m_active) {
        return;
    }
    std::wstring error;
    if (!SetVddEnabled(false, &error)) {
        LogWarn("failed to disable VDD: %ls", error.c_str());
    } else {
        LogInfo("virtual display removed (VDD disabled)");
    }
    m_active = false;
}
