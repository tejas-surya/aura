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

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
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

    currentOpacity_ = opacity;
    if (opacity < 100) {
        LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        BYTE alpha = static_cast<BYTE>(opacity * 255 / 100);
        SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
    }

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
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (currentOpacity_ < 100) {
        if (!(ex & WS_EX_LAYERED)) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, (ex | WS_EX_LAYERED) | WS_EX_NOACTIVATE);
        }
        BYTE alpha = static_cast<BYTE>(currentOpacity_ * 255 / 100);
        SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
    } else {
        if (ex & WS_EX_LAYERED) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, (ex & ~WS_EX_LAYERED) | WS_EX_NOACTIVATE);
            RedrawWindow(hwnd_, nullptr, nullptr,
                RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }
    }
}

void OverlayWindow::SetClickThrough(bool enable) {
    clickThrough_ = enable;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enable) {
        exStyle |= WS_EX_TRANSPARENT;
    } else {
        exStyle &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
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

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, borderPen));
    MoveToEx(hdc, 0, kToolbarHeight - 1, nullptr);
    LineTo(hdc, clientRect.right, kToolbarHeight - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(180, 180, 180));

    HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

    const wchar_t* leftLabels[] = {L"\x2630", L"\x26B2", L"\x2699"};
    int x = 6;
    for (int i = 0; i < 3; i++) {
        RECT btnRect = {x, 2, x + 28, kToolbarHeight - 4};
        DrawTextW(hdc, leftLabels[i], -1, &btnRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        x += 28;
    }

    const wchar_t* rightLabels[] = {L"\x2015", L"\x25A1", L"\x2715"};
    int rightX = clientRect.right - 6;
    for (int i = 2; i >= 0; i--) {
        RECT btnRect = {rightX - 24, 2, rightX, kToolbarHeight - 4};
        if (i == 2) SetTextColor(hdc, RGB(220, 80, 80));
        else SetTextColor(hdc, RGB(180, 180, 180));
        DrawTextW(hdc, rightLabels[i], -1, &btnRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        rightX -= 28;
    }

    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
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
            return MA_NOACTIVATE;

        case WM_ACTIVATE:
        case WM_SETFOCUS:
        case WM_SHOWWINDOW:
        case WM_DISPLAYCHANGE:
            if (controller_.HasWebView()) {
                controller_.GetWebView().NotifyPositionChanged();
            }
            SetFocus(nullptr);
            return DefWindowProcW(hwnd_, msg, wParam, lParam);

        case WM_APP_FORWARD_KEY: {
            DWORD vkCode   = static_cast<DWORD>(wParam);
            DWORD scanCode = static_cast<DWORD>(lParam & 0xFF);
            DWORD flags    = static_cast<DWORD>((lParam >> 8) & 0xFF);
            bool  keyDown  = ((lParam >> 16) & 0x1) != 0;
            ForwardKeyToWebView(vkCode, scanCode, flags, keyDown);
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
                int rightBtnX = rc.right - 6 - 28 * 3;
                if (pt.x > rightBtnX && pt.x < rc.right - 6) {
                    return HTCLIENT;
                }
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rc{};
            GetClientRect(hwnd_, &rc);

            if (pt.y < kToolbarHeight) {
                int rightBtnX = rc.right - 6 - 28 * 3;
                if (pt.x > rightBtnX && pt.x < rc.right - 6) {
                    int btnIdx = (rc.right - 6 - pt.x) / 28;
                    if (btnIdx == 0) {
                        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    } else if (btnIdx == 2) {
                        ShowWindow(hwnd_, SW_MINIMIZE);
                    }
                    return 0;
                }
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

LRESULT CALLBACK OverlayWindow::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance_ && s_instance_->hwnd_ &&
        (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN)) {

        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        RECT rc{};
        GetWindowRect(s_instance_->hwnd_, &rc);

        bool inWebView = IsWindowVisible(s_instance_->hwnd_) &&
                         ms->pt.x >= rc.left && ms->pt.x <= rc.right &&
                         ms->pt.y >= rc.top + kToolbarHeight && ms->pt.y <= rc.bottom;

        s_keyTargetWebView_ = inWebView;
    }
    return CallNextHookEx(s_mouseHook_, nCode, wParam, lParam);
}

LRESULT CALLBACK OverlayWindow::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_keyTargetWebView_ && s_instance_ &&
        s_instance_->hwnd_ && IsWindowVisible(s_instance_->hwnd_)) {

        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool keyUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        if (keyDown || keyUp) {
            LPARAM packed = (kb->scanCode & 0xFF)
                          | ((kb->flags & 0xFF) << 8)
                          | ((keyDown ? 1 : 0) << 16);
            PostMessageW(s_instance_->hwnd_, WM_APP_FORWARD_KEY,
                         static_cast<WPARAM>(kb->vkCode), packed);
            return 1;
        }
    }
    return CallNextHookEx(s_keyboardHook_, nCode, wParam, lParam);
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

    // Translate to a printable character (respecting shift/caps) for insertText.
    BYTE ks[256]{};
    GetKeyboardState(ks);
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   ks[VK_SHIFT]   = 0x80;
    if (GetAsyncKeyState(VK_CAPITAL) & 1)      ks[VK_CAPITAL] |= 1;
    WCHAR chars[8]{};
    int n = ToUnicode(vkCode, scanCode, ks, chars, 8, 0);

    if (keyDown && n > 0 && !ctrlOrAlt && chars[0] >= 0x20 && chars[0] != 0x7F) {
        // Printable text: insert directly into the focused element.
        wv.InjectText(std::wstring(chars, chars + n));
        return;
    }

    // Non-printable / control keys: dispatch a key event.
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
