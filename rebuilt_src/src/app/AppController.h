#pragma once
#include <Windows.h>
#include <memory>
#include <string>
#include <set>
#include <unordered_set>

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
class DesktopWatcher;

class AppController {
public:
    explicit AppController(HINSTANCE hInstance, bool isDesktopInstance = false);
    ~AppController();

    bool Initialize(int nCmdShow);
    void Run();
    void Shutdown();

    void OnHotkeyAction(int actionId);
    void OnWebMessage(const std::wstring& message);

    HINSTANCE GetInstance() const { return hInstance_; }
    bool IsDesktopInstance() const { return isDesktopInstance_; }
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

    void TryReactiveInject(DWORD pid);

private:
    bool InitHookSystem();
    void CleanupHookSystem();
    bool InjectIntoPid(DWORD pid);
    bool ShouldSkipProcess(DWORD pid);

    void StartReactiveInjection();
    void StopReactiveInjection();
    void SweepExistingProcesses();

    static void CALLBACK InjectionWinEventProc(HWINEVENTHOOK hHook, DWORD event,
        HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);
    static DWORD WINAPI InjectionWorker(LPVOID param);

    HINSTANCE hInstance_;
    bool isDesktopInstance_ = false;

    // Hook injection system
    HANDLE hSharedMem_ = nullptr;
    SharedHookData* pShared_ = nullptr;
    HANDLE hScrubEvent_ = nullptr;
    std::set<DWORD> injectedPids_;
    CRITICAL_SECTION injectLock_;
    std::wstring hookDllPath_;

    // Phase 2: Desktop watcher
    std::unique_ptr<DesktopWatcher> desktopWatcher_;

    // Phase 3: Reactive foreground-change injection
    HWINEVENTHOOK injectionHook_ = nullptr;
    std::unordered_set<DWORD> reactiveInjectedPids_;
    static AppController* s_instance_;

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

    unsigned long long lastShotTick_ = 0;
    bool shotInProgress_ = false;
    static constexpr unsigned long long kShotDebounceMs = 1500;
};
