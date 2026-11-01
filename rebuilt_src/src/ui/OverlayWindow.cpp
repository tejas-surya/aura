#include "OverlayWindow.h"
#include "../app/AppController.h"
#include "../browser/WebViewManager.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <string>

#pragma comment(lib, "dwmapi.lib")

OverlayWindow* OverlayWindow::s_instance_ = nullptr;
HHOOK OverlayWindow::s_keyboardHook_ = nullptr;
HHOOK OverlayWindow::s_mouseHook_ = nullptr;
bool OverlayWindow::s_keyTargetWebView_ = false;
bool OverlayWindow::s_urlEditing_ = false;

OverlayWindow::OverlayWindow(AppController& controller)
    : controller_(controller) {
    s_instance_ = this;
}

OverlayWindow::~OverlayWindow() {
    if (s_instance_ == this) s_instance_ = nullptr;
    Destroy();
}

bool OverlayWindow::Create(int width, int height, int opacity) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = controller_.GetInstance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(controller_.GetInstance(), MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;

    RegisterClassExW(&wc);

    // Matches the original's CreateWindowExA exStyle = 0x8080088:
    //   WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE
    // WS_EX_NOACTIVATE: overlay never becomes foreground (dual-focus invariant).
    // WS_EX_LAYERED from creation: forces DWM to composite all child content
    // (including WebView2) as one layered surface, so SetWindowDisplayAffinity
    // covers everything. Without this at creation time, WebView2's
    // DirectComposition surface bypasses the parent's WDA.
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE;
    DWORD style = WS_POPUP | WS_CLIPCHILDREN;

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int x = workArea.right - width - 20;
    int y = workArea.top + 50;

    hwnd_ = CreateWindowExW(
        exStyle, kClassName, L"MYOWNAPP",
        style,
        x, y, width, height,
        nullptr, nullptr, controller_.GetInstance(), this);

    if (!hwnd_) return false;

    // Window is WS_EX_LAYERED from creation — always set alpha, even at 100%.
    currentOpacity_ = opacity;
    BYTE alpha = (opacity < 100)
        ? static_cast<BYTE>(opacity * 255 / 100)
        : 255;
    SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);

    // Matches original: SetWindowDisplayAffinity → ShowWindow (capture protection
    // applied before the window is ever shown).
    ApplyCaptureProtection();

    winEventHook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_REORDER,
        nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    s_keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
        GetModuleHandleW(nullptr), 0);
    s_mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
        GetModuleHandleW(nullptr), 0);

    SetTimer(hwnd_, kTopmostTimer, 1000, nullptr);

    return true;
}

void OverlayWindow::Show(int nCmdShow) {
    ShowWindow(hwnd_, nCmdShow);
    ApplyCaptureProtection();  // re-assert capture exclusion on every show
    ReassertTopmost();
}

void OverlayWindow::Hide() {
    ShowWindow(hwnd_, SW_HIDE);
}

void OverlayWindow::Destroy() {
    if (s_keyboardHook_) {
        UnhookWindowsHookEx(s_keyboardHook_);
        s_keyboardHook_ = nullptr;
    }
    if (s_mouseHook_) {
        UnhookWindowsHookEx(s_mouseHook_);
        s_mouseHook_ = nullptr;
    }
    if (winEventHook_) {
        UnhookWinEvent(winEventHook_);
        winEventHook_ = nullptr;
    }
    if (hwnd_) {
        KillTimer(hwnd_, kTopmostTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(kClassName, controller_.GetInstance());
}

void OverlayWindow::SetOpacity(int percent) {
    currentOpacity_ = std::clamp(percent, 0, 100);
    // WS_EX_LAYERED is permanent (required for WDA_EXCLUDEFROMCAPTURE to cover
    // WebView2 child surfaces). Just update the alpha value.
    BYTE alpha = (currentOpacity_ < 100)
        ? static_cast<BYTE>(currentOpacity_ * 255 / 100)
        : 255;
    SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
}

void OverlayWindow::SetClickThrough(bool enable) {
    clickThrough_ = enable;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enable) {
        exStyle |= WS_EX_TRANSPARENT;
    } else {
        exStyle &= ~WS_EX_TRANSPARENT;
    }
    // WS_EX_LAYERED is permanent; WS_EX_NOACTIVATE must always be re-asserted.
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_LAYERED | WS_EX_NOACTIVATE);
}

void OverlayWindow::MoveTo(int x, int y) {
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void OverlayWindow::Resize(int width, int height) {
    SetWindowPos(hwnd_, nullptr, 0, 0, width, height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void OverlayWindow::RemoveFromTaskbar() {
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW;
    exStyle &= ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
}

void OverlayWindow::RestoreToTaskbar() {
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    exStyle &= ~WS_EX_TOOLWINDOW;
    exStyle &= ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
}

void OverlayWindow::EnableMouseReveal(bool enable) {
    mouseReveal_ = enable;
    if (enable) {
        savedOpacity_ = currentOpacity_;
        SetOpacity(0);
        SetTimer(hwnd_, kMouseRevealTimer, 50, nullptr);
    } else {
        KillTimer(hwnd_, kMouseRevealTimer);
        SetOpacity(savedOpacity_);
    }
}

void OverlayWindow::PaintToolbar(HDC hdc, const RECT& clientRect) {
    RECT toolbarRect = {0, 0, clientRect.right, kToolbarHeight};
    HBRUSH bgBrush = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(hdc, &toolbarRect, bgBrush);
    DeleteObject(bgBrush);

    // Divider lines under each row.
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, borderPen));
    MoveToEx(hdc, 0, kTabRowHeight - 1, nullptr);   LineTo(hdc, clientRect.right, kTabRowHeight - 1);
    MoveToEx(hdc, 0, kToolbarHeight - 1, nullptr);  LineTo(hdc, clientRect.right, kToolbarHeight - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    SetBkMode(hdc, TRANSPARENT);
    HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

    // --- Row 1: drag grip + tabs + minimize/close ---
    RECT grip = GripRect(clientRect);
    SetTextColor(hdc, RGB(120, 120, 120));
    DrawTextW(hdc, L"\x283F", -1, &grip,  // braille-dots grip glyph
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    int active = controller_.HasWebView()
        ? controller_.GetWebView().GetCurrentSiteIndex() : -1;
    int n = TabCount();
    for (int i = 0; i < n; ++i) {
        RECT r = TabRect(i, clientRect);
        if (i == active) {
            HBRUSH hl = CreateSolidBrush(RGB(52, 52, 58));
            FillRect(hdc, &r, hl);
            DeleteObject(hl);
            HPEN accent = CreatePen(PS_SOLID, 2, RGB(90, 150, 245));
            HPEN op = static_cast<HPEN>(SelectObject(hdc, accent));
            MoveToEx(hdc, r.left + 3, kTabRowHeight - 3, nullptr);
            LineTo(hdc, r.right - 3, kTabRowHeight - 3);
            SelectObject(hdc, op);
            DeleteObject(accent);
            SetTextColor(hdc, RGB(235, 235, 235));
        } else {
            SetTextColor(hdc, RGB(150, 150, 150));
        }
        std::wstring label = controller_.HasWebView()
            ? controller_.GetWebView().GetSiteLabel(i) : L"";
        DrawTextW(hdc, label.c_str(), -1, &r,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    const wchar_t* rightLabels[] = {L"\x2015", L"\x2715"};  // minimize, close
    int rightX = clientRect.right - 6;
    for (int i = 1; i >= 0; i--) {
        RECT btnRect = {rightX - 24, 2, rightX, kTabRowHeight - 4};
        SetTextColor(hdc, i == 1 ? RGB(220, 80, 80) : RGB(180, 180, 180));
        DrawTextW(hdc, rightLabels[i], -1, &btnRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        rightX -= 28;
    }

    // --- Row 2: browser nav bar ---
    PaintNavBar(hdc, clientRect);

    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
}

void OverlayWindow::PaintNavBar(HDC hdc, const RECT& clientRect) {
    // Back / Forward / Refresh icons.
    struct { RECT r; const wchar_t* glyph; } btns[] = {
        { BackRect(clientRect),    L"\x2039" },  // ‹
        { FwdRect(clientRect),     L"\x203A" },  // ›
        { RefreshRect(clientRect), L"\x21BB" },  // ⟳
    };
    SetTextColor(hdc, RGB(200, 200, 200));
    for (auto& b : btns) {
        DrawTextW(hdc, b.glyph, -1, &b.r,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    }

    // Omnibox box.
    RECT omni = OmniRect(clientRect);
    HBRUSH field = CreateSolidBrush(s_urlEditing_ ? RGB(48, 48, 54) : RGB(40, 40, 44));
    FillRect(hdc, &omni, field);
    DeleteObject(field);
    HPEN edge = CreatePen(PS_SOLID, 1, s_urlEditing_ ? RGB(90, 150, 245) : RGB(70, 70, 76));
    HPEN op = static_cast<HPEN>(SelectObject(hdc, edge));
    HBRUSH nb = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HBRUSH ob = static_cast<HBRUSH>(SelectObject(hdc, nb));
    Rectangle(hdc, omni.left, omni.top, omni.right, omni.bottom);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(edge);

    std::wstring shown = s_urlEditing_
        ? urlText_
        : (controller_.HasWebView() ? controller_.GetWebView().GetCurrentUrl() : L"");
    bool placeholder = shown.empty();
    if (placeholder && !s_urlEditing_) shown = L"Search Google or type a URL";
    if (s_urlEditing_ && caretOn_) shown += L"\x2502";  // caret bar

    RECT textR = omni;
    textR.left += 8; textR.right -= 6;
    SetTextColor(hdc, placeholder && !s_urlEditing_ ? RGB(120, 120, 120) : RGB(230, 230, 230));
    DrawTextW(hdc, shown.c_str(), -1, &textR,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT OverlayWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            // Never activate on click: the underlying app keeps foreground/focus.
            return MA_NOACTIVATE;

        case WM_ACTIVATE:
        case WM_SETFOCUS:
            // Backstop: if we somehow receive focus, release it immediately so
            // the real app keeps the caret. WebView input comes via CDP, not focus.
            if (controller_.HasWebView()) {
                controller_.GetWebView().NotifyPositionChanged();
            }
            SetFocus(nullptr);
            return 0;

        case WM_SHOWWINDOW:
        case WM_DISPLAYCHANGE:
            if (controller_.HasWebView()) {
                controller_.GetWebView().NotifyPositionChanged();
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);

        case WM_APP_FORWARD_KEY: {
            DWORD vkCode   = static_cast<DWORD>(wParam);
            DWORD scanCode = static_cast<DWORD>(lParam & 0xFF);
            DWORD flags    = static_cast<DWORD>((lParam >> 8) & 0xFF);
            bool  keyDown  = ((lParam >> 16) & 0x1) != 0;
            ForwardKeyToWebView(vkCode, scanCode, flags, keyDown);
            return 0;
        }

        case WM_APP_URL_KEY: {
            DWORD vkCode   = static_cast<DWORD>(wParam);
            DWORD scanCode = static_cast<DWORD>(lParam & 0xFF);
            OnUrlKey(vkCode, scanCode);
            return 0;
        }

        case WM_APP_CLIP_CMD: {
            // Ctrl+C/V/X/A forwarded from the keyboard hook; run the clipboard op
            // inside the WebView (matches the original's execCommand approach).
            if (controller_.HasWebView()) {
                controller_.GetWebView().ClipboardCommand(static_cast<wchar_t>(wParam));
            }
            return 0;
        }

        case WM_WINDOWPOSCHANGING: {
            auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
            wp->flags |= SWP_NOACTIVATE;
            break;
        }

        case WM_HOTKEY:
            OnHotkey(wParam);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd_, &ps);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            PaintToolbar(hdc, rc);

            RECT bodyRect = {0, kToolbarHeight, rc.right, rc.bottom};
            HBRUSH bodyBrush = CreateSolidBrush(RGB(30, 30, 30));
            FillRect(hdc, &bodyRect, bodyBrush);
            DeleteObject(bodyBrush);

            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE: {
            if (controller_.HasWebView()) {
                controller_.GetWebView().OnParentResized();
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd_, &pt);
            RECT rc{};
            GetClientRect(hwnd_, &rc);

            if (!clickThrough_ && pt.y < kToolbarHeight) {
                if (pt.y < kTabRowHeight) {
                    int rightBtnLeft = rc.right - 6 - 28 * 2;
                    if (pt.x >= rightBtnLeft && pt.x < rc.right - 6) return HTCLIENT; // min/close
                    if (TabHitTest(pt, rc) >= 0) return HTCLIENT;                     // tabs
                    return HTCAPTION;  // grip / empty row-1 area → drag the window
                }
                return HTCLIENT;  // row 2 (nav bar) is all interactive
            }
            return HTCLIENT;
        }

        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rc{};
            GetClientRect(hwnd_, &rc);

            if (pt.y < kTabRowHeight) {
                // Row 1: minimize / close / tabs
                int rightBtnLeft = rc.right - 6 - 28 * 2;
                if (pt.x >= rightBtnLeft && pt.x < rc.right - 6) {
                    int btnIdx = (rc.right - 6 - pt.x) / 28;  // 0=close, 1=minimize
                    if (btnIdx == 0) {
                        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    } else if (btnIdx == 1) {
                        ShowWindow(hwnd_, SW_MINIMIZE);
                    }
                    return 0;
                }
                int tab = TabHitTest(pt, rc);
                if (tab >= 0 && controller_.HasWebView()) {
                    controller_.GetWebView().SwitchToSite(tab);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            } else if (pt.y < kToolbarHeight) {
                // Row 2: back / forward / refresh / omnibox
                RECT rb = BackRect(rc), rf = FwdRect(rc), rr = RefreshRect(rc), ro = OmniRect(rc);
                if (PtInRect(&rb, pt)) { if (controller_.HasWebView()) controller_.GetWebView().GoBack();    return 0; }
                if (PtInRect(&rf, pt)) { if (controller_.HasWebView()) controller_.GetWebView().GoForward(); return 0; }
                if (PtInRect(&rr, pt)) { if (controller_.HasWebView()) controller_.GetWebView().Reload();    return 0; }
                if (PtInRect(&ro, pt)) { BeginUrlEdit(); return 0; }
            }
            break;
        }

        case WM_TIMER:
            OnTimer(static_cast<UINT_PTR>(wParam));
            return 0;

        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                controller_.ScrollPage(delta > 0 ? -1 : 1);
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void OverlayWindow::OnHotkey(WPARAM hotkeyId) {
    controller_.OnHotkeyAction(static_cast<int>(hotkeyId));
}

void OverlayWindow::OnMouseMove(int, int) {}

void OverlayWindow::ApplyCaptureProtection() {
    if (!hwnd_) return;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
}

void OverlayWindow::SuspendCaptureProtection() {
    if (!hwnd_) return;
    SetWindowDisplayAffinity(hwnd_, 0);
}

void OverlayWindow::RestoreCaptureProtection() {
    ApplyCaptureProtection();
}

void OverlayWindow::ReassertTopmost() {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    if (!IsWindowVisible(hwnd_)) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void CALLBACK OverlayWindow::WinEventProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (!s_instance_ || !s_instance_->hwnd_) return;
    if (hwnd == s_instance_->hwnd_) return;
    if (event == EVENT_SYSTEM_FOREGROUND && s_instance_->controller_.HasWebView()) {
        s_instance_->controller_.GetWebView().TrackForegroundWindow();
    }
    s_instance_->ReassertTopmost();
}

void OverlayWindow::OnTimer(UINT_PTR timerId) {
    if (timerId == kTopmostTimer) {
        ReassertTopmost();
        return;
    }
    if (timerId == kCaretTimer) {
        if (s_urlEditing_) {
            caretOn_ = !caretOn_;
            RECT rc{}; GetClientRect(hwnd_, &rc);
            RECT nav = {0, kTabRowHeight, rc.right, kToolbarHeight};
            InvalidateRect(hwnd_, &nav, FALSE);
        }
        return;
    }
    if (timerId == kMouseRevealTimer && mouseReveal_) {
        POINT pt{};
        GetCursorPos(&pt);
        RECT rc{};
        GetWindowRect(hwnd_, &rc);

        bool hovering = PtInRect(&rc, pt);
        int targetOpacity = hovering ? savedOpacity_ : 0;

        if (currentOpacity_ != targetOpacity) {
            int step = (targetOpacity > currentOpacity_) ? 10 : -10;
            int next = currentOpacity_ + step;
            if ((step > 0 && next > targetOpacity) || (step < 0 && next < targetOpacity))
                next = targetOpacity;

            BYTE alpha = static_cast<BYTE>(next * 255 / 100);
            SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
            currentOpacity_ = next;
        }
    }
}

int OverlayWindow::TabCount() const {
    if (!controller_.HasWebView()) return 0;
    return static_cast<int>(controller_.GetWebView().GetSites().size());
}

RECT OverlayWindow::TabRect(int index, const RECT& clientRect) const {
    // Row 1: after the drag grip on the left, before minimize+close on the right.
    const int rightReserve = 6 + 28 * 2;
    int areaLeft = 30;  // leave room for the grip
    int areaRight = clientRect.right - rightReserve;
    int n = TabCount();
    if (n <= 0 || areaRight <= areaLeft) return RECT{0, 0, 0, 0};
    int w = (areaRight - areaLeft) / n;
    RECT r = { areaLeft + index * w, 2,
               areaLeft + (index + 1) * w, kTabRowHeight - 2 };
    return r;
}

RECT OverlayWindow::GripRect(const RECT&) const {
    return RECT{ 4, 2, 26, kTabRowHeight - 2 };
}

RECT OverlayWindow::BackRect(const RECT&) const {
    return RECT{ 4, kTabRowHeight + 3, 32, kToolbarHeight - 3 };
}

RECT OverlayWindow::FwdRect(const RECT&) const {
    return RECT{ 34, kTabRowHeight + 3, 62, kToolbarHeight - 3 };
}

RECT OverlayWindow::RefreshRect(const RECT&) const {
    return RECT{ 64, kTabRowHeight + 3, 92, kToolbarHeight - 3 };
}

RECT OverlayWindow::OmniRect(const RECT& clientRect) const {
    return RECT{ 98, kTabRowHeight + 4, clientRect.right - 8, kToolbarHeight - 4 };
}

int OverlayWindow::TabHitTest(POINT pt, const RECT& clientRect) const {
    for (int i = 0; i < TabCount(); ++i) {
        RECT r = TabRect(i, clientRect);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

// "Last click decides" — a click inside the WebView body makes the keyboard
// hook route keystrokes to the WebView; a click anywhere else lets them flow to
// whatever app the user is in. This hook never eats mouse events.
LRESULT CALLBACK OverlayWindow::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance_ && s_instance_->hwnd_ &&
        (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN)) {

        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        RECT rc{};
        GetWindowRect(s_instance_->hwnd_, &rc);
        bool visible = IsWindowVisible(s_instance_->hwnd_);

        bool inWebView = visible &&
                         ms->pt.x >= rc.left && ms->pt.x <= rc.right &&
                         ms->pt.y >= rc.top + kToolbarHeight && ms->pt.y <= rc.bottom;
        s_keyTargetWebView_ = inWebView;

        // Cancel omnibox editing unless the click is on the omnibox itself.
        if (s_urlEditing_) {
            RECT client{}; GetClientRect(s_instance_->hwnd_, &client);
            RECT omni = s_instance_->OmniRect(client);
            POINT sp = { omni.left, omni.top }; ClientToScreen(s_instance_->hwnd_, &sp);
            POINT ep = { omni.right, omni.bottom }; ClientToScreen(s_instance_->hwnd_, &ep);
            bool onOmni = visible && ms->pt.x >= sp.x && ms->pt.x <= ep.x &&
                          ms->pt.y >= sp.y && ms->pt.y <= ep.y;
            if (!onOmni) {
                s_urlEditing_ = false;
                InvalidateRect(s_instance_->hwnd_, nullptr, FALSE);
            }
        }
    }
    return CallNextHookEx(s_mouseHook_, nCode, wParam, lParam);
}

static bool IsModifierVk(DWORD vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
        case VK_CAPITAL:
            return true;
        default:
            return false;
    }
}

// Routes keystrokes to the WebView (via CDP) when the last click was inside it,
// WITHOUT stealing Win32 focus from the user's real app.
//
// Two fixes over the earlier broken version:
//  1. Modifier keys (Shift/Ctrl/Alt/Win/CapsLock) are NEVER eaten — they pass
//     through so the OS keyboard state stays correct (ToUnicode then produces
//     capitals and shifted symbols) and so global hotkeys can still see them.
//  2. Any chord that holds Ctrl/Alt/Win passes through untouched, so registered
//     global hotkeys (Ctrl+Shift+H/D/A/...) fire instead of being swallowed.
LRESULT CALLBACK OverlayWindow::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance_ &&
        s_instance_->hwnd_ && IsWindowVisible(s_instance_->hwnd_)) {

        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool keyUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        bool ctrlAltWin =
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
            (GetAsyncKeyState(VK_MENU)    & 0x8000) ||
            (GetAsyncKeyState(VK_LWIN)    & 0x8000) ||
            (GetAsyncKeyState(VK_RWIN)    & 0x8000);

        bool capturable = (keyDown || keyUp) && !IsModifierVk(kb->vkCode) && !ctrlAltWin;

        // Ctrl+C / Ctrl+V / Ctrl+X / Ctrl+A routed INTO the WebView when it is the
        // active target. WebViewManager::ClipboardCommand reads/inserts the
        // selection via JS and moves the data through the OS clipboard with the
        // Win32 API (in-page execCommand/navigator.clipboard are rejected by
        // Chromium because the overlay's document never has focus).
        // Only a PLAIN Ctrl chord is eaten (never Ctrl+Shift/Alt/Win), so global
        // hotkeys (Ctrl+Shift+H/D/A, tab switch, zoom, …) still fire. When the
        // WebView is NOT the target the chord falls through untouched, so the
        // underlying desktop app's own Ctrl+C/V keeps working normally. The
        // shared OS clipboard makes copy/paste work across the two directions.
        {
            bool ctrlDown  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shiftDown = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool altDown   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
            bool winDown   = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
            bool ctrlOnly  = ctrlDown && !shiftDown && !altDown && !winDown;
            bool isClipVk  = (kb->vkCode == 'C' || kb->vkCode == 'V' ||
                              kb->vkCode == 'X' || kb->vkCode == 'A');
            if (s_keyTargetWebView_ && !s_urlEditing_ && ctrlOnly && isClipVk &&
                (keyDown || keyUp)) {
                if (keyDown) {
                    PostMessageW(s_instance_->hwnd_, WM_APP_CLIP_CMD,
                                 static_cast<WPARAM>(kb->vkCode), 0);
                }
                return 1;  // eat down AND up — the WebView handles it, not the app
            }
        }

        // Omnibox editing takes priority and routes keys to the URL field.
        if (s_urlEditing_ && capturable) {
            if (keyDown) {
                LPARAM packed = (kb->scanCode & 0xFF) | ((kb->flags & 0xFF) << 8);
                PostMessageW(s_instance_->hwnd_, WM_APP_URL_KEY,
                             static_cast<WPARAM>(kb->vkCode), packed);
            }
            return 1;  // eat (both down and up) so it never reaches other apps
        }

        // Otherwise, route to the WebView when the last click was inside it.
        if (s_keyTargetWebView_ && !s_urlEditing_ && capturable) {
            LPARAM packed = (kb->scanCode & 0xFF)
                          | ((kb->flags & 0xFF) << 8)
                          | ((keyDown ? 1 : 0) << 16);
            PostMessageW(s_instance_->hwnd_, WM_APP_FORWARD_KEY,
                         static_cast<WPARAM>(kb->vkCode), packed);
            return 1;  // eat: it goes to the WebView instead of the focused app
        }
        // Modifiers and Ctrl/Alt/Win chords fall through to the system.
    }
    return CallNextHookEx(s_keyboardHook_, nCode, wParam, lParam);
}

void OverlayWindow::BeginUrlEdit() {
    s_urlEditing_ = true;
    urlText_ = controller_.HasWebView() ? controller_.GetWebView().GetCurrentUrl() : L"";
    urlSelectAll_ = true;   // first keystroke replaces the whole field (browser-like)
    caretOn_ = true;
    SetTimer(hwnd_, kCaretTimer, 500, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void OverlayWindow::EndUrlEdit(bool commit) {
    if (!s_urlEditing_) return;
    s_urlEditing_ = false;
    KillTimer(hwnd_, kCaretTimer);
    if (commit && controller_.HasWebView() && !urlText_.empty()) {
        controller_.GetWebView().NavigateOmnibox(urlText_);
    }
    urlText_.clear();
    urlSelectAll_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void OverlayWindow::OnUrlKey(DWORD vkCode, DWORD scanCode) {
    if (!s_urlEditing_) return;
    if (vkCode == VK_RETURN) { EndUrlEdit(true);  return; }
    if (vkCode == VK_ESCAPE) { EndUrlEdit(false); return; }

    if (vkCode == VK_BACK) {
        if (urlSelectAll_) { urlText_.clear(); urlSelectAll_ = false; }
        else if (!urlText_.empty()) urlText_.pop_back();
    } else {
        BYTE ks[256]{};
        GetKeyboardState(ks);
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   ks[VK_SHIFT]    = 0x80;
        if (GetAsyncKeyState(VK_CAPITAL) & 1)      ks[VK_CAPITAL] |= 1;
        WCHAR ch[8]{};
        int nn = ToUnicode(vkCode, scanCode, ks, ch, 8, 0);
        if (nn > 0 && ch[0] >= 0x20 && ch[0] != 0x7F) {
            if (urlSelectAll_) { urlText_.clear(); urlSelectAll_ = false; }
            urlText_.append(ch, ch + nn);
        }
    }
    caretOn_ = true;
    RECT rc{}; GetClientRect(hwnd_, &rc);
    RECT nav = {0, kTabRowHeight, rc.right, kToolbarHeight};
    InvalidateRect(hwnd_, &nav, FALSE);
}

void OverlayWindow::ForwardKeyToWebView(DWORD vkCode, DWORD scanCode, DWORD flags, bool keyDown) {
    if (!controller_.HasWebView()) return;
    auto& wv = controller_.GetWebView();

    int modifiers = 0;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    modifiers |= 1; // Alt
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= 2; // Ctrl
    if (GetAsyncKeyState(VK_LWIN) & 0x8000 ||
        GetAsyncKeyState(VK_RWIN) & 0x8000)    modifiers |= 4; // Meta
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   modifiers |= 8; // Shift

    bool ctrlOrAlt = (modifiers & (1 | 2)) != 0;

    // Translate to a printable character, respecting the (now correct) Shift/Caps
    // state, for Input.insertText.
    BYTE ks[256]{};
    GetKeyboardState(ks);
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   ks[VK_SHIFT]    = 0x80;
    if (GetAsyncKeyState(VK_CAPITAL) & 1)      ks[VK_CAPITAL] |= 1;
    WCHAR chars[8]{};
    int n = ToUnicode(vkCode, scanCode, ks, chars, 8, 0);

    if (keyDown && n > 0 && !ctrlOrAlt && chars[0] >= 0x20 && chars[0] != 0x7F) {
        wv.InjectText(std::wstring(chars, chars + n));
        return;
    }

    std::wstring key, code;
    switch (vkCode) {
        case VK_RETURN:  key = L"Enter";      code = L"Enter";       break;
        case VK_BACK:    key = L"Backspace";  code = L"Backspace";   break;
        case VK_DELETE:  key = L"Delete";     code = L"Delete";      break;
        case VK_TAB:     key = L"Tab";        code = L"Tab";         break;
        case VK_ESCAPE:  key = L"Escape";     code = L"Escape";      break;
        case VK_LEFT:    key = L"ArrowLeft";  code = L"ArrowLeft";   break;
        case VK_RIGHT:   key = L"ArrowRight"; code = L"ArrowRight";  break;
        case VK_UP:      key = L"ArrowUp";    code = L"ArrowUp";     break;
        case VK_DOWN:    key = L"ArrowDown";  code = L"ArrowDown";   break;
        case VK_HOME:    key = L"Home";       code = L"Home";        break;
        case VK_END:     key = L"End";        code = L"End";         break;
        default:
            if (n > 0) { key.assign(1, chars[0]); code = L""; }
            else return;
    }
    wv.InjectKey(vkCode, key, code, keyDown, modifiers);
}
