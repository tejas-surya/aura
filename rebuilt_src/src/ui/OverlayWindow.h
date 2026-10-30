#pragma once
#include <Windows.h>
#include <dwmapi.h>
#include <functional>

class AppController;

class OverlayWindow {
public:
    explicit OverlayWindow(AppController& controller);
    ~OverlayWindow();

    bool Create(int width, int height, int opacity);
    void Show(int nCmdShow = SW_SHOWNOACTIVATE);
    void Hide();
    void Destroy();

    void SetOpacity(int percent);
    void SetClickThrough(bool enable);
    void MoveTo(int x, int y);
    void Resize(int width, int height);
    void RemoveFromTaskbar();
    void RestoreToTaskbar();

    void EnableMouseReveal(bool enable);

    HWND GetHwnd() const { return hwnd_; }

    static constexpr int kToolbarHeight = 30;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnHotkey(WPARAM hotkeyId);
    void OnMouseMove(int x, int y);
    void OnTimer(UINT_PTR timerId);
    void PaintToolbar(HDC hdc, const RECT& clientRect);

    AppController& controller_;
    HWND hwnd_ = nullptr;
    bool clickThrough_ = false;
    bool mouseReveal_ = false;
    int currentOpacity_ = 100;
    int savedOpacity_ = 100;

    void ReassertTopmost();

    static void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event,
        HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void ForwardKeyToWebView(DWORD vkCode, DWORD scanCode, DWORD flags, bool keyDown);

    HWINEVENTHOOK winEventHook_ = nullptr;
    static OverlayWindow* s_instance_;
    static HHOOK s_keyboardHook_;
    static HHOOK s_mouseHook_;
    static bool s_keyTargetWebView_;

    static constexpr UINT WM_APP_FORWARD_KEY = WM_APP + 0x101;

    static constexpr wchar_t kClassName[] = L"MYOWNAPP_OverlayClass";
    static constexpr UINT_PTR kMouseRevealTimer = 1001;
    static constexpr UINT_PTR kSlideTimer = 1002;
    static constexpr UINT_PTR kTopmostTimer = 1003;
};
