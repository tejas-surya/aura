#include "ViewportManager.h"
#include <ShellScalingApi.h>

ViewportManager::ViewportManager() {
    RefreshMonitors();
}

void ViewportManager::RefreshMonitors() {
    monitors_.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
        reinterpret_cast<LPARAM>(&monitors_));
}

BOOL CALLBACK ViewportManager::MonitorEnumProc(
    HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto monitors = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    MonitorInfo info{};
    info.hMonitor = hMon;
    info.rcWork = mi.rcWork;
    info.rcMonitor = mi.rcMonitor;
    info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    monitors->push_back(info);

    return TRUE;
}

RECT ViewportManager::GetPrimaryWorkArea() const {
    for (auto& m : monitors_) {
        if (m.isPrimary) return m.rcWork;
    }
    RECT rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    return rc;
}

RECT ViewportManager::GetNearestWorkArea(HWND hwnd) const {
    return GetNearestMonitor(hwnd).rcWork;
}

MonitorInfo ViewportManager::GetNearestMonitor(HWND hwnd) const {
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    for (auto& m : monitors_) {
        if (m.hMonitor == hMon) return m;
    }
    if (!monitors_.empty()) return monitors_[0];

    MonitorInfo fallback{};
    fallback.rcWork = GetPrimaryWorkArea();
    fallback.rcMonitor = fallback.rcWork;
    return fallback;
}

void ViewportManager::ClampToWorkArea(HWND hwnd) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    RECT work = GetNearestWorkArea(hwnd);

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = rc.left;
    int y = rc.top;

    if (x + w > work.right) x = work.right - w;
    if (y + h > work.bottom) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;

    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ViewportManager::CenterOnPrimary(HWND hwnd, int width, int height) {
    RECT work = GetPrimaryWorkArea();
    int x = work.left + (work.right - work.left - width) / 2;
    int y = work.top + (work.bottom - work.top - height) / 2;
    SetWindowPos(hwnd, nullptr, x, y, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

int ViewportManager::GetDpiForWindow(HWND hwnd) const {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        auto fn = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (fn) return static_cast<int>(fn(hwnd));
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    return dpi;
}

float ViewportManager::GetScaleFactor(HWND hwnd) const {
    return static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
}
