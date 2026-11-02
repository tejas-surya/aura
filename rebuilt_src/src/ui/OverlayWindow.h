#pragma once
#include <Windows.h>
#include <dwmapi.h>
#include <functional>
#include <string>

class AppController;

class OverlayWindow {
public:
    explicit OverlayWindow(AppController& controller);
    ~OverlayWindow();

    bool Create(int width, int height, int opacity);
    void Show(int nCmdShow = SW_SHOWNOACTIVATE);
    void Hide();
    void Destroy();
    void RequestClose();

    void SetOpacity(int percent);
    void SetClickThrough(bool enable);
    void MoveTo(int x, int y);
    void Resize(int width, int height);
    void RemoveFromTaskbar();
    void RestoreToTaskbar();

    void EnableMouseReveal(bool enable);

    HWND GetHwnd() const { return hwnd_; }

    // Two-row chrome: tab strip + browser nav bar. kToolbarHeight is the total
    // height above the WebView (kept named for existing call sites).
    static constexpr int kTabRowHeight = 30;
    static constexpr int kNavRowHeight = 32;
    static constexpr int kToolbarHeight = kTabRowHeight + kNavRowHeight;

    // True while the last click landed in the WebView body (used to gate the
    // zoom hotkeys to "our app's area").
    static bool IsKeyTargetWebView() { return s_keyTargetWebView_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnHotkey(WPARAM hotkeyId);
    void OnMouseMove(int x, int y);
    void OnTimer(UINT_PTR timerId);
    void PaintToolbar(HDC hdc, const RECT& clientRect);

    // Clickable AI-tab strip in the toolbar.
    int  TabCount() const;
    RECT TabRect(int index, const RECT& clientRect) const;
    int  TabHitTest(POINT ptClient, const RECT& clientRect) const;

    // Browser nav bar geometry (row 2).
    RECT GripRect(const RECT& clientRect) const;
    RECT BackRect(const RECT& clientRect) const;
    RECT FwdRect(const RECT& clientRect) const;
    RECT RefreshRect(const RECT& clientRect) const;
    RECT OmniRect(const RECT& clientRect) const;
    void PaintNavBar(HDC hdc, const RECT& clientRect);

    // Omnibox editing (captured via the keyboard hook, no Win32 focus).
    void BeginUrlEdit();
    void EndUrlEdit(bool commit);
    void OnUrlKey(DWORD vkCode, DWORD scanCode);

    AppController& controller_;
    HWND hwnd_ = nullptr;
    bool clickThrough_ = false;
    bool mouseReveal_ = false;
    bool closeRequested_ = false;
    int currentOpacity_ = 100;
    int savedOpacity_ = 100;

    // Omnibox state.
    std::wstring urlText_;        // text being edited / shown
    bool urlSelectAll_ = false;   // first keystroke replaces the whole field
    bool caretOn_ = true;         // blink state while editing

    void ReassertTopmost();
    void InstallHooks();
    void RemoveHooks();
    // Excludes the overlay from screen capture/recording via
    // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE); matches the original.
    void ApplyCaptureProtection();
public:
    void SuspendCaptureProtection();
    void RestoreCaptureProtection();
private:

    static void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event,
        HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

    // System-wide low-level hooks implementing the "main app never loses focus"
    // routing: the mouse hook decides whether the next keystrokes target the
    // WebView, and the keyboard hook forwards those keystrokes into the WebView
    // via CDP (so the overlay never needs Win32 focus).
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void ForwardKeyToWebView(DWORD vkCode, DWORD scanCode, DWORD flags, bool keyDown);

    HWINEVENTHOOK winEventHook_ = nullptr;
    static OverlayWindow* s_instance_;
    static HHOOK s_keyboardHook_;
    static HHOOK s_mouseHook_;
    static bool s_keyTargetWebView_;
    static bool s_urlEditing_;        // true while the omnibox is capturing keys

    static constexpr UINT WM_APP_FORWARD_KEY = WM_APP + 0x101;
    static constexpr UINT WM_APP_URL_KEY     = WM_APP + 0x102;
    static constexpr UINT WM_APP_CLIP_CMD    = WM_APP + 0x103;

    static constexpr wchar_t kClassName[] = L"Shell_InputOverlayWnd";
    static constexpr UINT_PTR kMouseRevealTimer = 1001;
    static constexpr UINT_PTR kSlideTimer = 1002;
    static constexpr UINT_PTR kTopmostTimer = 1003;
    static constexpr UINT_PTR kCaretTimer = 1004;
};
