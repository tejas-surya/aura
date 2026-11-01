#include "WebViewManager.h"
#include "../app/AppController.h"
#include "../app/ConfigManager.h"
#include "../automation/ScreenCapture.h"
#include <WebView2EnvironmentOptions.h>
#include <ShlObj.h>
#include <wincrypt.h>
#include <filesystem>
#include <sstream>
#include <cstdio>

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
    fs::path dataDir = fs::path(appData) / L"Microsoft" / L"WebToolkit" / L"Default";
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

    // CRITICAL (matches original MYOWNAPP.exe): disable Chromium's native-window
    // occlusion detection and background throttling via additional browser args.
    // By default Chromium marks the WebView's renderer "occluded" when another
    // window covers it (e.g. a topmost / full-screen locked-down exam app). Once
    // occluded, the renderer pauses painting AND silently drops injected input,
    // so the user cannot type into the overlay even though its caret blinks.
    // Disabling CalculateNativeWinOcclusion keeps the renderer live regardless of
    // what sits on top. (The original additionally passes --no-sandbox /
    // --in-process-gpu / --disable-win32k-lockdown; those weaken the browser
    // sandbox and are not needed for the input fix, so they are intentionally
    // omitted. Add them here only if exact parity is required.)
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(
        L"--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,"
        L"msWebView2BrowserHitTransparent");

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), options.Get(),
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

    // Keep the omnibox in sync with the page URL (links, redirects, tab switches).
    webView_->add_SourceChanged(
        Callback<ICoreWebView2SourceChangedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                RefreshCurrentUrl();
                return S_OK;
            }).Get(), &token);

    webViewController_->add_GotFocus(
        Callback<ICoreWebView2FocusChangedEventHandler>(
            [this](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                return S_OK;
            }).Get(), &token);

    // Auto-grant clipboard-read so the page's own paste features and our
    // Ctrl+V JS injection can use navigator.clipboard when available.
    webView_->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PERMISSION_KIND kind =
                    COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                args->get_PermissionKind(&kind);
                if (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ) {
                    args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                }
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
    RefreshCurrentUrl();
}

void WebViewManager::RefreshCurrentUrl() {
    if (!webView_) return;
    wchar_t* src = nullptr;
    if (SUCCEEDED(webView_->get_Source(&src)) && src) {
        currentUrl_ = src;
        CoTaskMemFree(src);
    }
    InvalidateNavBar();
}

void WebViewManager::InvalidateNavBar() {
    if (parentHwnd_) {
        RECT bar{0, 0, 32767, kToolbarHeight};
        InvalidateRect(parentHwnd_, &bar, FALSE);
    }
}

void WebViewManager::GoBack() {
    if (webView_) webView_->GoBack();
}

void WebViewManager::GoForward() {
    if (webView_) webView_->GoForward();
}

void WebViewManager::Reload() {
    if (webView_) webView_->Reload();
}

void WebViewManager::SetZoomDelta(double step) {
    double f = zoomFactor_ + step;
    if (f < 0.3) f = 0.3;
    if (f > 3.0) f = 3.0;
    SetZoom(f);
}

void WebViewManager::NavigateOmnibox(const std::wstring& input) {
    std::wstring s = input;
    // trim
    size_t a = s.find_first_not_of(L" \t\r\n");
    size_t b = s.find_last_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return;
    s = s.substr(a, b - a + 1);
    if (s.empty()) return;

    bool looksLikeUrl = false;
    if (s.find(L"://") != std::wstring::npos) {
        looksLikeUrl = true;
    } else if (s.find(L' ') == std::wstring::npos) {
        // No spaces + has a dot (and a TLD-ish tail) → treat as a domain.
        size_t dot = s.find(L'.');
        if (dot != std::wstring::npos && dot > 0 && dot + 1 < s.size()) looksLikeUrl = true;
        if (s == L"localhost" || s.rfind(L"localhost:", 0) == 0) looksLikeUrl = true;
    }

    std::wstring url;
    if (looksLikeUrl) {
        url = (s.find(L"://") == std::wstring::npos) ? (L"https://" + s) : s;
    } else {
        // Google search with URL-encoded query.
        std::wstring q;
        for (wchar_t c : s) {
            if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
                (c >= L'a' && c <= L'z') || c == L'-' || c == L'_' || c == L'.' || c == L'~') {
                q += c;
            } else if (c == L' ') {
                q += L'+';
            } else if (c < 128) {
                wchar_t buf[4];
                swprintf_s(buf, L"%%%02X", static_cast<int>(c));
                q += buf;
            } else {
                // UTF-8 percent-encode non-ASCII
                char utf8[8]{};
                int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, sizeof(utf8), nullptr, nullptr);
                for (int i = 0; i < n; ++i) {
                    wchar_t buf[4];
                    swprintf_s(buf, L"%%%02X", static_cast<unsigned char>(utf8[i]));
                    q += buf;
                }
            }
        }
        url = L"https://www.google.com/search?q=" + q;
    }
    NavigateToUrl(url);
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
    if (parentHwnd_) {
        RECT bar{0, 0, 32767, kToolbarHeight};
        InvalidateRect(parentHwnd_, &bar, FALSE);
    }
}

void WebViewManager::RotateToPrevSite() {
    int n = static_cast<int>(siteRotation_.size());
    currentSiteIndex_ = (currentSiteIndex_ - 1 + n) % n;

    auto customUrl = controller_.GetConfig().GetWString("customWebsiteUrl", L"");
    if (!customUrl.empty() && currentSiteIndex_ == 0) {
        siteRotation_[0] = customUrl;
    }

    NavigateToSite(siteRotation_[currentSiteIndex_]);
    if (parentHwnd_) {
        RECT bar{0, 0, 32767, kToolbarHeight};
        InvalidateRect(parentHwnd_, &bar, FALSE);
    }
}

void WebViewManager::SwitchToSite(int index) {
    int n = static_cast<int>(siteRotation_.size());
    if (index < 0 || index >= n) return;
    currentSiteIndex_ = index;
    NavigateToSite(siteRotation_[currentSiteIndex_]);
    if (parentHwnd_) {
        RECT bar{0, 0, 32767, kToolbarHeight};
        InvalidateRect(parentHwnd_, &bar, FALSE);
    }
}

std::wstring WebViewManager::GetSiteLabel(int index) const {
    if (index < 0 || index >= static_cast<int>(siteRotation_.size())) return L"";
    const std::wstring& key = siteRotation_[index];
    if (key == L"chatgpt") return L"ChatGPT";
    if (key == L"claude")  return L"Claude";
    if (key == L"gemini")  return L"Gemini";
    if (key == L"google")  return L"Google";
    if (key == L"copilot") return L"Copilot";
    // Custom URL: show a short host-ish label.
    std::wstring s = key;
    size_t p = s.find(L"://");
    if (p != std::wstring::npos) s = s.substr(p + 3);
    if (s.size() > 10) s = s.substr(0, 10);
    return s;
}

void WebViewManager::FocusWebView() {
    if (webViewController_) {
        webViewController_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
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

    // Matches original MYOWNAPP.exe: inject printable text by running JS in the
    // page rather than via CDP Input.insertText. CDP input is dropped by the
    // browser when the WebView is not the focus owner / is occluded; running JS
    // directly locates the composer, focuses it at the DOM level, and inserts the
    // text with execCommand('insertText') + an InputEvent so React/ProseMirror
    // composers (ChatGPT, Claude, etc.) register it. This is focus- and
    // occlusion-independent, so it works under a locked-down foreground app.
    std::wstring js =
        L"(function(){var txt=\"" + JsonEscape(text) + L"\";"
        L"var el=document.activeElement;"
        L"var ok=el&&(el.tagName==='TEXTAREA'||el.tagName==='INPUT'||el.isContentEditable);"
        L"if(!ok){var l=document.querySelectorAll('#prompt-textarea,textarea,"
        L"[contenteditable=\\\"true\\\"],.ProseMirror,[role=\\\"textbox\\\"],"
        L"input:not([type=\\\"hidden\\\"]):not([type=\\\"submit\\\"]):not([type=\\\"button\\\"])"
        L":not([type=\\\"checkbox\\\"]):not([type=\\\"radio\\\"])');"
        L"el=l.length?l[l.length-1]:null;}"
        L"if(!el)return;try{el.focus();}catch(e){}"
        L"var done=false;try{done=document.execCommand('insertText',false,txt);}catch(e){}"
        L"if(!done){if(!el.isContentEditable&&typeof el.value!=='undefined'){"
        L"var s=el.selectionStart,e2=el.selectionEnd;"
        L"if(typeof s!=='number'){s=e2=el.value.length;}"
        L"el.value=el.value.slice(0,s)+txt+el.value.slice(e2);"
        L"try{el.selectionStart=el.selectionEnd=s+txt.length;}catch(e){}}}"
        L"try{el.dispatchEvent(new InputEvent('input',{bubbles:true,"
        L"inputType:'insertText',data:txt}));}catch(e){"
        L"el.dispatchEvent(new Event('input',{bubbles:true}));}})();";
    webView_->ExecuteScript(js.c_str(), nullptr);
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

// Decodes an ExecuteScript result that is a JSON-quoted base64(UTF-8) string
// (or "null") into a wide string. Base64 needs no JSON unescaping.
static std::wstring DecodeB64ScriptResult(HRESULT hr, LPCWSTR resultJson) {
    std::wstring text;
    if (FAILED(hr) || !resultJson) return text;
    std::wstring json(resultJson);
    if (json.size() < 2 || json.front() != L'"' || json.back() != L'"') return text;
    std::wstring b64 = json.substr(1, json.size() - 2);
    DWORD nbytes = 0;
    if (!CryptStringToBinaryW(b64.c_str(), static_cast<DWORD>(b64.size()),
            CRYPT_STRING_BASE64, nullptr, &nbytes, nullptr, nullptr) || !nbytes) return text;
    std::vector<uint8_t> utf8(nbytes);
    if (!CryptStringToBinaryW(b64.c_str(), static_cast<DWORD>(b64.size()),
            CRYPT_STRING_BASE64, utf8.data(), &nbytes, nullptr, nullptr)) return text;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(utf8.data()),
                                   static_cast<int>(nbytes), nullptr, 0);
    if (wlen > 0) {
        text.resize(wlen);
        MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(utf8.data()),
                            static_cast<int>(nbytes), text.data(), wlen);
    }
    return text;
}

static bool OpenClipboardRetry(HWND owner) {
    // Another process may briefly hold the clipboard; retry for ~200 ms.
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(10);
    }
    return false;
}

bool WebViewManager::SetClipboardTextNative(HWND owner, const std::wstring& textIn) {
    // Windows apps expect CRLF line endings on CF_UNICODETEXT.
    std::wstring text;
    text.reserve(textIn.size() + 16);
    for (size_t i = 0; i < textIn.size(); ++i) {
        wchar_t c = textIn[i];
        if (c == L'\n' && (i == 0 || textIn[i - 1] != L'\r')) text += L'\r';
        text += c;
    }
    if (!OpenClipboardRetry(owner)) return false;
    bool ok = false;
    EmptyClipboard();
    SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            if (SetClipboardData(CF_UNICODETEXT, h)) ok = true;
        }
        if (!ok) GlobalFree(h);  // ownership passes to the system only on success
    }
    CloseClipboard();
    return ok;
}

void WebViewManager::PasteTextIntoPage(const std::wstring& text) {
    if (!webView_ || text.empty()) return;
    // Plain inputs/textareas: setRangeText at the caret (same as the original's
    // insert path). Rich editors (ChatGPT/Claude ProseMirror, Gemini Quill):
    // dispatch a synthetic `paste` ClipboardEvent carrying a DataTransfer — the
    // same technique the original uses for screenshot paste — so the site's own
    // paste handler runs; if nobody handles it, fall back to insertText. All of
    // this is focus-independent DOM work, so the user's real app keeps focus.
    std::wstring js =
        L"(function(){var txt=\"" + JsonEscape(text) + LR"JS(";
        var el = document.activeElement;
        var ok = el && (el.tagName === 'TEXTAREA' || el.tagName === 'INPUT' || el.isContentEditable);
        if (!ok) {
            var l = document.querySelectorAll('#prompt-textarea,textarea,[contenteditable="true"],.ProseMirror,[role="textbox"],input:not([type="hidden"]):not([type="submit"]):not([type="button"]):not([type="checkbox"]):not([type="radio"])');
            el = l.length ? l[l.length - 1] : null;
        }
        if (!el) return;
        try { el.focus(); } catch(e) {}
        if (el.tagName === 'TEXTAREA' || el.tagName === 'INPUT') {
            var s = null, e2 = null;
            try { s = el.selectionStart; e2 = el.selectionEnd; } catch(e) {}
            if (typeof s !== 'number') { s = e2 = (el.value || '').length; }
            if (el.setRangeText) { el.setRangeText(txt, s, e2, 'end'); }
            else { var v = el.value || ''; el.value = v.slice(0, s) + txt + v.slice(e2); }
            try { el.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: 'insertFromPaste', data: txt })); }
            catch(e) { el.dispatchEvent(new Event('input', { bubbles: true })); }
            el.dispatchEvent(new Event('change', { bubbles: true }));
            return;
        }
        var handled = false;
        try {
            var dt = new DataTransfer();
            dt.setData('text/plain', txt);
            var ev = new ClipboardEvent('paste', { clipboardData: dt, bubbles: true, cancelable: true });
            el.dispatchEvent(ev);
            handled = ev.defaultPrevented;
        } catch(e) {}
        if (!handled) {
            var done = false;
            try { done = document.execCommand('insertText', false, txt); } catch(e) {}
            if (!done) {
                try { el.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: 'insertFromPaste', data: txt })); } catch(e) {}
            }
        }
    })();)JS";
    webView_->ExecuteScript(js.c_str(), nullptr);
}

void WebViewManager::ClipboardCommand(wchar_t op) {
    if (!webView_) return;

    // Matches original MYOWNAPP.exe keyboard hook (0x140018580):
    //  • Copy/Cut/SelectAll: post the native Ctrl+key sequence to the Chrome
    //    child HWND via PostMessage. Chromium's own accelerator table fires and
    //    it reads the DOM selection / writes the OS clipboard internally.
    //  • Paste: the original intercepts Ctrl+V and runs a JS snippet via
    //    ExecuteScript (because Chromium's paste needs a user gesture). We read
    //    the clipboard with the Win32 API and inject text via JS — same effect,
    //    no focus needed.
    switch (op) {
        case L'A':
        case L'C':
        case L'X':
            PostClipboardKeys(static_cast<DWORD>(op));
            return;

        case L'V': {
            std::wstring text;
            std::vector<uint8_t> png;
            if (OpenClipboardRetry(parentHwnd_)) {
                if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                    HANDLE h = GetClipboardData(CF_UNICODETEXT);
                    if (h) {
                        auto* p = static_cast<const wchar_t*>(GlobalLock(h));
                        if (p) { text = p; GlobalUnlock(h); }
                    }
                } else if (IsClipboardFormatAvailable(CF_BITMAP)) {
                    HBITMAP hb = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
                    if (hb) {
                        BITMAP bm{};
                        GetObjectW(hb, sizeof(bm), &bm);
                        ScreenCapture sc;
                        png = sc.HBitmapToPng(hb, bm.bmWidth, bm.bmHeight);
                    }
                }
                CloseClipboard();
            }
            if (!text.empty()) {
                std::wstring norm;
                norm.reserve(text.size());
                for (size_t i = 0; i < text.size(); ++i) {
                    if (text[i] == L'\r') {
                        if (i + 1 < text.size() && text[i + 1] == L'\n') continue;
                        norm += L'\n';
                    } else {
                        norm += text[i];
                    }
                }
                PasteTextIntoPage(norm);
            } else if (!png.empty()) {
                SendScreenshot(png);
            }
            return;
        }

        default:
            return;
    }
}

void WebViewManager::TrackForegroundWindow() {
    HWND fg = GetForegroundWindow();
    if (fg && fg != parentHwnd_ && IsWindow(fg)) {
        prevForeground_ = fg;
    }
}

static BOOL CALLBACK FindChromeWidgetCallback(HWND hwnd, LPARAM lParam) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0) {
        *reinterpret_cast<HWND*>(lParam) = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND WebViewManager::FindChromeChildHwnd() {
    if (!parentHwnd_) return nullptr;
    HWND found = nullptr;
    EnumChildWindows(parentHwnd_, FindChromeWidgetCallback,
                     reinterpret_cast<LPARAM>(&found));
    return found;
}

void WebViewManager::PostClipboardKeys(DWORD vk) {
    if (!chromeChildHwnd_ || !IsWindow(chromeChildHwnd_))
        chromeChildHwnd_ = FindChromeChildHwnd();
    if (!chromeChildHwnd_) return;

    DWORD scanCtrl = MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC);
    DWORD scanKey  = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);

    LPARAM ctrlDn = 1 | (static_cast<LPARAM>(scanCtrl) << 16);
    LPARAM keyDn  = 1 | (static_cast<LPARAM>(scanKey) << 16);
    LPARAM keyUp  = 1 | (static_cast<LPARAM>(scanKey) << 16)
                      | (1L << 30) | (1L << 31);
    LPARAM ctrlUp = 1 | (static_cast<LPARAM>(scanCtrl) << 16)
                      | (1L << 30) | (1L << 31);

    PostMessageW(chromeChildHwnd_, WM_KEYDOWN, VK_CONTROL, ctrlDn);
    PostMessageW(chromeChildHwnd_, WM_KEYDOWN, vk,         keyDn);
    PostMessageW(chromeChildHwnd_, WM_KEYUP,   vk,         keyUp);
    PostMessageW(chromeChildHwnd_, WM_KEYUP,   VK_CONTROL, ctrlUp);
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
    if (pngData.empty() || !webView_) return;

    // Base64-encode the raw PNG bytes (no line breaks).
    std::wstring base64;
    DWORD base64Len = 0;
    CryptBinaryToStringW(pngData.data(), static_cast<DWORD>(pngData.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &base64Len);
    if (base64Len == 0) return;
    base64.resize(base64Len);
    CryptBinaryToStringW(pngData.data(), static_cast<DWORD>(pngData.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64.data(), &base64Len);
    // CryptBinaryToStringW null-terminates; trim the terminator from the std::wstring.
    if (!base64.empty() && base64.back() == L'\0') base64.pop_back();

    // Faithful reconstruction of the original "Direct Paste" injection: the PNG is
    // wrapped in a synthetic `paste` ClipboardEvent (carrying a File via DataTransfer)
    // and dispatched at the active editor/textbox of the loaded AI site, with a
    // file-input fallback. Recovered from MYOWNAPP.exe CaptureAndInjectScreenshot.
    static const wchar_t* kPrefix = LR"JS(
            (function() {
                window.__wv_cap = 1;
                setTimeout(function(){ window.__wv_cap = 0; }, 4000);

                var b64Data = ')JS";

    static const wchar_t* kSuffix = LR"JS(';
                var binStr = atob(b64Data);
                var len = binStr.length;
                var arr = new Uint8Array(len);
                for (var i = 0; i < len; i++) arr[i] = binStr.charCodeAt(i);
                var blob = new Blob([arr], { type: 'image/png' });

                var fileName = "screenshot_" + Date.now() + "_" + Math.floor(Math.random() * 1000) + ".png";
                var file = new File([blob], fileName, { type: "image/png" });
                var dt = new DataTransfer();
                dt.items.add(file);

                var target = document.activeElement;
                if (!target || (target.tagName !== 'TEXTAREA' && target.tagName !== 'INPUT' && !target.isContentEditable)) {
                    var editors = document.querySelectorAll('div[contenteditable="true"], #prompt-textarea, div.ProseMirror, [role="textbox"], textarea.gLFyf, input[name="q"], textarea, input[type="text"]');
                    target = editors.length > 0 ? editors[editors.length - 1] : document.body;
                }
                if (target && target.focus) { target.focus(); target.click(); }

                var fileInputs = document.querySelectorAll('input[type="file"]');
                var pasteEvent = new ClipboardEvent('paste', {
                    clipboardData: dt,
                    bubbles: true,
                    cancelable: true
                });

                var dispatchTarget = target || document.activeElement || document.body || document.documentElement;
                var pasteHandled = false;
                if (dispatchTarget) {
                    try {
                        var res = dispatchTarget.dispatchEvent(pasteEvent);
                        if (pasteEvent.defaultPrevented || res === false) {
                            pasteHandled = true;
                        }
                    } catch(e) {}
                }
                if (!pasteHandled && (!target || target === document.body) && fileInputs.length > 0) {
                    for (var i = 0; i < fileInputs.length; i++) {
                        try {
                            fileInputs[i].files = dt.files;
                            fileInputs[i].dispatchEvent(new Event('change', { bubbles: true }));
                            break;
                        } catch(e) {}
                    }
                }
            })();
        )JS";

    std::wstring script;
    script.reserve(base64.size() + 3072);
    script.append(kPrefix).append(base64).append(kSuffix);
    ExecuteScript(script);
}

void WebViewManager::GetSelectedText(std::function<void(const std::wstring&)> onText) {
    if (!webView_) { onText(L""); return; }

    // Exact capture JS recovered from the original MYOWNAPP.exe: take the live
    // selection (fallback to the last <pre> block), strip comments, collapse blank
    // lines, and return it base64(UTF-8)-encoded (or null when empty).
    static const wchar_t* kSelectionJs = LR"JS((function(){
        var t = (window.getSelection && window.getSelection().toString()) || "";
        if (!t || !t.trim()) { var p = document.querySelectorAll('pre'); if (p.length) t = p[p.length-1].innerText; }
        if (!t || !t.trim()) return null;
        t = t.replace(/\/\*[\s\S]*?\*\/|\/\/.*/g, "").replace(/\n\s*\n\s*\n/g, "\n\n").trim();
        if (!t) return null;
        return btoa(unescape(encodeURIComponent(t)));
    })())JS";

    auto cb = std::move(onText);
    webView_->ExecuteScript(kSelectionJs,
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [cb](HRESULT hr, LPCWSTR resultJson) -> HRESULT {
                std::wstring text = DecodeB64ScriptResult(hr, resultJson);
                cb(text);
                return S_OK;
            }).Get());
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
