#pragma once
#include <Windows.h>

// The shared memory name is the only static kernel object name (bootstrap).
// All other names use the per-session suffix stored in SharedHookData.
static constexpr wchar_t kSharedMemName[] = L"Local\\WinRT_Comp_SharedData";

// Format strings for building per-session names.  %s = sessionSuffix.
static constexpr wchar_t kHookActiveEventFmt[]   = L"Local\\WinRT_Comp_ModReady_%lu_%s";
static constexpr wchar_t kScrubEventFmt[]         = L"Local\\WinRT_Comp_FS_%s";

static constexpr int kMaxTrackedWindows = 64;

struct SharedHookData {
    DWORD ownerPid;
    HWND  overlayHwnd;
    BOOL  hookActive;
    BOOL  wdaCleared;

    wchar_t sessionSuffix[16];

    DWORD trackedWindowCount;
    struct {
        HWND  hwnd;
        DWORD originalWda;
        BOOL  tracked;
    } trackedWindows[kMaxTrackedWindows];
};

static constexpr DWORD kSharedMemSize = sizeof(SharedHookData);

static constexpr const wchar_t* kIgnoredProcesses[] = {
    L"explorer.exe",
    L"dwm.exe",
    L"sihost.exe",
    L"taskhostw.exe",
    L"RuntimeBroker.exe",
};

static constexpr const wchar_t* kSelfProcessNames[] = {
    L"DesktopWidget.exe",
};

static constexpr const wchar_t* kBrowserProcesses[] = {
    L"msedgewebview2.exe",
    L"MicrosoftEdge.exe",
    L"msedge.exe",
    L"chrome.exe",
    L"brave.exe",
    L"vivaldi.exe",
    L"opera.exe",
    L"firefox.exe",
    L"iexplore.exe",
    L"CefSharp.BrowserSubprocess.exe",
    L"AcroCEF.exe",
};

static constexpr const wchar_t* kSecurityProcesses[] = {
    L"MsMpEng.exe",
    L"MpCmdRun.exe",
    L"SecurityHealthService.exe",
    L"bduserhost.exe",
    L"bdagent.exe",
};
