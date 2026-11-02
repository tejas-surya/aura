#pragma once

#include <Windows.h>
#include <unknwn.h>
#include <objbase.h>
#include <ocidl.h>
#include <EventToken.h>

#include <WebView2.h>

#include <wrl.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <string>
#include <vector>
#include <functional>

class AppController;

class WebViewManager {
public:
    // Two-row chrome: tab strip (30) + browser nav bar (32).
    static constexpr int kTabRowHeight = 30;
    static constexpr int kNavRowHeight = 32;
    static constexpr int kToolbarHeight = kTabRowHeight + kNavRowHeight;

    explicit WebViewManager(AppController& controller);
    ~WebViewManager();

    bool Initialize(HWND parentHwnd);
    void NavigateToSite(const std::wstring& siteKey);
    void NavigateToUrl(const std::wstring& url);

    // Browser navigation controls for the nav bar.
    void GoBack();
    void GoForward();
    void Reload();
    // Omnibox: treat input as a URL (navigate) or a search query (Google).
    void NavigateOmnibox(const std::wstring& input);
    std::wstring GetCurrentUrl() const { return currentUrl_; }
    void SetZoomDelta(double step);  // +/- zoom from current factor

    void RotateToNextSite();
    void RotateToPrevSite();
    void SwitchToSite(int index);
    const std::vector<std::wstring>& GetSites() const { return siteRotation_; }
    int GetCurrentSiteIndex() const { return currentSiteIndex_; }
    std::wstring GetSiteLabel(int index) const;

    // Give the hosted WebView2 keyboard focus (native typing, incl. Shift).
    void FocusWebView();
    // Last external (non-overlay) foreground window — the auto-type target.
    HWND GetPrevForeground() const { return prevForeground_; }

    void ExecuteScript(const std::wstring& script);
    // Asynchronously fetches the current page text selection (plain UTF-16),
    // mirroring the original "auto-type highlighted code" capture JS. The
    // callback runs on the UI thread with an empty string when nothing is selected.
    void GetSelectedText(std::function<void(const std::wstring&)> onText);
    void InjectTextOnlyMode(bool enable);
    void SendScreenshot(const std::vector<uint8_t>& pngData);
    void SendExtractedText(const std::wstring& text);
    void TriggerSubmit();

    void ResizeToBounds(RECT bounds);
    void OnParentResized();
    void NotifyPositionChanged();
    void TrackForegroundWindow();

    void InjectText(const std::wstring& text);
    void InjectKey(DWORD vkCode, const std::wstring& key, const std::wstring& code,
                   bool keyDown, int modifiers);
    // Clipboard operation for a Ctrl chord routed from the keyboard hook.
    // op: 'C' copy, 'V' paste, 'X' cut, 'A' select-all.
    // Copy/Cut/SelectAll: posts the native Ctrl+key sequence to the WebView's
    // Chrome child HWND via PostMessage — Chromium handles it internally
    // (matches the original MYOWNAPP.exe). Paste: reads the Win32 clipboard
    // and injects text via JS (the original also uses JS for paste because
    // Chromium's execCommand('paste') needs a user gesture).
    void ClipboardCommand(wchar_t op);

    void SetZoom(double factor);
    double GetZoom() const { return zoomFactor_; }
    bool IsReady() const { return webView_ != nullptr || dockedEdgeMode_; }

private:
    void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env);
    void OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller);
    void OnWebMessageReceived(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args);
    void OnNavigationCompleted(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args);
    void RefreshCurrentUrl();
    void InvalidateNavBar();
    void PasteTextIntoPage(const std::wstring& text);
    static bool SetClipboardTextNative(HWND owner, const std::wstring& text);
    HWND FindChromeChildHwnd();
    void PostClipboardKeys(DWORD vk);

    RECT GetWebViewBounds() const;
    std::wstring ResolveUrl(const std::wstring& siteKey) const;
    std::wstring GetUserDataFolder() const;
    std::wstring FindBrowserExecutableFolder() const;
    bool CreateWebViewEnvironment(const std::wstring& browserPath,
                                  const std::wstring& userDataFolder);

    bool LaunchAndDockEdge();
    std::wstring FindEdgePath() const;
    static BOOL CALLBACK EdgeEnumProc(HWND hwnd, LPARAM lParam);
    static DWORD WINAPI EdgeFinderThread(LPVOID param);

    AppController& controller_;
    HWND parentHwnd_ = nullptr;
    int initRetries_ = 0;
    bool dockedEdgeMode_ = false;
    HANDLE edgeProcess_ = nullptr;
    DWORD edgePid_ = 0;
    HWND edgeHwnd_ = nullptr;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> webViewController_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webView_;

    std::wstring pendingUrl_;

    // AI tabs cycled by Ctrl+Shift+Left/Right (feature #5): the sites actually used.
    std::vector<std::wstring> siteRotation_ = {
        L"chatgpt", L"claude", L"gemini", L"google"
    };
    int currentSiteIndex_ = 0;
    std::wstring currentUrl_;
    double zoomFactor_ = 1.0;
    bool textOnlyActive_ = false;
    HWND prevForeground_ = nullptr;
    HWND chromeChildHwnd_ = nullptr;
};
