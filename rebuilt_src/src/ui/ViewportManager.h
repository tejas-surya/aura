#pragma once
#include <Windows.h>
#include <vector>

struct MonitorInfo {
    HMONITOR hMonitor;
    RECT rcWork;
    RECT rcMonitor;
    bool isPrimary;
};

class ViewportManager {
public:
    ViewportManager();

    void RefreshMonitors();
    RECT GetPrimaryWorkArea() const;
    RECT GetNearestWorkArea(HWND hwnd) const;
    MonitorInfo GetNearestMonitor(HWND hwnd) const;

    void ClampToWorkArea(HWND hwnd);
    void CenterOnPrimary(HWND hwnd, int width, int height);
    int GetDpiForWindow(HWND hwnd) const;
    float GetScaleFactor(HWND hwnd) const;

private:
    static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam);
    std::vector<MonitorInfo> monitors_;
};
