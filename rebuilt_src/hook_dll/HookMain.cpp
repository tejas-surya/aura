#include <Windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "SharedIPC.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include "MinHook/MinHook.h"

// Shared memory
static HANDLE              g_hMapFile   = nullptr;
static SharedHookData*     g_pShared    = nullptr;
static HANDLE              g_hScrubEvent = nullptr;
static HANDLE              g_hInitThread = nullptr;
static volatile BOOL       g_running    = TRUE;

// Original function pointers
typedef BOOL (WINAPI* PFN_SetWindowDisplayAffinity)(HWND, DWORD);
typedef BOOL (WINAPI* PFN_GetWindowDisplayAffinity)(HWND, DWORD*);
typedef BOOL (WINAPI* PFN_PrintWindow)(HWND, HDC, UINT);
typedef BOOL (WINAPI* PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef HWND (WINAPI* PFN_ChildWindowFromPointEx)(HWND, POINT, UINT);

static PFN_SetWindowDisplayAffinity fpSetWDA       = nullptr;
static PFN_GetWindowDisplayAffinity fpGetWDA       = nullptr;
static PFN_PrintWindow             fpPrintWindow   = nullptr;
static PFN_BitBlt                  fpBitBlt        = nullptr;
static PFN_ChildWindowFromPointEx  fpChildWndPoint = nullptr;

// Encrypted debug logging
static HANDLE g_hLogMutex = nullptr;

static void EncryptedLog(const char* fmt, ...) {
    if (!g_hLogMutex) {
        g_hLogMutex = CreateMutexW(nullptr, FALSE, kEncLogMutex);
    }
    WaitForSingleObject(g_hLogMutex, 1000);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char buf[2048]{};
    int offset = wsprintfA(buf, "[%02d:%02d:%02d.%03d][Hooked PID:%lu] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        GetCurrentProcessId());

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + offset, sizeof(buf) - offset, fmt, args);
    va_end(args);
    strcat(buf, "\n");

    OutputDebugStringA(buf);

    // Append to encrypted log file
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(exePath, L"debug_log.enc");

    // Read existing, decrypt, append, re-encrypt
    HANDLE hFile = CreateFileW(exePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize == INVALID_FILE_SIZE) fileSize = 0;

        // For simplicity, append plaintext then encrypt entire file
        // Real implementation uses AES CBC with per-machine key
        HCRYPTPROV hProv = 0;
        CryptAcquireContextW(&hProv, nullptr,
            MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);

        if (hProv) {
            // Simple XOR-based log for now (replace with full AES in production)
            DWORD written = 0;
            SetFilePointer(hFile, 0, nullptr, FILE_END);
            DWORD len = static_cast<DWORD>(strlen(buf));
            // XOR with PID-based key for basic obfuscation
            BYTE key = static_cast<BYTE>(GetCurrentProcessId() & 0xFF);
            for (DWORD i = 0; i < len; i++) buf[i] ^= key;
            WriteFile(hFile, buf, len, &written, nullptr);

            CryptReleaseContext(hProv, 0);
        }
        CloseHandle(hFile);
    }

    ReleaseMutex(g_hLogMutex);
}

// Check if HWND is one of our tracked overlay windows
static bool IsTrackedWindow(HWND hwnd) {
    if (!g_pShared) return false;
    for (DWORD i = 0; i < g_pShared->trackedWindowCount; i++) {
        if (g_pShared->trackedWindows[i].hwnd == hwnd &&
            g_pShared->trackedWindows[i].tracked)
            return true;
    }
    return false;
}

static bool IsOverlayWindow(HWND hwnd) {
    return g_pShared && g_pShared->overlayHwnd == hwnd;
}

// Hook: SetWindowDisplayAffinity
static BOOL WINAPI MySetWDA(HWND hwnd, DWORD affinity) {
    EncryptedLog("MySetWDA: HWND %p requesting WDA: %lu", hwnd, affinity);

    if (IsOverlayWindow(hwnd) || IsTrackedWindow(hwnd)) {
        EncryptedLog("MySetWDA: HWND %p actually set to %lu, success: 1", hwnd, affinity);
        return TRUE;
    }
    return fpSetWDA(hwnd, affinity);
}

// Hook: GetWindowDisplayAffinity
static BOOL WINAPI MyGetWDA(HWND hwnd, DWORD* affinity) {
    if (IsTrackedWindow(hwnd) && g_pShared && g_pShared->wdaCleared) {
        if (affinity) *affinity = 0;
        EncryptedLog("MyGetWDA: HWND %p queried. Real WDA is temporarily cleared, returning spoofed/fake WDA: 0");
        return TRUE;
    }

    BOOL result = fpGetWDA(hwnd, affinity);
    EncryptedLog("MyGetWDA: HWND %p queried (not tracked). Returning real WDA: %lu, success: %d",
        hwnd, affinity ? *affinity : 0, result);
    return result;
}

// Hook: PrintWindow - fake black output for tracked windows
static BOOL WINAPI MyPrintWindow(HWND hwnd, HDC hdc, UINT flags) {
    if (IsTrackedWindow(hwnd) && g_pShared && g_pShared->wdaCleared) {
        EncryptedLog("MyPrintWindow intercepted for HWND %p (spoofed WDA exists). Faking black screen output.", hwnd);
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        RECT fillRc = {0, 0, w, h};
        FillRect(hdc, &fillRc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return TRUE;
    }
    return fpPrintWindow(hwnd, hdc, flags);
}

// Hook: BitBlt - fake black output for tracked windows
static BOOL WINAPI MyBitBlt(HDC hdcDest, int x, int y, int cx, int cy,
                            HDC hdcSrc, int x1, int y1, DWORD rop) {
    HWND srcWnd = WindowFromDC(hdcSrc);
    if (srcWnd && IsTrackedWindow(srcWnd) && g_pShared && g_pShared->wdaCleared) {
        EncryptedLog("MyBitBlt intercepted for HWND %p (spoofed WDA exists). Faking black screen output.", srcWnd);
        RECT fillRc = {x, y, x + cx, y + cy};
        FillRect(hdcDest, &fillRc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return TRUE;
    }
    return fpBitBlt(hdcDest, x, y, cx, cy, hdcSrc, x1, y1, rop);
}

// Hook: ChildWindowFromPointEx - spoof overlay to parent
static HWND WINAPI MyChildWindowFromPointEx(HWND hwndParent, POINT pt, UINT flags) {
    HWND result = fpChildWndPoint(hwndParent, pt, flags);
    if (result && IsOverlayWindow(result)) {
        EncryptedLog("MyChildWindowFromPointEx: Hit our overlay HWND %p inside parent HWND %p. Spoofing to return parent.",
            result, hwndParent);
        return hwndParent;
    }
    return result;
}

// WDA Scrub routine
static void ScrubWdaNow(bool clear) {
    if (!g_pShared) return;

    if (clear) {
        EncryptedLog("ScrubNow event: Starting WDA removal loop for %d tracked windows...",
            g_pShared->trackedWindowCount);
        for (DWORD i = 0; i < g_pShared->trackedWindowCount; i++) {
            auto& tw = g_pShared->trackedWindows[i];
            if (!tw.tracked || !IsWindow(tw.hwnd)) continue;

            DWORD currentWda = 0;
            if (fpGetWDA) fpGetWDA(tw.hwnd, &currentWda);
            tw.originalWda = currentWda;

            BOOL ok = fpSetWDA ? fpSetWDA(tw.hwnd, 0) : FALSE;
            EncryptedLog("ScrubNow event: Temporarily cleared HWND %p WDA, success: %d",
                tw.hwnd, ok);
        }
        g_pShared->wdaCleared = TRUE;
        EncryptedLog("ScrubNow event: WDA removal completed, shared memory updated (cleared=TRUE).");
    } else {
        EncryptedLog("ScrubNow event: Restoring original WDA on %d tracked windows...",
            g_pShared->trackedWindowCount);
        for (DWORD i = 0; i < g_pShared->trackedWindowCount; i++) {
            auto& tw = g_pShared->trackedWindows[i];
            if (!tw.tracked || !IsWindow(tw.hwnd)) continue;

            BOOL ok = fpSetWDA ? fpSetWDA(tw.hwnd, tw.originalWda) : FALSE;
            EncryptedLog("ScrubNow event: Restored HWND %p to WDA %lu, success: %d",
                tw.hwnd, tw.originalWda, ok);
        }
        g_pShared->wdaCleared = FALSE;
        EncryptedLog("ScrubNow event: WDA restoration completed, shared memory updated (cleared=FALSE).");
    }
}

// Initialize shared memory
static bool InitSharedMemory() {
    g_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemName);
    if (!g_hMapFile) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;

        SECURITY_DESCRIPTOR sd{};
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
        sa.lpSecurityDescriptor = &sd;

        g_hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
            PAGE_READWRITE, 0, kSharedMemSize, kSharedMemName);
    }

    if (!g_hMapFile) return false;

    g_pShared = static_cast<SharedHookData*>(
        MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, kSharedMemSize));

    return g_pShared != nullptr;
}

// Install hooks
static bool InstallHooks() {
    if (MH_Initialize() != MH_OK) return false;

    MH_CreateHookApi(L"user32.dll", "SetWindowDisplayAffinity",
        reinterpret_cast<void*>(MySetWDA), reinterpret_cast<void**>(&fpSetWDA));

    MH_CreateHookApi(L"user32.dll", "GetWindowDisplayAffinity",
        reinterpret_cast<void*>(MyGetWDA), reinterpret_cast<void**>(&fpGetWDA));

    MH_CreateHookApi(L"user32.dll", "PrintWindow",
        reinterpret_cast<void*>(MyPrintWindow), reinterpret_cast<void**>(&fpPrintWindow));

    MH_CreateHookApi(L"gdi32.dll", "BitBlt",
        reinterpret_cast<void*>(MyBitBlt), reinterpret_cast<void**>(&fpBitBlt));

    MH_CreateHookApi(L"user32.dll", "ChildWindowFromPointEx",
        reinterpret_cast<void*>(MyChildWindowFromPointEx),
        reinterpret_cast<void**>(&fpChildWndPoint));

    MH_EnableHook(MH_ALL_HOOKS);
    return true;
}

// Hook initialization thread
static DWORD WINAPI HookInitThread(LPVOID) {
    InitSharedMemory();

    if (g_pShared) {
        g_pShared->ownerPid = GetCurrentProcessId();
        g_pShared->hookActive = TRUE;
    }

    InstallHooks();

    // Signal hook ready
    wchar_t eventName[128]{};
    wsprintfW(eventName, kHookActiveEventFmt, GetCurrentProcessId());
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (hEvent) {
        SetEvent(hEvent);
        CloseHandle(hEvent);
    }

    // Scrub event listener
    g_hScrubEvent = CreateEventW(nullptr, FALSE, FALSE, kDwmCompositionNotify);
    while (g_running) {
        DWORD wait = WaitForSingleObject(g_hScrubEvent, 500);
        if (wait == WAIT_OBJECT_0) {
            ScrubWdaNow(true);
            Sleep(100);
            ScrubWdaNow(false);
        }
    }

    return 0;
}

// DllMain
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hModule);

            wchar_t ipcName[128]{};
            wsprintfW(ipcName, kHookActiveEventFmt, GetCurrentProcessId());

            EncryptedLog("DllMain: [SUCCESS] app_hook.dll ATTACHED (IPC=%S)! Spawning hook initialization thread...",
                ipcName);

            g_hInitThread = CreateThread(nullptr, 0, HookInitThread, nullptr, 0, nullptr);
            break;
        }

        case DLL_PROCESS_DETACH: {
            g_running = FALSE;

            if (g_hScrubEvent) {
                SetEvent(g_hScrubEvent);
                CloseHandle(g_hScrubEvent);
            }

            if (g_hInitThread) {
                WaitForSingleObject(g_hInitThread, 3000);
                CloseHandle(g_hInitThread);
            }

            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();

            if (g_pShared) {
                g_pShared->hookActive = FALSE;
                UnmapViewOfFile(g_pShared);
            }
            if (g_hMapFile) CloseHandle(g_hMapFile);
            if (g_hLogMutex) CloseHandle(g_hLogMutex);
            break;
        }
    }
    return TRUE;
}
