#include "desktop.h"

namespace {

    constexpr UINT WM_SPAWN_WORKERW = 0x052C;

    constexpr int kAttempts = 40;
    constexpr DWORD kAttemptDelayMs = 50;

    BOOL CALLBACK MonitorProc(const HMONITOR monitor, HDC /*hdc*/, LPRECT /*clip*/, const LPARAM lparam)
    {
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info))
            reinterpret_cast<std::vector<RECT>*>(lparam)->push_back(info.rcMonitor);
        return TRUE;
    }

}

HWND AcquireWallpaperHost()
{
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman == nullptr)
        return nullptr;

    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, WM_SPAWN_WORKERW, 0, 0, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, WM_SPAWN_WORKERW, 0x0D, 0, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, WM_SPAWN_WORKERW, 0x0D, 1, SMTO_NORMAL, 1000, &result);

    for (int attempt = 0; attempt < kAttempts; ++attempt)
    {
        if (const HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); worker != nullptr)
            return worker;
        Sleep(kAttemptDelayMs);
    }

    return nullptr;
}

std::vector<RECT> EnumerateMonitors()
{
    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}