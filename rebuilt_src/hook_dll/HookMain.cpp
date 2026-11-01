#include <Windows.h>
#include <cstdio>
#include <cstring>
#include "SharedIPC.h"

#pragma comment(lib, "user32.lib")

static HANDLE          g_hMapFile    = nullptr;
static SharedHookData* g_pShared     = nullptr;
static HANDLE          g_hScrubEvent = nullptr;
static HANDLE          g_hInitThread = nullptr;
static volatile BOOL   g_running     = TRUE;

static void DbgLog(const char* fmt, ...) {
    char buf[512]{};
    int off = wsprintfA(buf, "[app_hook PID:%lu] ", GetCurrentProcessId());
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}

static void ScrubWdaNow(bool clear) {
    if (!g_pShared) return;

    if (clear) {
        DbgLog("Scrub: clearing WDA on %lu windows", g_pShared->trackedWindowCount);
        for (DWORD i = 0; i < g_pShared->trackedWindowCount; i++) {
            auto& tw = g_pShared->trackedWindows[i];
            if (!tw.tracked || !IsWindow(tw.hwnd)) continue;

            DWORD currentWda = 0;
            GetWindowDisplayAffinity(tw.hwnd, &currentWda);
            tw.originalWda = currentWda;

            BOOL ok = SetWindowDisplayAffinity(tw.hwnd, 0);
            DbgLog("  HWND %p WDA %lu->0 ok=%d GLE=%lu",
                tw.hwnd, currentWda, ok, ok ? 0 : GetLastError());
        }
        g_pShared->wdaCleared = TRUE;
    } else {
        for (DWORD i = 0; i < g_pShared->trackedWindowCount; i++) {
            auto& tw = g_pShared->trackedWindows[i];
            if (!tw.tracked || !IsWindow(tw.hwnd)) continue;
            SetWindowDisplayAffinity(tw.hwnd, tw.originalWda);
        }
        g_pShared->wdaCleared = FALSE;
        DbgLog("Scrub: WDA restored");
    }
}

static bool InitSharedMemory() {
    g_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemName);
    if (!g_hMapFile) return false;

    g_pShared = static_cast<SharedHookData*>(
        MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, kSharedMemSize));
    return g_pShared != nullptr;
}

static DWORD WINAPI HookInitThread(LPVOID) {
    bool ok = InitSharedMemory();
    DbgLog("Init: sharedMem=%s (%p)", ok ? "OK" : "FAIL", g_pShared);

    if (g_pShared)
        g_pShared->hookActive = TRUE;

    wchar_t eventName[128]{};
    wsprintfW(eventName, kHookActiveEventFmt, GetCurrentProcessId());
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (hEvent) { SetEvent(hEvent); CloseHandle(hEvent); }

    g_hScrubEvent = OpenEventW(SYNCHRONIZE, FALSE, kDwmCompositionNotify);
    if (!g_hScrubEvent)
        g_hScrubEvent = CreateEventW(nullptr, FALSE, FALSE, kDwmCompositionNotify);

    DbgLog("Init: ready (event=%p)", g_hScrubEvent);

    while (g_running) {
        DWORD wait = WaitForSingleObject(g_hScrubEvent, 500);
        if (wait == WAIT_OBJECT_0) {
            ScrubWdaNow(true);
            for (int i = 0; i < 400 && g_running; i++) {
                if (g_pShared && !g_pShared->wdaCleared) break;
                Sleep(5);
            }
            ScrubWdaNow(false);
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            g_hInitThread = CreateThread(nullptr, 0, HookInitThread, nullptr, 0, nullptr);
            break;

        case DLL_PROCESS_DETACH:
            g_running = FALSE;
            if (g_hScrubEvent) { SetEvent(g_hScrubEvent); CloseHandle(g_hScrubEvent); }
            if (g_hInitThread) { WaitForSingleObject(g_hInitThread, 3000); CloseHandle(g_hInitThread); }
            if (g_pShared) { g_pShared->hookActive = FALSE; UnmapViewOfFile(g_pShared); }
            if (g_hMapFile) CloseHandle(g_hMapFile);
            break;
    }
    return TRUE;
}
