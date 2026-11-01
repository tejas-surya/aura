#pragma once
#include <Windows.h>
#include <memory>
#include <string>
#include <set>

struct SharedHookData;

class ConfigManager;
class StateManager;
class OverlayWindow;
class ViewportManager;
class WebViewManager;
class HotkeyManager;
class TypingSimulator;
class ScreenCapture;
class TextExtractor;

class AppController {
public:
    explicit AppController(HINSTANCE hInstance);
    ~AppController();

    bool Initialize(int nCmdShow);
    void Run();
    void Shutdown();

    void OnHotkeyAction(int actionId);
    void OnWebMessage(const std::wstring& message);

    HINSTANCE GetInstance() const { return hInstance_; }
    ConfigManager& GetConfig() { return *config_; }
    OverlayWindow& GetOverlay() { return *overlay_; }
    bool HasWebView() const { return webView_ != nullptr; }
    WebViewManager& GetWebView() { return *webView_; }

    void ToggleVisibility();
    void PanicHide();
    void ToggleTextOnlyMode();
    void ToggleMouseOnlyMode();
    void ToggleClickThrough();
    void AdjustOpacity(int delta);
    void MoveWindow(int dx, int dy);
    void ScrollPage(int direction);
    void AdjustTypingSpeed(int delta);
    void AdjustZoom(double delta);
    void RotateWebsite();
    void CaptureAndSend();
    void ExtractUIText();
    void ExtractAndSendAI();
    void TypeLastAnswer();
    void AutoTypeSelection();
    void ToggleVoiceCopilot();

private:
    bool InitHookSystem();
    void CleanupHookSystem();
    bool InjectIntoPid(DWORD pid);
    bool ShouldSkipProcess(DWORD pid);

    HINSTANCE hInstance_;

    // Hook injection system (matches original's app_hook.dll mechanism)
    HANDLE hSharedMem_ = nullptr;
    SharedHookData* pShared_ = nullptr;
    HANDLE hScrubEvent_ = nullptr;
    std::set<DWORD> injectedPids_;
    std::wstring hookDllPath_;

    std::unique_ptr<ConfigManager> config_;
    std::unique_ptr<StateManager> state_;
    std::unique_ptr<OverlayWindow> overlay_;
    std::unique_ptr<ViewportManager> viewport_;
    std::unique_ptr<WebViewManager> webView_;
    std::unique_ptr<HotkeyManager> hotkeys_;
    std::unique_ptr<TypingSimulator> typingSim_;
    std::unique_ptr<ScreenCapture> screenCap_;
    std::unique_ptr<TextExtractor> textExtract_;

    bool visible_ = true;
    bool panicHidden_ = false;
    bool textOnlyMode_ = false;
    bool mouseOnlyMode_ = false;
    bool clickThrough_ = false;
    int currentOpacity_ = 100;
    std::wstring lastAiAnswer_;

    // Screenshot debounce state (mirrors the original CaptureAndInjectScreenshot).
    unsigned long long lastShotTick_ = 0;   // GetTickCount64() of the last capture
    bool shotInProgress_ = false;           // guard against re-entrant requests
    static constexpr unsigned long long kShotDebounceMs = 1500; // 0x5dc in the original
};
