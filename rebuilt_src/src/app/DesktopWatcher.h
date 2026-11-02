#pragma once
#include <Windows.h>
#include <string>
#include <set>
#include <map>

class DesktopWatcher {
public:
    DesktopWatcher();
    ~DesktopWatcher();

    void Start();
    void Stop();

private:
    static DWORD WINAPI WatchThread(LPVOID param);
    void WatchLoop();
    bool RelaunchOnDesktop(const std::wstring& desktopName);
    DWORD FindExplorerPid();

    HANDLE hThread_ = nullptr;
    volatile BOOL running_ = FALSE;
    std::set<std::wstring> knownDesktops_;
    std::map<std::wstring, HANDLE> satelliteProcs_;
};
