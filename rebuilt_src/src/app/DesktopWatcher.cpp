#include "DesktopWatcher.h"
#include <TlHelp32.h>
#include <filesystem>

namespace fs = std::filesystem;

DesktopWatcher::DesktopWatcher() {}

DesktopWatcher::~DesktopWatcher() {
    Stop();
}

struct DesktopEnumCtx {
    std::set<std::wstring>* desktops;
};

static BOOL CALLBACK DesktopEnumProc(LPWSTR lpszDesktop, LPARAM lParam) {
    auto* ctx = reinterpret_cast<DesktopEnumCtx*>(lParam);
    ctx->desktops->insert(lpszDesktop);
    return TRUE;
}

void DesktopWatcher::Start() {
    HWINSTA hWinSta = GetProcessWindowStation();
    if (hWinSta) {
        DesktopEnumCtx ctx{&knownDesktops_};
        EnumDesktopsW(hWinSta, DesktopEnumProc, reinterpret_cast<LPARAM>(&ctx));
    }

    running_ = TRUE;
    hThread_ = CreateThread(nullptr, 0, WatchThread, this, 0, nullptr);
}

void DesktopWatcher::Stop() {
    running_ = FALSE;
    if (hThread_) {
        WaitForSingleObject(hThread_, 5000);
        CloseHandle(hThread_);
        hThread_ = nullptr;
    }
    for (auto& [name, handle] : satelliteProcs_) {
        CloseHandle(handle);
    }
    satelliteProcs_.clear();
}

DWORD WINAPI DesktopWatcher::WatchThread(LPVOID param) {
    auto* self = static_cast<DesktopWatcher*>(param);
    self->WatchLoop();
    return 0;
}

void DesktopWatcher::WatchLoop() {
    while (running_) {
        for (int i = 0; i < 30 && running_; i++) Sleep(100);
        if (!running_) break;

        HWINSTA hWinSta = GetProcessWindowStation();
        if (!hWinSta) continue;

        std::set<std::wstring> current;
        DesktopEnumCtx ctx{&current};
        EnumDesktopsW(hWinSta, DesktopEnumProc, reinterpret_cast<LPARAM>(&ctx));

        std::set<std::wstring> needsRelaunch;
        for (auto it = satelliteProcs_.begin(); it != satelliteProcs_.end(); ) {
            if (WaitForSingleObject(it->second, 0) == WAIT_OBJECT_0) {
                CloseHandle(it->second);
                needsRelaunch.insert(it->first);
                it = satelliteProcs_.erase(it);
            } else {
                ++it;
            }
        }

        for (auto& name : current) {
            bool isNew = knownDesktops_.find(name) == knownDesktops_.end();
            bool respawn = needsRelaunch.count(name) > 0;
            if (isNew || respawn) {
                RelaunchOnDesktop(name);
            }
        }
        knownDesktops_ = current;
    }
}

DWORD DesktopWatcher::FindExplorerPid() {
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD explorerPid = 0;

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                DWORD sessId = 0;
                if (ProcessIdToSessionId(pe.th32ProcessID, &sessId) &&
                    sessId == mySession) {
                    explorerPid = pe.th32ProcessID;
                    break;
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return explorerPid;
}

bool DesktopWatcher::RelaunchOnDesktop(const std::wstring& desktopName) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring workDir = fs::path(exePath).parent_path().wstring();

    std::wstring cmdLine = L"\"";
    cmdLine += exePath;
    cmdLine += L"\" --desktop-instance \"";
    cmdLine += desktopName;
    cmdLine += L"\"";

    std::wstring desktop = L"WinSta0\\" + desktopName;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(desktop.c_str());
    PROCESS_INFORMATION pi{};

    bool launched = false;

    DWORD explorerPid = FindExplorerPid();
    if (explorerPid) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, explorerPid);
        if (hProc) {
            HANDLE hToken = nullptr;
            if (OpenProcessToken(hProc,
                    TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY, &hToken)) {
                HANDLE hDupToken = nullptr;
                if (DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr,
                        SecurityImpersonation, TokenPrimary, &hDupToken)) {
                    launched = CreateProcessWithTokenW(hDupToken, 0, nullptr,
                        const_cast<LPWSTR>(cmdLine.c_str()),
                        0, nullptr, workDir.c_str(), &si, &pi) != FALSE;

                    if (!launched) {
                        launched = CreateProcessAsUserW(hDupToken, nullptr,
                            const_cast<LPWSTR>(cmdLine.c_str()),
                            nullptr, nullptr, FALSE, 0, nullptr,
                            workDir.c_str(), &si, &pi) != FALSE;
                    }
                    CloseHandle(hDupToken);
                }
                CloseHandle(hToken);
            }
            CloseHandle(hProc);
        }
    }

    if (!launched) {
        launched = CreateProcessW(nullptr,
            const_cast<LPWSTR>(cmdLine.c_str()),
            nullptr, nullptr, FALSE, 0, nullptr,
            workDir.c_str(), &si, &pi) != FALSE;
    }

    if (launched) {
        satelliteProcs_[desktopName] = pi.hProcess;
        CloseHandle(pi.hThread);
    }
    return launched;
}
