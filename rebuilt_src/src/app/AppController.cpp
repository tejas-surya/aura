#include "AppController.h"
#include "ConfigManager.h"
#include "StateManager.h"
#include "RegistrySetup.h"
#include "DesktopWatcher.h"
#include "../ui/OverlayWindow.h"
#include "../ui/ViewportManager.h"
#include "../browser/WebViewManager.h"
#include "../browser/WebBridgeEvents.h"
#include "../input/HotkeyManager.h"
#include "../input/TypingSimulator.h"
#include "../automation/ScreenCapture.h"
#include "../automation/TextExtractor.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include "SharedIPC.h"

#include <Psapi.h>
#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

AppController* AppController::s_instance_ = nullptr;

#ifdef NDEBUG
static inline void DbgLog(const char*, ...) {}
#else
static void DbgLog(const char* fmt, ...) {
    char buf[512]{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}
#endif

AppController::AppController(HINSTANCE hInstance, bool isDesktopInstance)
    : hInstance_(hInstance), isDesktopInstance_(isDesktopInstance) {
    InitializeCriticalSection(&injectLock_);
}

AppController::~AppController() {
    Shutdown();
    DeleteCriticalSection(&injectLock_);
}

bool AppController::Initialize(int nCmdShow) {
    config_ = std::make_unique<ConfigManager>();
    if (!config_->Load()) {
        config_->CreateDefaults();
        config_->Save();
    }

    state_ = std::make_unique<StateManager>();

    currentOpacity_ = config_->GetInt("opacity", 100);

    viewport_ = std::make_unique<ViewportManager>();
    overlay_ = std::make_unique<OverlayWindow>(*this);
    if (!overlay_->Create(
        config_->GetInt("width", 350),
        config_->GetInt("height", 500),
        currentOpacity_)) {
        return false;
    }

    webView_ = std::make_unique<WebViewManager>(*this);
    if (!webView_->Initialize(overlay_->GetHwnd())) {
        return false;
    }

    std::wstring defaultSite = config_->GetWString("defaultWebsite", L"chatgpt");
    webView_->NavigateToSite(defaultSite);

    hotkeys_ = std::make_unique<HotkeyManager>(*this);
    hotkeys_->RegisterFromConfig(*config_);

    typingSim_ = std::make_unique<TypingSimulator>();
    typingSim_->SetDelay(config_->GetInt("typeDelayMs", 200));
    typingSim_->SetHumanMode(config_->GetBool("humanTyping", true));

    screenCap_ = std::make_unique<ScreenCapture>();
    textExtract_ = std::make_unique<TextExtractor>();

    InitHookSystem();

    // Phase 1: AppInit_DLLs — only the primary instance sets up the registry.
    if (!isDesktopInstance_) {
        RegistrySetup::EnsureAppInit();
    }

    // Phase 2: Multi-desktop relaunch — only the primary instance watches.
    if (!isDesktopInstance_) {
        desktopWatcher_ = std::make_unique<DesktopWatcher>();
        desktopWatcher_->Start();
    }

    // Phase 3: Reactive foreground-change injection + initial sweep.
    StartReactiveInjection();
    SweepExistingProcesses();

    overlay_->Show(SW_SHOWNOACTIVATE);

    return true;
}

void AppController::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void AppController::Shutdown() {
    StopReactiveInjection();
    if (desktopWatcher_) desktopWatcher_->Stop();
    CleanupHookSystem();
    if (hotkeys_) hotkeys_->UnregisterAll();
    if (config_) {
        config_->SetInt("opacity", currentOpacity_);
        config_->Save();
    }
}

void AppController::OnHotkeyAction(int actionId) {
    switch (actionId) {
        case 4:  ToggleVisibility(); break;
        case 1:  CaptureAndSend(); break;
        case 15: ExtractUIText(); break;
        case 16: ExtractAndSendAI(); break;
        case 14: ToggleVoiceCopilot(); break;
        case 5:  AutoTypeSelection(); break;
        case 6:  AdjustOpacity(-5); break;
        case 7:  AdjustOpacity(5); break;
        case 9:  ToggleClickThrough(); break;
        case 10: webView_->RotateToNextSite(); break;
        case 11: webView_->RotateToPrevSite(); break;
        case 23: MoveWindow(0, -30); break;
        case 24: MoveWindow(0, 30); break;
        case 17: ScrollPage(-1); break;
        case 18: ScrollPage(1); break;
        case 8:  RotateWebsite(); break;
        case 12: AdjustTypingSpeed(-10); break;
        case 13: AdjustTypingSpeed(10); break;
        case 19: ToggleTextOnlyMode(); break;
        case 20: PanicHide(); break;
        case 25: ToggleMouseOnlyMode(); break;
        case 26: AdjustZoom(0.1); break;
        case 27: AdjustZoom(-0.1); break;
        default: break;
    }
}

void AppController::OnWebMessage(const std::wstring& message) {
    try {
        std::string narrow(message.size(), '\0');
        for (size_t i = 0; i < message.size(); i++)
            narrow[i] = static_cast<char>(message[i] & 0xFF);
        auto j = nlohmann::json::parse(narrow);
        std::string type = j.value("type", "");

        if (type == "aiResponse") {
            std::string text = j.value("text", "");
            lastAiAnswer_ = std::wstring(text.begin(), text.end());
        } else if (type == "ready") {
            if (textOnlyMode_) {
                webView_->InjectTextOnlyMode(true);
            }
        }
    } catch (...) {}
}

void AppController::ToggleVisibility() {
    visible_ = !visible_;
    if (visible_) {
        overlay_->Show(SW_SHOWNOACTIVATE);
        if (panicHidden_) {
            panicHidden_ = false;
            overlay_->SetOpacity(currentOpacity_);
            overlay_->RestoreToTaskbar();

            RECT workArea{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            int x = workArea.right - config_->GetInt("width", 350) - 20;
            int y = workArea.top + 50;
            overlay_->MoveTo(x, y);
        }
    } else {
        overlay_->Hide();
    }
}

void AppController::PanicHide() {
    panicHidden_ = true;
    visible_ = false;
    overlay_->SetOpacity(0);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int offscreenX = workArea.left - 5000;
    int offscreenY = workArea.top;
    overlay_->MoveTo(offscreenX, offscreenY);

    if (!textOnlyMode_) {
        textOnlyMode_ = true;
        webView_->InjectTextOnlyMode(true);
    }

    overlay_->RemoveFromTaskbar();
}

void AppController::ToggleTextOnlyMode() {
    textOnlyMode_ = !textOnlyMode_;
    webView_->InjectTextOnlyMode(textOnlyMode_);
}

void AppController::ToggleMouseOnlyMode() {
    mouseOnlyMode_ = !mouseOnlyMode_;
    if (mouseOnlyMode_) {
        overlay_->SetOpacity(0);
        overlay_->EnableMouseReveal(true);
    } else {
        overlay_->SetOpacity(currentOpacity_);
        overlay_->EnableMouseReveal(false);
    }
}

void AppController::ToggleClickThrough() {
    clickThrough_ = !clickThrough_;
    overlay_->SetClickThrough(clickThrough_);
}

void AppController::AdjustOpacity(int delta) {
    currentOpacity_ = std::clamp(currentOpacity_ + delta, 5, 100);
    overlay_->SetOpacity(currentOpacity_);
    if (mouseOnlyMode_) {
        mouseOnlyMode_ = false;
        overlay_->EnableMouseReveal(false);
    }
}

void AppController::MoveWindow(int dx, int dy) {
    RECT rc{};
    GetWindowRect(overlay_->GetHwnd(), &rc);
    overlay_->MoveTo(rc.left + dx, rc.top + dy);
}

void AppController::ScrollPage(int direction) {
    webView_->ExecuteScript(
        direction < 0
            ? L"window.scrollBy(0, -200);"
            : L"window.scrollBy(0, 200);");
}

void AppController::AdjustTypingSpeed(int delta) {
    int current = config_->GetInt("typeDelayMs", 200);
    current = std::clamp(current + delta, 10, 1000);
    config_->SetInt("typeDelayMs", current);
    typingSim_->SetDelay(current);
}

void AppController::AdjustZoom(double delta) {
    if (!OverlayWindow::IsKeyTargetWebView()) return;
    if (webView_) webView_->SetZoomDelta(delta);
}

void AppController::RotateWebsite() {
    webView_->RotateToNextSite();
}

struct EnumWdaContext {
    DWORD targetPid;
    SharedHookData* pShared;
};

static BOOL CALLBACK EnumWdaProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumWdaContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->targetPid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;

    DWORD wda = 0;
    if (GetWindowDisplayAffinity(hwnd, &wda) && wda != 0) {
        DWORD idx = ctx->pShared->trackedWindowCount;
        if (idx < kMaxTrackedWindows) {
            ctx->pShared->trackedWindows[idx].hwnd = hwnd;
            ctx->pShared->trackedWindows[idx].originalWda = wda;
            ctx->pShared->trackedWindows[idx].tracked = TRUE;
            ctx->pShared->trackedWindowCount = idx + 1;
        }
    }
    return TRUE;
}

void AppController::CaptureAndSend() {
    unsigned long long now = GetTickCount64();
    if (shotInProgress_ || (now - lastShotTick_) < kShotDebounceMs) {
        return;
    }
    shotInProgress_ = true;
    lastShotTick_ = now;

    if (!webView_ || !webView_->IsReady()) {
        shotInProgress_ = false;
        return;
    }

    HWND fgWnd = GetForegroundWindow();
    bool targetHasWda = false;
    DWORD targetPid = 0;

    if (fgWnd && fgWnd != overlay_->GetHwnd() && pShared_ && !hookDllPath_.empty()) {
        GetWindowThreadProcessId(fgWnd, &targetPid);

        pShared_->trackedWindowCount = 0;
        EnumWdaContext ctx{ targetPid, pShared_ };
        EnumWindows(EnumWdaProc, reinterpret_cast<LPARAM>(&ctx));
        targetHasWda = (pShared_->trackedWindowCount > 0);

        DbgLog("[CAPTURE] fg=%p pid=%lu wdaWindows=%lu hookDll=%d",
            fgWnd, targetPid, pShared_->trackedWindowCount, !hookDllPath_.empty());

        if (targetHasWda) {
            bool injected = InjectIntoPid(targetPid);
            DbgLog("[CAPTURE] InjectIntoPid(%lu) => %d", targetPid, injected);
        }
    }

    overlay_->SuspendCaptureProtection();
    Sleep(50);

    if (targetHasWda && hScrubEvent_ && pShared_) {
        SetEvent(hScrubEvent_);
        int polls = 0;
        for (int i = 0; i < 30; i++) {
            if (pShared_->wdaCleared) break;
            Sleep(1);
            polls++;
        }
        DbgLog("[CAPTURE] wdaCleared=%d after %d polls", pShared_->wdaCleared, polls);
    }

    auto bitmap = screenCap_->CaptureVirtualScreen();

    if (targetHasWda && pShared_) {
        pShared_->wdaCleared = FALSE;
    }

    overlay_->RestoreCaptureProtection();

    DbgLog("[CAPTURE] bitmap=%zu bytes, targetHadWda=%d",
        bitmap.size(), targetHasWda);

    if (!bitmap.empty()) {
        webView_->SendScreenshot(bitmap);
    }

    lastShotTick_ = GetTickCount64();
    shotInProgress_ = false;
}

void AppController::ExtractUIText() {
    HWND target = GetForegroundWindow();
    if (target == overlay_->GetHwnd()) return;

    std::wstring text = textExtract_->ExtractFromWindow(target);
    if (!text.empty()) {
        webView_->SendExtractedText(text);
    }
}

void AppController::ExtractAndSendAI() {
    HWND target = GetForegroundWindow();
    if (target == overlay_->GetHwnd()) return;

    std::wstring text = textExtract_->ExtractFromWindow(target);
    if (!text.empty()) {
        webView_->SendExtractedText(text);
        webView_->TriggerSubmit();
    }
}

void AppController::TypeLastAnswer() {
    if (lastAiAnswer_.empty()) return;

    HWND target = GetForegroundWindow();
    if (target == overlay_->GetHwnd()) return;

    SetForegroundWindow(target);
    Sleep(100);
    typingSim_->TypeText(lastAiAnswer_);
}

void AppController::AutoTypeSelection() {
    if (!webView_ || !webView_->IsReady()) return;

    HWND target = webView_->GetPrevForeground();
    HWND fg = GetForegroundWindow();
    if (fg != overlay_->GetHwnd() && fg != nullptr) target = fg;

    webView_->GetSelectedText([this, target](const std::wstring& text) {
        if (text.empty()) return;
        if (target && IsWindow(target)) {
            SetForegroundWindow(target);
            Sleep(120);
        }
        typingSim_->TypeText(text);
    });
}

void AppController::ToggleVoiceCopilot() {
    webView_->ExecuteScript(L"toggleVoiceCopilot();");
}

bool AppController::InitHookSystem() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    hookDllPath_ = (exeDir / L"dxgi_assist.dll").wstring();

    if (!fs::exists(hookDllPath_)) {
        DbgLog("[HOOK] dxgi_assist.dll NOT FOUND next to exe -- WDA bypass disabled");
        hookDllPath_.clear();
        return false;
    }
    DbgLog("[HOOK] dxgi_assist.dll found, initializing hook system");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    sa.lpSecurityDescriptor = &sd;

    hSharedMem_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
        PAGE_READWRITE, 0, sizeof(SharedHookData), kSharedMemName);
    if (!hSharedMem_) {
        DbgLog("[HOOK] CreateFileMapping FAILED GLE=%lu", GetLastError());
        return false;
    }

    pShared_ = static_cast<SharedHookData*>(
        MapViewOfFile(hSharedMem_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedHookData)));
    if (!pShared_) {
        CloseHandle(hSharedMem_);
        hSharedMem_ = nullptr;
        return false;
    }

    ZeroMemory(pShared_, sizeof(SharedHookData));
    pShared_->ownerPid = GetCurrentProcessId();
    pShared_->overlayHwnd = overlay_->GetHwnd();

    {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        DWORD seed = GetCurrentProcessId() ^ static_cast<DWORD>(counter.LowPart);
        wsprintfW(pShared_->sessionSuffix, L"%08lX", seed ^ GetTickCount());
    }

    wchar_t scrubName[128]{};
    wsprintfW(scrubName, kScrubEventFmt, pShared_->sessionSuffix);
    hScrubEvent_ = CreateEventW(&sa, FALSE, FALSE, scrubName);

    DbgLog("[HOOK] Hook system ready: sharedMem=%p overlayHwnd=%p scrubEvent=%p suffix=%ls",
        pShared_, pShared_->overlayHwnd, hScrubEvent_, pShared_->sessionSuffix);
    return true;
}

void AppController::CleanupHookSystem() {
    if (hScrubEvent_) { CloseHandle(hScrubEvent_); hScrubEvent_ = nullptr; }
    if (pShared_) { UnmapViewOfFile(pShared_); pShared_ = nullptr; }
    if (hSharedMem_) { CloseHandle(hSharedMem_); hSharedMem_ = nullptr; }
    EnterCriticalSection(&injectLock_);
    injectedPids_.clear();
    LeaveCriticalSection(&injectLock_);
}

bool AppController::ShouldSkipProcess(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return true;

    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(pid, &sessionId) || sessionId == 0) return true;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return true;

    BOOL isWow64 = FALSE;
    IsWow64Process(hProc, &isWow64);
    if (isWow64) {
        CloseHandle(hProc);
        return true;
    }

    wchar_t procPath[MAX_PATH]{};
    DWORD pathLen = MAX_PATH;
    QueryFullProcessImageNameW(hProc, 0, procPath, &pathLen);
    CloseHandle(hProc);

    // System path filter (matches original FUN_140041380).
    {
        wchar_t lowerPath[MAX_PATH]{};
        wcscpy_s(lowerPath, procPath);
        _wcslwr_s(lowerPath);

        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        _wcslwr_s(sysDir);
        wcscat_s(sysDir, L"\\");
        if (wcsncmp(lowerPath, sysDir, wcslen(sysDir)) == 0) return true;

        wchar_t winDir[MAX_PATH]{};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        _wcslwr_s(winDir);

        const wchar_t* subDirs[] = {
            L"\\syswow64\\", L"\\winsxs\\", L"\\servicing\\", L"\\systemapps\\"
        };
        for (auto sub : subDirs) {
            wchar_t check[MAX_PATH]{};
            wcscpy_s(check, winDir);
            wcscat_s(check, sub);
            if (wcsncmp(lowerPath, check, wcslen(check)) == 0) return true;
        }
    }

    wchar_t* fileName = wcsrchr(procPath, L'\\');
    if (fileName) fileName++; else fileName = procPath;

    for (auto& ignored : kIgnoredProcesses) {
        if (_wcsicmp(fileName, ignored) == 0) return true;
    }
    for (auto& self : kSelfProcessNames) {
        if (_wcsicmp(fileName, self) == 0) return true;
    }
    for (auto& browser : kBrowserProcesses) {
        if (_wcsicmp(fileName, browser) == 0) return true;
    }
    for (auto& sec : kSecurityProcesses) {
        if (_wcsicmp(fileName, sec) == 0) return true;
    }
    return false;
}

static bool IsDllLoadedInProcess(DWORD pid, const std::wstring& dllName) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return false;

    HMODULE mods[512]{};
    DWORD needed = 0;
    if (K32EnumProcessModulesEx(hProc, mods, sizeof(mods), &needed, LIST_MODULES_64BIT)) {
        DWORD count = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < count; i++) {
            wchar_t modName[MAX_PATH]{};
            if (K32GetModuleBaseNameW(hProc, mods[i], modName, MAX_PATH)) {
                if (_wcsicmp(modName, dllName.c_str()) == 0) {
                    CloseHandle(hProc);
                    return true;
                }
                // Also detect AppInit-loaded copies (dxgi_assist_active_*.dll).
                if (_wcsnicmp(modName, L"dxgi_assist_active_", 19) == 0) {
                    CloseHandle(hProc);
                    return true;
                }
            }
        }
    }
    CloseHandle(hProc);
    return false;
}

bool AppController::InjectIntoPid(DWORD pid) {
    EnterCriticalSection(&injectLock_);
    bool alreadyDone = injectedPids_.count(pid) > 0;
    LeaveCriticalSection(&injectLock_);
    if (alreadyDone) return true;

    if (ShouldSkipProcess(pid)) {
        DbgLog("[INJECT] PID %lu: skipped (system/self/browser/wow64/session0)", pid);
        return false;
    }

    if (IsDllLoadedInProcess(pid, L"dxgi_assist.dll")) {
        EnterCriticalSection(&injectLock_);
        injectedPids_.insert(pid);
        LeaveCriticalSection(&injectLock_);
        DbgLog("[INJECT] PID %lu: DLL already loaded (module check)", pid);
        return true;
    }

    if (hookDllPath_.empty()) return false;

    constexpr DWORD kInjAccess = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION;

    bool elevatedPriv = false;
    bool success = false;
    HANDLE hProc = nullptr;
    void* remoteMem = nullptr;
    HANDLE hThread = nullptr;

    hProc = OpenProcess(kInjAccess, FALSE, pid);
    if (!hProc && GetLastError() == ERROR_ACCESS_DENIED) {
        HANDLE hToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(),
                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
                AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
                elevatedPriv = (GetLastError() == ERROR_SUCCESS);
            }
            CloseHandle(hToken);
        }
        hProc = OpenProcess(kInjAccess, FALSE, pid);
    }
    if (!hProc) {
        DbgLog("[INJECT] PID %lu: OpenProcess FAILED GLE=%lu", pid, GetLastError());
        goto cleanup;
    }

    {
        size_t pathBytes = (hookDllPath_.size() + 1) * sizeof(wchar_t);
        remoteMem = VirtualAllocEx(hProc, nullptr, pathBytes,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteMem) {
            DbgLog("[INJECT] PID %lu: VirtualAllocEx FAILED GLE=%lu", pid, GetLastError());
            goto cleanup;
        }

        if (!WriteProcessMemory(hProc, remoteMem, hookDllPath_.c_str(), pathBytes, nullptr)) {
            DbgLog("[INJECT] PID %lu: WriteProcessMemory FAILED GLE=%lu", pid, GetLastError());
            goto cleanup;
        }

        auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (!loadLibAddr) {
            DbgLog("[INJECT] GetProcAddress(LoadLibraryW) FAILED");
            goto cleanup;
        }

        hThread = CreateRemoteThread(hProc, nullptr, 0,
            loadLibAddr, remoteMem, 0, nullptr);
        if (!hThread) {
            DbgLog("[INJECT] PID %lu: CreateRemoteThread FAILED GLE=%lu", pid, GetLastError());
            goto cleanup;
        }

        WaitForSingleObject(hThread, 5000);

        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);

        DbgLog("[INJECT] PID %lu: LoadLibraryW exitCode=0x%08lX (%s)",
            pid, exitCode, exitCode ? "SUCCESS" : "DLL LOAD FAILED");

        if (exitCode != 0) {
            EnterCriticalSection(&injectLock_);
            injectedPids_.insert(pid);
            LeaveCriticalSection(&injectLock_);
            success = true;

            if (pShared_) {
                wchar_t eventName[128]{};
                wsprintfW(eventName, kHookActiveEventFmt, pid, pShared_->sessionSuffix);
                HANDLE hReady = OpenEventW(SYNCHRONIZE, FALSE, eventName);
                if (hReady) {
                    WaitForSingleObject(hReady, 3000);
                    CloseHandle(hReady);
                    DbgLog("[INJECT] Hook ready event signaled");
                } else {
                    Sleep(200);
                    DbgLog("[INJECT] Hook ready event not found, waited 200ms fallback");
                }
            }
        }
    }

cleanup:
    if (hThread)    CloseHandle(hThread);
    if (remoteMem && hProc) VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    if (hProc)      CloseHandle(hProc);

    if (elevatedPriv) {
        HANDLE hTok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hTok)) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = 0;
            LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid);
            AdjustTokenPrivileges(hTok, FALSE, &tp, 0, nullptr, nullptr);
            CloseHandle(hTok);
        }
    }
    return success;
}

// ── Phase 3: Reactive foreground-change injection ───────────────────────

void CALLBACK AppController::InjectionWinEventProc(
    HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!s_instance_ || idObject != OBJID_WINDOW) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    s_instance_->TryReactiveInject(pid);
}

DWORD WINAPI AppController::InjectionWorker(LPVOID param) {
    DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(param));
    if (s_instance_) s_instance_->InjectIntoPid(pid);
    return 0;
}

void AppController::TryReactiveInject(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return;
    if (hookDllPath_.empty()) return;
    if (reactiveInjectedPids_.count(pid)) return;
    reactiveInjectedPids_.insert(pid);
    CreateThread(nullptr, 0, InjectionWorker,
        reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(pid)), 0, nullptr);
}

void AppController::StartReactiveInjection() {
    s_instance_ = this;
    injectionHook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, InjectionWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT);
}

void AppController::StopReactiveInjection() {
    if (injectionHook_) {
        UnhookWinEvent(injectionHook_);
        injectionHook_ = nullptr;
    }
    s_instance_ = nullptr;
}

static BOOL CALLBACK SweepWindowProc(HWND hwnd, LPARAM lParam) {
    auto* self = reinterpret_cast<AppController*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    self->TryReactiveInject(pid);
    return TRUE;
}

void AppController::SweepExistingProcesses() {
    if (hookDllPath_.empty()) return;
    EnumWindows(SweepWindowProc, reinterpret_cast<LPARAM>(this));
}
