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
    static constexpr int kToolbarHeight = 30;

    explicit WebViewManager(AppController& controller);
    ~WebViewManager();

    bool Initialize(HWND parentHwnd);
    void NavigateToSite(const std::wstring& siteKey);
    void NavigateToUrl(const std::wstring& url);
    void RotateToNextSite();

    void ExecuteScript(const std::wstring& script);
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

    void SetZoom(double factor);
    double GetZoom() const { return zoomFactor_; }
    bool IsReady() const { return webView_ != nullptr; }

private:
    void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env);
    void OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller);
    void OnWebMessageReceived(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args);
    void OnNavigationCompleted(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args);

    RECT GetWebViewBounds() const;
    std::wstring ResolveUrl(const std::wstring& siteKey) const;
    std::wstring GetUserDataFolder() const;

    AppController& controller_;
    HWND parentHwnd_ = nullptr;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> webViewController_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webView_;

    std::wstring pendingUrl_;

    std::vector<std::wstring> siteRotation_ = {
        L"chatgpt", L"google", L"claude", L"gemini", L"copilot"
    };
    int currentSiteIndex_ = 0;
    double zoomFactor_ = 1.0;
    bool textOnlyActive_ = false;
    HWND prevForeground_ = nullptr;
};
