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

    LoadHookDll();

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
    UnloadHookDll();
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
        case 5:  TypeLastAnswer(); break;
        case 6:  AdjustOpacity(-5); break;
        case 7:  AdjustOpacity(5); break;
        case 9:  ToggleClickThrough(); break;
        case 10: MoveWindow(30, 0); break;
        case 11: MoveWindow(-30, 0); break;
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

void AppController::RotateWebsite() {
    webView_->RotateToNextSite();
}

void AppController::CaptureAndSend() {
    HWND target = GetForegroundWindow();
    if (target == overlay_->GetHwnd()) return;

    auto bitmap = screenCap_->CaptureWindow(target);
    if (!bitmap.empty()) {
        webView_->SendScreenshot(bitmap);
    }
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

void AppController::ToggleVoiceCopilot() {
    webView_->ExecuteScript(L"toggleVoiceCopilot();");
}

bool AppController::LoadHookDll() {
    wchar_t dllPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, dllPath, MAX_PATH);

    fs::path exeDir = fs::path(dllPath).parent_path();
    fs::path hookPath = exeDir / L"app_hook.dll";

    if (!fs::exists(hookPath)) return false;

    hookDll_ = LoadLibraryW(hookPath.c_str());
    if (!hookDll_) return false;

    using MH_InitFn = int(*)();
    auto mhInit = reinterpret_cast<MH_InitFn>(GetProcAddress(hookDll_, "MH_Initialize"));
    if (mhInit) mhInit();

    UpdateSharedMemory();
    return true;
}

void AppController::UnloadHookDll() {
    if (hookDll_) {
        using MH_UninitFn = int(*)();
        auto mhUninit = reinterpret_cast<MH_UninitFn>(GetProcAddress(hookDll_, "MH_Uninitialize"));
        if (mhUninit) mhUninit();

        FreeLibrary(hookDll_);
        hookDll_ = nullptr;
    }
}

void AppController::UpdateSharedMemory() {
    wchar_t eventName[128]{};
    wsprintfW(eventName, L"Local\\UFOlee_Hook_Active_%lu", GetCurrentProcessId());
    HANDLE hEvent = CreateEventW(nullptr, TRUE, TRUE, eventName);
    if (hEvent) CloseHandle(hEvent);
}
