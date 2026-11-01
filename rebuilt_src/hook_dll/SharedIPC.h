#pragma once
#include <Windows.h>

static constexpr wchar_t kHookActiveEventFmt[]   = L"Local\\UFOlee_Hook_Active_%lu";
static constexpr wchar_t kDwmCompositionNotify[]  = L"Local\\DWM_Composition_Notify";
static constexpr wchar_t kDwmCompositionReady[]   = L"Local\\DWM_Composition_Ready";
static constexpr wchar_t kDwmCompositionSession[] = L"Local\\DWM_Composition_Session";
static constexpr wchar_t kDwmCompositionData[]    = L"Local\\DWM_Composition_Data";
static constexpr wchar_t kDwmWdaMutexFmt[]        = L"Local\\DWM_WDA_Mutex";
static constexpr wchar_t kEncLogMutex[]           = L"Local\\UFOleeEncLogMutex";

static constexpr int kMaxTrackedWindows = 64;

struct SharedHookData {
    DWORD ownerPid;
    HWND  overlayHwnd;
    BOOL  hookActive;
    BOOL  wdaCleared;

    DWORD trackedWindowCount;
    struct {
        HWND  hwnd;
        DWORD originalWda;
        BOOL  tracked;
    } trackedWindows[kMaxTrackedWindows];
};

static constexpr wchar_t kSharedMemName[] = L"Local\\DWM_Composition_Data";
static constexpr DWORD   kSharedMemSize   = sizeof(SharedHookData);

static constexpr const wchar_t* kIgnoredProcesses[] = {
    L"explorer.exe",
    L"dwm.exe",
    L"sihost.exe",
    L"taskhostw.exe",
    L"RuntimeBroker.exe",
};

static constexpr const wchar_t* kSelfProcessNames[] = {
    L"UFOlee.exe",
    L"ufolee_render.exe",
    L"app_hook_loader.exe",
    L"stealth_launcher.exe",
    L"comet.exe",
    L"MYOWNAPP.exe",
    L"MYOWNAPP_rebuilt.exe",
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
