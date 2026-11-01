#include "AppController.h"
#include "ConfigManager.h"
#include "StateManager.h"
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

namespace fs = std::filesystem;

AppController::AppController(HINSTANCE hInstance)
    : hInstance_(hInstance) {}

AppController::~AppController() {
    Shutdown();
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
        case 10: webView_->RotateToNextSite(); break;  // Ctrl+Shift+Right: next AI tab
        case 11: webView_->RotateToPrevSite(); break;  // Ctrl+Shift+Left: previous AI tab
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
        case 26: AdjustZoom(0.1); break;   // Ctrl+Shift++  zoom in
        case 27: AdjustZoom(-0.1); break;  // Ctrl+Shift+-  zoom out
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
    // Only zoom when the user is interacting with our overlay ("our app's area").
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

        char dbg[256];
        wsprintfA(dbg, "[CAPTURE] fg=%p pid=%lu wdaWindows=%lu hookDll=%d",
            fgWnd, targetPid, pShared_->trackedWindowCount, !hookDllPath_.empty());
        OutputDebugStringA(dbg);

        if (targetHasWda) {
            bool injected = InjectIntoPid(targetPid);
            wsprintfA(dbg, "[CAPTURE] InjectIntoPid(%lu) => %d", targetPid, injected);
            OutputDebugStringA(dbg);
        }
    }

    overlay_->SuspendCaptureProtection();
    Sleep(50);

    if (targetHasWda && hScrubEvent_ && pShared_) {
        SetEvent(hScrubEvent_);
        int polls = 0;
        for (int i = 0; i < 100; i++) {
            if (pShared_->wdaCleared) break;
            Sleep(5);
            polls++;
        }
        char dbg[128];
        wsprintfA(dbg, "[CAPTURE] wdaCleared=%d after %d polls", pShared_->wdaCleared, polls);
        OutputDebugStringA(dbg);
    }

    auto bitmap = screenCap_->CaptureVirtualScreen();

    if (targetHasWda && pShared_) {
        pShared_->wdaCleared = FALSE;
    }

    overlay_->RestoreCaptureProtection();

    char dbg[128];
    wsprintfA(dbg, "[CAPTURE] bitmap=%zu bytes, targetHadWda=%d",
        bitmap.size(), targetHasWda);
    OutputDebugStringA(dbg);

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
    // Feature #4: highlight text/code inside the UFOlee webview, press the hotkey,
    // and it is hardware-typed (SendInput) into whatever editor is focused beneath
    // the overlay. Mirrors the original: grab the webview selection, then type it.
    if (!webView_ || !webView_->IsReady()) return;

    // The overlay is now activatable, so when the user selects text inside the
    // webview the overlay is foreground. The editor to type into is the last
    // external window that held focus before the overlay.
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
    hookDllPath_ = (exeDir / L"app_hook.dll").wstring();

    if (!fs::exists(hookDllPath_)) {
        OutputDebugStringA("[HOOK] app_hook.dll NOT FOUND next to exe — WDA bypass disabled");
        hookDllPath_.clear();
        return false;
    }
    OutputDebugStringA("[HOOK] app_hook.dll found, initializing hook system");

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
        char dbg[128];
        wsprintfA(dbg, "[HOOK] CreateFileMapping FAILED GLE=%lu", GetLastError());
        OutputDebugStringA(dbg);
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

    hScrubEvent_ = CreateEventW(&sa, FALSE, FALSE, kDwmCompositionNotify);

    char dbg[256];
    wsprintfA(dbg, "[HOOK] Hook system ready: sharedMem=%p overlayHwnd=%p scrubEvent=%p",
        pShared_, pShared_->overlayHwnd, hScrubEvent_);
    OutputDebugStringA(dbg);
    return true;
}

void AppController::CleanupHookSystem() {
    if (hScrubEvent_) { CloseHandle(hScrubEvent_); hScrubEvent_ = nullptr; }
    if (pShared_) { UnmapViewOfFile(pShared_); pShared_ = nullptr; }
    if (hSharedMem_) { CloseHandle(hSharedMem_); hSharedMem_ = nullptr; }
    injectedPids_.clear();
}

bool AppController::ShouldSkipProcess(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return true;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return true;

    wchar_t procPath[MAX_PATH]{};
    DWORD pathLen = MAX_PATH;
    QueryFullProcessImageNameW(hProc, 0, procPath, &pathLen);
    CloseHandle(hProc);

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
    return false;
}

bool AppController::InjectIntoPid(DWORD pid) {
    if (injectedPids_.count(pid)) return true;
    if (ShouldSkipProcess(pid)) {
        char dbg[128];
        wsprintfA(dbg, "[INJECT] PID %lu: skipped (system/self/browser)", pid);
        OutputDebugStringA(dbg);
        return false;
    }

    // Try enabling SeDebugPrivilege for access to elevated processes.
    {
        HANDLE hToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken)) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid);
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
            CloseHandle(hToken);
        }
    }

    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProc) {
        char dbg[128];
        wsprintfA(dbg, "[INJECT] PID %lu: OpenProcess FAILED GLE=%lu", pid, GetLastError());
        OutputDebugStringA(dbg);
        return false;
    }

    size_t pathBytes = (hookDllPath_.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProc, nullptr, pathBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        char dbg[128];
        wsprintfA(dbg, "[INJECT] PID %lu: VirtualAllocEx FAILED GLE=%lu", pid, GetLastError());
        OutputDebugStringA(dbg);
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteMem, hookDllPath_.c_str(), pathBytes, nullptr)) {
        char dbg[128];
        wsprintfA(dbg, "[INJECT] PID %lu: WriteProcessMemory FAILED GLE=%lu", pid, GetLastError());
        OutputDebugStringA(dbg);
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLibAddr) {
        OutputDebugStringA("[INJECT] GetProcAddress(LoadLibraryW) FAILED");
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        loadLibAddr, remoteMem, 0, nullptr);
    if (!hThread) {
        char dbg[128];
        wsprintfA(dbg, "[INJECT] PID %lu: CreateRemoteThread FAILED GLE=%lu", pid, GetLastError());
        OutputDebugStringA(dbg);
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, 5000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    char dbg[128];
    wsprintfA(dbg, "[INJECT] PID %lu: LoadLibraryW exitCode=0x%08lX (%s)",
        pid, exitCode, exitCode ? "SUCCESS" : "DLL LOAD FAILED");
    OutputDebugStringA(dbg);

    if (exitCode != 0) {
        injectedPids_.insert(pid);

        wchar_t eventName[128]{};
        wsprintfW(eventName, kHookActiveEventFmt, pid);
        HANDLE hReady = OpenEventW(SYNCHRONIZE, FALSE, eventName);
        if (hReady) {
            WaitForSingleObject(hReady, 3000);
            CloseHandle(hReady);
            OutputDebugStringA("[INJECT] Hook ready event signaled");
        } else {
            Sleep(200);
            OutputDebugStringA("[INJECT] Hook ready event not found, waited 200ms fallback");
        }
        return true;
    }
    return false;
}
