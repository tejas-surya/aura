#include "WebViewManager.h"
#include "../app/AppController.h"
#include "../app/ConfigManager.h"
#include <ShlObj.h>
#include <wincrypt.h>
#include <filesystem>
#include <sstream>

#pragma comment(lib, "crypt32.lib")

namespace fs = std::filesystem;
using namespace Microsoft::WRL;

WebViewManager::WebViewManager(AppController& controller)
    : controller_(controller) {}

WebViewManager::~WebViewManager() {
    if (webViewController_) {
        webViewController_->Close();
    }
}

std::wstring WebViewManager::GetUserDataFolder() const {
    wchar_t appData[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
    fs::path dataDir = fs::path(appData) / L"UFOlee" / L"edge_profile_";
    fs::create_directories(dataDir);
    return dataDir.wstring();
}

RECT WebViewManager::GetWebViewBounds() const {
    RECT rc{};
    if (parentHwnd_) {
        GetClientRect(parentHwnd_, &rc);
        rc.top += kToolbarHeight;
    }
    return rc;
}

bool WebViewManager::Initialize(HWND parentHwnd) {
    parentHwnd_ = parentHwnd;

    std::wstring userDataFolder = GetUserDataFolder();

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                OnEnvironmentCreated(result, env);
                return S_OK;
            }).Get());

    return SUCCEEDED(hr);
}

void WebViewManager::OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env) {
    if (FAILED(result) || !env) return;
    environment_ = env;

    env->CreateCoreWebView2Controller(
        parentHwnd_,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                OnControllerCreated(result, controller);
                return S_OK;
            }).Get());
}

void WebViewManager::OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || !controller) return;

    webViewController_ = controller;
    controller->get_CoreWebView2(&webView_);

    RECT bounds = GetWebViewBounds();
    webViewController_->put_Bounds(bounds);
    webViewController_->put_IsVisible(TRUE);

    COREWEBVIEW2_COLOR bgColor = {255, 30, 30, 30};
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(webViewController_.As(&controller2))) {
        controller2->put_DefaultBackgroundColor(bgColor);
    }

    ICoreWebView2Settings* settings = nullptr;
    webView_->get_Settings(&settings);
    if (settings) {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->Release();
    }

    EventRegistrationToken token{};
    webView_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                OnWebMessageReceived(sender, args);
                return S_OK;
            }).Get(), &token);

    webView_->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                OnNavigationCompleted(sender, args);
                return S_OK;
            }).Get(), &token);

    webViewController_->add_GotFocus(
        Callback<ICoreWebView2FocusChangedEventHandler>(
            [this](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                return S_OK;
            }).Get(), &token);

    webView_->AddScriptToExecuteOnDocumentCreated(
        LR"(
        window.chrome.webview.addEventListener('message', function(e) {
            try { window.onNativeMessage && window.onNativeMessage(e.data); } catch(ex) {}
        });
        window.sendToNative = function(obj) {
            window.chrome.webview.postMessage(JSON.stringify(obj));
        };
        )", nullptr);

    if (!pendingUrl_.empty()) {
        webView_->Navigate(pendingUrl_.c_str());
        pendingUrl_.clear();
    } else {
        webView_->Navigate(L"https://chatgpt.com");
    }
}

void WebViewManager::OnWebMessageReceived(
    ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
    wchar_t* messageRaw = nullptr;
    args->TryGetWebMessageAsString(&messageRaw);
    if (messageRaw) {
        controller_.OnWebMessage(messageRaw);
        CoTaskMemFree(messageRaw);
    }
}

void WebViewManager::OnNavigationCompleted(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) {
    if (textOnlyActive_) {
        InjectTextOnlyMode(true);
    }
}

std::wstring WebViewManager::ResolveUrl(const std::wstring& siteKey) const {
    if (siteKey == L"chatgpt")  return L"https://chatgpt.com";
    if (siteKey == L"google")   return L"https://www.google.com";
    if (siteKey == L"claude")   return L"https://claude.ai";
    if (siteKey == L"gemini")   return L"https://gemini.google.com";
    if (siteKey == L"copilot")  return L"https://copilot.microsoft.com";

    if (siteKey.find(L"://") != std::wstring::npos) return siteKey;
    return L"https://" + siteKey;
}

void WebViewManager::NavigateToSite(const std::wstring& siteKey) {
    std::wstring url = ResolveUrl(siteKey);
    NavigateToUrl(url);
}

void WebViewManager::NavigateToUrl(const std::wstring& url) {
    if (webView_) {
        webView_->Navigate(url.c_str());
    } else {
        pendingUrl_ = url;
    }
}

void WebViewManager::RotateToNextSite() {
    currentSiteIndex_ = (currentSiteIndex_ + 1) % static_cast<int>(siteRotation_.size());

    auto customUrl = controller_.GetConfig().GetWString("customWebsiteUrl", L"");
    if (!customUrl.empty() && currentSiteIndex_ == 0) {
        siteRotation_[0] = customUrl;
    }

    NavigateToSite(siteRotation_[currentSiteIndex_]);
}

void WebViewManager::ExecuteScript(const std::wstring& script) {
    if (webView_) {
        webView_->ExecuteScript(script.c_str(), nullptr);
    }
}

void WebViewManager::OnParentResized() {
    if (webViewController_) {
        RECT bounds = GetWebViewBounds();
        webViewController_->put_Bounds(bounds);
    }
}

void WebViewManager::NotifyPositionChanged() {
    if (webViewController_) {
        webViewController_->NotifyParentWindowPositionChanged();
    }
}

static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            case L'\b': out += L"\\b";  break;
            case L'\f': out += L"\\f";  break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void WebViewManager::InjectText(const std::wstring& text) {
    if (!webView_) return;
    std::wstring params = L"{\"text\":\"" + JsonEscape(text) + L"\"}";
    webView_->CallDevToolsProtocolMethod(L"Input.insertText", params.c_str(), nullptr);
}

void WebViewManager::InjectKey(DWORD vkCode, const std::wstring& key,
                               const std::wstring& code, bool keyDown, int modifiers) {
    if (!webView_) return;
    std::wstring type = keyDown ? L"rawKeyDown" : L"keyUp";
    std::wstring params =
        L"{\"type\":\"" + type + L"\""
        L",\"windowsVirtualKeyCode\":" + std::to_wstring(vkCode) +
        L",\"nativeVirtualKeyCode\":" + std::to_wstring(vkCode) +
        L",\"modifiers\":" + std::to_wstring(modifiers) +
        L",\"key\":\"" + JsonEscape(key) + L"\""
        L",\"code\":\"" + JsonEscape(code) + L"\"}";
    webView_->CallDevToolsProtocolMethod(L"Input.dispatchKeyEvent", params.c_str(), nullptr);
}

void WebViewManager::TrackForegroundWindow() {
    HWND fg = GetForegroundWindow();
    if (fg && fg != parentHwnd_ && IsWindow(fg)) {
        prevForeground_ = fg;
    }
}

void WebViewManager::InjectTextOnlyMode(bool enable) {
    textOnlyActive_ = enable;
    if (enable) {
        ExecuteScript(LR"(
            (function() {
                let styleId = '__myownapp_textonly';
                let existing = document.getElementById(styleId);
                if (existing) return;
                let style = document.createElement('style');
                style.id = styleId;
                style.textContent = `
                    * {
                        background: transparent !important;
                        background-image: none !important;
                        box-shadow: none !important;
                        border-color: transparent !important;
                    }
                    img, svg, video, iframe, canvas, picture, figure,
                    [role="img"], .avatar, .logo, header nav, footer,
                    [class*="banner"], [class*="sidebar"], [class*="ad-"] {
                        display: none !important;
                    }
                    body {
                        background: transparent !important;
                        color: #ccc !important;
                        font-family: 'Segoe UI', sans-serif !important;
                        font-size: 14px !important;
                    }
                    a { color: #8af !important; }
                    input, textarea, [contenteditable] {
                        background: rgba(255,255,255,0.05) !important;
                        color: #eee !important;
                        border: 1px solid rgba(255,255,255,0.1) !important;
                    }
                `;
                document.head.appendChild(style);
            })();
        )");
    } else {
        ExecuteScript(LR"(
            (function() {
                let el = document.getElementById('__myownapp_textonly');
                if (el) el.remove();
            })();
        )");
    }
}

void WebViewManager::SendScreenshot(const std::vector<uint8_t>& pngData) {
    std::wstring base64;
    DWORD base64Len = 0;
    CryptBinaryToStringW(pngData.data(), static_cast<DWORD>(pngData.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &base64Len);
    base64.resize(base64Len);
    CryptBinaryToStringW(pngData.data(), static_cast<DWORD>(pngData.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64.data(), &base64Len);

    std::wstring script = L"window.onScreenshot && window.onScreenshot('data:image/png;base64,"
        + base64 + L"');";
    ExecuteScript(script);
}

void WebViewManager::SendExtractedText(const std::wstring& text) {
    std::wstring escaped;
    for (wchar_t c : text) {
        switch (c) {
            case L'\\': escaped += L"\\\\"; break;
            case L'\'': escaped += L"\\'"; break;
            case L'\n': escaped += L"\\n"; break;
            case L'\r': escaped += L"\\r"; break;
            case L'\t': escaped += L"\\t"; break;
            default: escaped += c; break;
        }
    }
    std::wstring script = L"window.onExtractedText && window.onExtractedText('"
        + escaped + L"');";
    ExecuteScript(script);
}

void WebViewManager::TriggerSubmit() {
    ExecuteScript(LR"(
        (function() {
            let btn = document.querySelector('[data-testid="send-button"]')
                || document.querySelector('button[aria-label*="Send"]')
                || document.querySelector('form button[type="submit"]');
            if (btn) btn.click();
        })();
    )");
}

void WebViewManager::ResizeToBounds(RECT bounds) {
    if (webViewController_) {
        webViewController_->put_Bounds(bounds);
    }
}

void WebViewManager::SetZoom(double factor) {
    zoomFactor_ = factor;
    if (webViewController_) {
        webViewController_->put_ZoomFactor(factor);
    }
}
