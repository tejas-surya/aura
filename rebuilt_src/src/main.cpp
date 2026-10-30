#include <WinSock2.h>
#include <Windows.h>
#include <objidl.h>
#include <objbase.h>
#include <gdiplus.h>
#include "app/AppController.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")

static constexpr wchar_t kMutexName[] = L"Local\\MYOWNAPP_SingleInstance_Mutex";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    WSADATA wsaData{};
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    {
        AppController app(hInstance);
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
