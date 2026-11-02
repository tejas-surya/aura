#include <WinSock2.h>
#include <Windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <objbase.h>
#include <gdiplus.h>
#include <aclapi.h>
#include "app/AppController.h"
#include "app/RegistrySetup.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")

static constexpr wchar_t kMutexName[] = L"Local\\DWM_Composition_Lock";

static LONG WINAPI CrashGuard(EXCEPTION_POINTERS*) {
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ProtectProcess() {
    SID_IDENTIFIER_AUTHORITY worldAuth = SECURITY_WORLD_SID_AUTHORITY;
    PSID pEveryoneSid = nullptr;
    if (!AllocateAndInitializeSid(&worldAuth, 1, SECURITY_WORLD_RID,
            0, 0, 0, 0, 0, 0, 0, &pEveryoneSid))
        return;

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = PROCESS_TERMINATE;
    ea.grfAccessMode = DENY_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(pEveryoneSid);

    PACL pOldDacl = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    HANDLE hProc = GetCurrentProcess();

    if (GetSecurityInfo(hProc, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &pOldDacl, nullptr, &pSD) == ERROR_SUCCESS) {
        PACL pNewDacl = nullptr;
        if (SetEntriesInAclW(1, &ea, pOldDacl, &pNewDacl) == ERROR_SUCCESS) {
            SetSecurityInfo(hProc, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                nullptr, nullptr, pNewDacl, nullptr);
            LocalFree(pNewDacl);
        }
    }

    if (pSD) LocalFree(pSD);
    FreeSid(pEveryoneSid);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    SetUnhandledExceptionFilter(CrashGuard);
    ProtectProcess();

    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        L"--no-sandbox --in-process-gpu --force-device-scale-factor=1 "
        L"--disable-features=RendererCodeIntegrity,FlashDeprecation,"
        L"CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,"
        L"msWebView2BrowserHitTransparent "
        L"--allow-no-sandbox-job --disable-win32k-lockdown");
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isSetup = false;
    bool isCleanup = false;
    bool isDesktopInstance = false;
    std::wstring desktopName;
    DWORD setupParentPid = 0;

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--setup") == 0 && i + 1 < argc) {
            isSetup = true;
            setupParentPid = static_cast<DWORD>(_wtoi(argv[i + 1]));
            i++;
        } else if (_wcsicmp(argv[i], L"--cleanup") == 0) {
            isCleanup = true;
        } else if (_wcsicmp(argv[i], L"--desktop-instance") == 0) {
            isDesktopInstance = true;
            if (i + 1 < argc) {
                desktopName = argv[i + 1];
                i++;
            }
        }
    }
    LocalFree(argv);

    if (isSetup)   return RegistrySetup::RunSetupHelper(setupParentPid);
    if (isCleanup) return RegistrySetup::RunCleanupHelper();

    HANDLE hMutex = nullptr;
    if (!isDesktopInstance) {
        hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
    }

    if (isDesktopInstance && !desktopName.empty()) {
        HDESK hDesk = OpenDesktopW(desktopName.c_str(), 0, FALSE, GENERIC_ALL);
        if (!hDesk)
            hDesk = OpenDesktopW(desktopName.c_str(), 0, FALSE, MAXIMUM_ALLOWED);
        if (hDesk)
            SetThreadDesktop(hDesk);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    WSADATA wsaData{};
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    {
        AppController app(hInstance, isDesktopInstance);
        if (!app.Initialize(nCmdShow)) {
            if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
            return 1;
        }
        app.Run();
    }

    WSACleanup();
    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return 0;
}
