# CLAUDE.md — Project rules for MYOWNAPP_rebuilt

## ⛔ TOP-PRIORITY INVARIANT — never break this, whatever else changes

**The user's real foreground app (Notepad, VS Code, exam window, etc.) must
NEVER lose focus — its caret must keep blinking — AND the user must still be
able to type into the ChatGPT/AI overlay. Both must hold at the same time.**

Every change, refactor, feature, or "fix" must preserve this. If a requested
change would conflict with it, STOP and warn the user before proceeding; do not
silently trade it away.

### How it is implemented (keep all of these intact)
Primary files: `rebuilt_src/src/ui/OverlayWindow.cpp` / `.h`,
`rebuilt_src/src/browser/WebViewManager.cpp` / `.h`. Full rationale in
`context.md`.

1. **Overlay is `WS_EX_NOACTIVATE`** (plus `WS_EX_TOPMOST | WS_EX_TOOLWINDOW`).
   Re-apply `WS_EX_NOACTIVATE` in *every* method that calls
   `SetWindowLongPtrW(GWL_EXSTYLE, ...)` (SetOpacity, SetClickThrough,
   RemoveFromTaskbar, RestoreToTaskbar). Never create the window without it.
2. **Backstops:** `WM_MOUSEACTIVATE → MA_NOACTIVATE`;
   `WM_WINDOWPOSCHANGING → force SWP_NOACTIVATE`; `WM_ACTIVATE`/`WM_SETFOCUS →
   SetFocus(nullptr)`. The overlay must never keep Win32 focus.
3. **Keystrokes reach the WebView via CDP, not Win32 focus** —
   `Input.insertText` (printable) and `Input.dispatchKeyEvent` (Enter, etc.) in
   `WebViewManager::InjectText` / `InjectKey`. Do NOT switch the overlay to a
   focusable/native-input model to "fix typing"; that breaks the invariant.
4. **Low-level hooks** (`WH_MOUSE_LL` + `WH_KEYBOARD_LL`) do the "last click
   decides" routing. The keyboard hook MUST:
   - never eat modifier keys (Shift/Ctrl/Alt/Win/CapsLock) — pass them through
     so `ToUnicode` sees the real state (capitals & shifted symbols work);
   - pass through any Ctrl/Alt/Win chord so global hotkeys (Ctrl+Shift+H/D/A,
     tab switch, etc.) still fire;
   - only eat plain / Shift-ed character & editing keys and forward them to the
     WebView via `WM_APP_FORWARD_KEY` → `ForwardKeyToWebView`.

Regression test after ANY change: (a) typing in the real app keeps its caret;
(b) typing in the ChatGPT area shows text incl. **capitals/symbols** while the
real app's caret still blinks; (c) Ctrl+Shift+H and Ctrl+Shift+D still fire.

## Build / deploy / versioning
- Build: `cd rebuilt_src && .\build.bat` (configures CMake, builds
  `DesktopWidget`, deploys to workspace root, and auto-saves a timestamped
  copy to `exports/`).
- When deploying, stop any running instance first:
  `Stop-Process -Name DesktopWidget -Force -ErrorAction SilentlyContinue`.
- Keep every build. `exports/DesktopWidget_<YYYYMMDD-HHmmss>.exe` + a dated
  entry in `VERSIONS.md`; the root `DesktopWidget.exe` matches the newest.
- `exports/` MUST stay self-contained: whenever you put an exe there, also copy
  `WebView2Loader.dll` (required to launch) and `dxgi_assist.dll` (optional) next to
  it, or it fails with "WebView2Loader.dll was not found". `build.bat` does this
  automatically; if you build/export manually, copy those DLLs too.
- No licensing/quota in the rebuild — keep it that way (unlimited use).

## Reverse-engineering source of truth
The original is `MYOWNAPP.exe` (UPX-packed). Analyze the unpacked copy
(`upx -d` into a scratch dir) with the REA tools, not the packed file.
