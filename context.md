# MYOWNAPP — Technical Reference

A WebView2 overlay that floats on top of any app, lets the user type into
ChatGPT (or any web AI), and captures screenshots of the app beneath it —
all **without the underlying app ever losing focus or keyboard input**.

---

## Core Invariant

> The user's real foreground app must NEVER lose focus (its caret keeps
> blinking) AND the user must still be able to type into the overlay.
> Both must hold at the same time.

---

## Architecture

```
+------------------+     +----------------------------+
|  Foreground App  |     |  Overlay (WS_EX_NOACTIVATE)|
|  (e.g. Notepad)  |     |  WS_EX_TOPMOST             |
|                  |     |  WS_EX_TOOLWINDOW           |
|  HAS FOCUS       |     |  +------------------------+ |
|  CARET BLINKS    |     |  | WebView2 (ChatGPT)    | |
|  GETS KEYBOARD   |     |  | Input via JS injection | |
+------------------+     |  +------------------------+ |
        ^                 +----------------------------+
        |                            |
   Normal Win32              JS execCommand('insertText')
   keyboard input            + CDP Input.dispatchKeyEvent
```

"Last click decides" routing: click inside the overlay → keystrokes go to
WebView2 via JS. Click anywhere else → keystrokes go to the foreground app
normally.

---

## 1. Overlay Never Takes Foreground

**File:** `rebuilt_src/src/ui/OverlayWindow.cpp`

### Window creation

```cpp
DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
```

`WS_EX_NOACTIVATE` prevents the window from ever becoming the foreground
window at the window-manager level. Clicks and scroll events still reach the
WebView2 child, but the top-level window cannot activate, so the real app
keeps foreground and keyboard.

### Re-assert in every style-modifying method

`SetWindowLongPtrW(GWL_EXSTYLE, ...)` replaces the entire ex-style word.
Every method that calls it must OR in `WS_EX_NOACTIVATE`:

- `SetOpacity()` (both add-layered and remove-layered branches)
- `SetClickThrough()`
- `RemoveFromTaskbar()` / `RestoreToTaskbar()`

### Defensive backstops

| Message | Response | Why |
|---------|----------|-----|
| `WM_MOUSEACTIVATE` | `MA_NOACTIVATE` | Block activation from mouse |
| `WM_WINDOWPOSCHANGING` | Force `SWP_NOACTIVATE` | Block activation from position changes |
| `WM_ACTIVATE` / `WM_SETFOCUS` | `SetFocus(nullptr)` | Last resort: strip focus if something slips through |

---

## 2. Input Routing (Mouse + Keyboard Hooks)

**File:** `rebuilt_src/src/ui/OverlayWindow.cpp`

### Mouse hook (`WH_MOUSE_LL`)

Watches `WM_LBUTTONDOWN` / `WM_RBUTTONDOWN`. If the click is inside the
overlay's WebView2 area (below toolbar), sets `s_keyTargetWebView_ = true`.
If outside, sets it `false`. Never eats mouse events — always calls
`CallNextHookEx`.

### Keyboard hook (`WH_KEYBOARD_LL`)

When `s_keyTargetWebView_` is true:
1. Packs `vkCode`, `scanCode`, `flags`, and `keyDown` into an lParam
2. `PostMessageW(hwnd_, WM_APP_FORWARD_KEY, vkCode, packed)`
3. Returns `1` — **eats** the keystroke so the foreground app doesn't see it

When `s_keyTargetWebView_` is false: calls `CallNextHookEx` — keystrokes
flow to the foreground app normally.

**Critical rules for the keyboard hook:**
- Never eat modifier keys (Shift/Ctrl/Alt/Win/CapsLock) — pass them through
  so `ToUnicode` sees the real keyboard state
- Pass through any Ctrl/Alt/Win chord so global hotkeys still fire
- Only eat plain or Shift-ed character and editing keys
- Must return fast (<200ms) or Windows removes the hook — just `PostMessage`,
  do heavy work on the UI thread

### Character translation (`ForwardKeyToWebView`)

Uses `ToUnicode()` with the current keyboard state to convert virtual key
codes to printable characters:

```cpp
BYTE ks[256]{};
GetKeyboardState(ks);
if (GetAsyncKeyState(VK_SHIFT) & 0x8000) ks[VK_SHIFT] = 0x80;
if (GetAsyncKeyState(VK_CAPITAL) & 1)    ks[VK_CAPITAL] |= 1;
WCHAR chars[8]{};
int n = ToUnicode(vkCode, scanCode, ks, chars, 8, 0);
```

- Printable + no Ctrl/Alt → `InjectText()` (JS text insertion)
- Non-printable or Ctrl/Alt → `InjectKey()` (CDP key event dispatch)

---

## 3. Text Injection Into WebView2

**File:** `rebuilt_src/src/browser/WebViewManager.cpp`

### Printable text → JS `execCommand('insertText')`

```cpp
void WebViewManager::InjectText(const std::wstring& text) {
    std::wstring js = L"(function(){var el=document.activeElement;"
        L"if(el){el.focus();document.execCommand('insertText',false,'"
        + JsonEscape(text) + L"');}})()";
    webView_->ExecuteScript(js.c_str(), nullptr);
}
```

This is focus/occlusion-independent — it works even when the WebView2 window
is not the Win32 focus owner.

### Non-printable keys → CDP `Input.dispatchKeyEvent`

```cpp
void WebViewManager::InjectKey(DWORD vkCode, const std::wstring& key,
                               const std::wstring& code, bool keyDown, int modifiers) {
    std::wstring type = keyDown ? L"rawKeyDown" : L"keyUp";
    // JSON with type, vkCode, modifiers, key, code
    webView_->CallDevToolsProtocolMethod(L"Input.dispatchKeyEvent", params.c_str(), nullptr);
}
```

CDP talks directly to the Chromium renderer via IPC — it does not depend on
Win32 focus. Used for Enter, Backspace, Delete, Tab, Escape, arrows.

**CDP modifiers bitmask:** Alt=1, Ctrl=2, Meta=4, Shift=8.

---

## 4. WebView2 Occlusion Fix

**File:** `rebuilt_src/src/browser/WebViewManager.cpp`

By default Chromium marks the renderer *occluded* when another window covers
the overlay, then **drops injected input and pauses painting**. Fix: pass
`AdditionalBrowserArguments` when creating the WebView2 environment:

```
--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,msWebView2BrowserHitTransparent
```

This keeps the renderer alive and accepting input even when covered by a
full-screen or topmost app.

---

## 5. Screen-Capture Invisibility (Own Overlay)

**File:** `rebuilt_src/src/ui/OverlayWindow.cpp`

Right after creating the window:

```cpp
SetWindowDisplayAffinity(hwnd, 0x11 /* WDA_EXCLUDEFROMCAPTURE */);
```

`WDA_EXCLUDEFROMCAPTURE` (Windows 10 2004+) makes DWM exclude the overlay
from every capture path — GDI BitBlt, PrintWindow, DXGI Desktop Duplication,
Windows.Graphics.Capture. The overlay stays visible to the user but any
screen recorder or proctoring tool captures the content *behind* it.

Fallback to `WDA_MONITOR` (0x01) on older Windows.

Applied in `ApplyCaptureProtection()`, called from `Create()` and `Show()`.
Temporarily suspended during our own captures (`SuspendCaptureProtection` /
`RestoreCaptureProtection`).

---

## 6. Screenshot Capture — DXGI Desktop Duplication

**File:** `rebuilt_src/src/automation/ScreenCapture.cpp`

Primary capture path (`CaptureVirtualScreen`):

```
D3D11CreateDevice → IDXGIDevice → GetAdapter → EnumOutputs(0)
→ IDXGIOutput1 → DuplicateOutput → AcquireNextFrame (500ms, 10 retries)
→ staging CreateTexture2D(CPU_READ) → CopyResource → Map
→ GDI+ Bitmap → PNG
```

Falls back to GDI `BitBlt` (full virtual screen) if DXGI fails.

DXGI reads the real composited GPU output, so it captures GPU/DWM-composited
windows that GDI BitBlt renders as empty desktop background.

---

## 7. WDA Bypass — Capturing Protected Target Apps (dxgi_assist.dll)

When the target app itself has `SetWindowDisplayAffinity` protection, our
DXGI capture sees black. The solution: inject a DLL into the target process
that temporarily clears WDA during capture.

### DLL design (~13 KB, no dependencies beyond user32/kernel32)

**File:** `rebuilt_src/hook_dll/HookMain.cpp`

```
DllMain(DLL_PROCESS_ATTACH):
  └─ CreateThread(HookInitThread)     ← NOTHING else (loader lock safe)

HookInitThread:
  ├─ OpenFileMappingW("Local\DWM_Composition_Data")
  ├─ MapViewOfFile → SharedHookData*
  ├─ Signal "Local\UFOlee_Hook_Active_<PID>" (ready event)
  └─ Loop: wait on "Local\DWM_Composition_Notify"
       → SetWindowDisplayAffinity(hwnd, 0)   for each tracked window
       → Wait for main exe to finish capture  (polls wdaCleared flag)
       → SetWindowDisplayAffinity(hwnd, orig) restore original WDA

DllMain(DLL_PROCESS_DETACH):
  └─ Signal thread to stop, wait 3s, unmap shared memory
```

**Hard rules for the injected DLL:**
- `DllMain` must do NOTHING except `DisableThreadLibraryCalls` + `CreateThread`.
  All real work goes in the worker thread (after loader lock is released).
- No file I/O in the target process — `GetModuleFileNameW(nullptr)` returns the
  *target's* path, not yours. Writing files there triggers security software.
- No `CryptAcquireContext` or any call that loads additional DLLs under loader lock.
- No named mutexes or blocking waits under loader lock.
- Logging: `OutputDebugStringA` only (zero side effects, no file handles).
- Call `SetWindowDisplayAffinity` directly — do NOT use API hooking/trampolining
  libraries. Simplified MinHook-style trampolines fail on x64 because they don't
  relocate RIP-relative instructions, causing crashes in the target process.

### Injection flow (AppController::InjectIntoPid)

**File:** `rebuilt_src/src/app/AppController.cpp`

```cpp
// 1. Enable SeDebugPrivilege (helps with elevated targets)
OpenProcessToken → AdjustTokenPrivileges(SE_DEBUG_NAME)

// 2. Open target process
HANDLE hProc = OpenProcess(
    PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
    PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
    FALSE, pid);

// 3. Write DLL path into target's memory
void* remoteMem = VirtualAllocEx(hProc, nullptr, pathBytes,
    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
WriteProcessMemory(hProc, remoteMem, hookDllPath, pathBytes, nullptr);

// 4. Create remote thread calling LoadLibraryW
auto loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLib, remoteMem, 0, nullptr);
WaitForSingleObject(hThread, 5000);

// 5. Check result (LoadLibraryW returns module handle; 0 = failure)
DWORD exitCode;
GetExitCodeThread(hThread, &exitCode);  // non-zero = success

// 6. Wait for DLL's "ready" event
HANDLE hReady = OpenEventW(SYNCHRONIZE, FALSE,
    L"Local\\WinRT_Comp_ModReady_<pid>");
WaitForSingleObject(hReady, 3000);
```

Skip system processes (`explorer.exe`, `dwm.exe`, etc.), browser processes
(`msedgewebview2.exe`, `chrome.exe`, etc.), and self processes. Track injected
PIDs to prevent double injection.

### Capture flow (AppController::CaptureAndSend)

```
1. GetForegroundWindow → enumerate its windows for WDA → fill SharedHookData.trackedWindows
2. If WDA found → InjectIntoPid(targetPid)
3. SuspendCaptureProtection()  — clear our own overlay's WDA
4. SetEvent(scrubEvent)        — DLL clears target's WDA
5. Poll SharedHookData.wdaCleared (up to 500ms)
6. CaptureVirtualScreen()      — DXGI Desktop Duplication
7. SharedHookData.wdaCleared = FALSE  — DLL restores target's WDA
8. RestoreCaptureProtection()  — re-apply our overlay's WDA
9. SendScreenshot(bitmap)      — pass PNG to WebView
```

### Shared memory layout

**File:** `rebuilt_src/hook_dll/SharedIPC.h`

```cpp
struct SharedHookData {
    DWORD ownerPid;          // Main exe's PID
    HWND  overlayHwnd;       // Our overlay window handle
    BOOL  hookActive;        // DLL loaded and listening
    BOOL  wdaCleared;        // TRUE while WDA temporarily cleared

    DWORD trackedWindowCount;
    struct {
        HWND  hwnd;          // Target window handle
        DWORD originalWda;   // Original WDA value to restore
        BOOL  tracked;       // Is this entry active
    } trackedWindows[64];
};
```

### Named kernel objects

| Name | Type | Owner | Purpose |
|------|------|-------|---------|
| `Local\WinRT_Comp_SharedData` | File mapping | Main exe creates, DLL opens | Shared memory for tracked windows and flags |
| `Local\WinRT_Comp_FrameSync` | Auto-reset event | Main exe signals | Tells DLL to clear WDA now |
| `Local\WinRT_Comp_ModReady_<PID>` | Manual-reset event | DLL signals | Tells main exe the DLL is ready |

### Manifest

**File:** `rebuilt_src/res/app.manifest`

Main exe uses `highestAvailable` execution level — auto-elevates for admin
users so injection works on elevated target processes. Non-admin users can
still inject into same-integrity-level targets.

---

## 8. Clipboard — Ctrl+C / V / X / A

**File:** `rebuilt_src/src/browser/WebViewManager.cpp`

The overlay can't use `execCommand('copy'|'paste')` because Chromium requires
real Win32 focus for clipboard access. Solution: native Win32 clipboard +
focus-free JS.

- **Routing:** keyboard hook eats Ctrl+C/V/X/A only when targeting
  WebView → `WM_APP_CLIP_CMD` → `ClipboardCommand(op)`
- **Copy/Cut:** `ExecuteScript` gets selection as base64(UTF-8), callback
  writes to clipboard via `OpenClipboard`/`SetClipboardData(CF_UNICODETEXT)`
- **Paste:** `GetClipboardData(CF_UNICODETEXT)` → JS: `setRangeText` for
  inputs, synthetic `paste` ClipboardEvent for rich editors, fallback
  `execCommand('insertText')`
- **Select All:** `el.select()` for inputs, else `execCommand('selectAll')`
- Image paste: `CF_BITMAP` → PNG via `HBitmapToPng` → `SendScreenshot`

---

## File Map

| File | Role |
|------|------|
| `rebuilt_src/src/ui/OverlayWindow.cpp/.h` | Window creation, WS_EX_NOACTIVATE, WDA protection, LL mouse+keyboard hooks, WM_APP_FORWARD_KEY handler, ForwardKeyToWebView |
| `rebuilt_src/src/browser/WebViewManager.cpp/.h` | WebView2 init (occlusion fix), InjectText (JS), InjectKey (CDP), clipboard, screenshot paste |
| `rebuilt_src/src/app/AppController.cpp/.h` | Orchestration, hook system init, CaptureAndSend, InjectIntoPid, hotkey dispatch |
| `rebuilt_src/src/automation/ScreenCapture.cpp/.h` | DXGI Desktop Duplication + GDI fallback, PNG encoding |
| `rebuilt_src/hook_dll/HookMain.cpp` | Injected DLL (`dxgi_assist.dll`): shared memory, scrub event loop, direct SetWindowDisplayAffinity calls |
| `rebuilt_src/hook_dll/SharedIPC.h` | SharedHookData struct, named object constants (WinRT_Comp_* prefix), process skip-lists |
| `rebuilt_src/hook_dll/CMakeLists.txt` | DLL build config (output: `dxgi_assist.dll`, user32 + kernel32 only, no MinHook) |
| `rebuilt_src/res/app.manifest` | `highestAvailable` elevation, DPI awareness, common controls |

---

## Build

```
cd rebuilt_src
.\build.bat
```

Builds both `DesktopWidget.exe` and `dxgi_assist.dll`, deploys to workspace
root, and saves a timestamped copy in `exports/`. The `exports/` directory
also gets `WebView2Loader.dll` and `dxgi_assist.dll` so each export is
self-contained and runnable.

Dependencies: Visual Studio / Build Tools (C++ workload), WebView2 NuGet
package, nlohmann/json.

---

## Diagnostic Logging

All logging uses `DbgLog()` which wraps `OutputDebugStringA` — compiled out
(`NDEBUG`) in Release builds. View with DebugView or VS Output window in
Debug builds only.

**Main exe prefixes:**

| Prefix | What it shows |
|--------|--------------|
| `[HOOK]` | DLL discovery at startup, shared memory/event creation |
| `[CAPTURE]` | Foreground window, PID, WDA window count, capture result |
| `[INJECT]` | OpenProcess/VirtualAllocEx/CreateRemoteThread/LoadLibraryW results with error codes |

**Injected DLL prefix:** `[dxgi PID:<pid>]` — shared memory status, scrub
event handling, per-window WDA clear/restore results with error codes.

---

## 9. Anti-Detection Hardening

Every user-facing name — process, window, kernel object, filesystem path,
and string literal — uses generic system-sounding identifiers that blend
with normal Windows components. A target app enumerating processes, windows,
or named objects sees nothing identifiable.

### Naming scheme

| Surface | Old (fingerprintable) | New (generic) |
|---------|----------------------|---------------|
| **Exe** | `MYOWNAPP_rebuilt.exe` | `DesktopWidget.exe` |
| **DLL** | `app_hook.dll` | `dxgi_assist.dll` |
| **Window class** | `MYOWNAPP_OverlayClass` | `Shell_InputOverlayWnd` |
| **Window title** | `MYOWNAPP` | *(empty string)* |
| **Manifest identity** | `MYOWNAPP` | `DesktopWidget` |
| **Single-instance mutex** | `Local\MYOWNAPP_SingleInstance_Mutex` | `Local\WinRT_Comp_SessionLock` |
| **Shared memory** | `Local\DWM_Composition_Data` | `Local\WinRT_Comp_SharedData` |
| **Scrub event** | `Local\DWM_Composition_Notify` | `Local\WinRT_Comp_FS_<session>` (per-session random suffix) |
| **Hook active event** | `Local\UFOlee_Hook_Active_<PID>` | `Local\WinRT_Comp_ModReady_<PID>_<session>` |
| **WebView2 data folder** | `%LOCALAPPDATA%\UFOlee\edge_profile_` | `%LOCALAPPDATA%\Microsoft\WebToolkit\Default` |
| **Debug log file** | `debug_log.enc` | `cache.dat` |
| **Crypto key seed** | `MYOWNAPP_v1` | `WinRT_Comp_v1` |
| **JS global** | `window.__ufolee_shot` | `window.__wv_cap` |
| **Self-process list** | `UFOlee.exe`, `MYOWNAPP.exe`, etc. (7 names) | `DesktopWidget.exe` (1 name) |
| **DLL debug prefix** | `[app_hook PID:...]` | `[dxgi PID:...]` |

### Mitigations applied (from futurecorrections.md)

| Vector | Mitigation |
|--------|-----------|
| `OutputDebugStringA` messages | **FIXED:** `DbgLog()` compiles to no-op in Release (`#ifdef NDEBUG`) |
| `SeDebugPrivilege` elevation | **MITIGATED:** JIT enable/disable — only enabled during `OpenProcess` if access denied, disabled immediately after |
| `WH_KEYBOARD_LL` / `WH_MOUSE_LL` detectable | **MITIGATED:** Hooks installed lazily on `Show()`, removed on `Hide()` — minimizes detection window |
| `CreateRemoteThread` injection | **MITIGATED:** Module-based duplicate check (`K32EnumProcessModulesEx`), session-0 filter, 32-bit process filter |
| WDA suspend window (~2000ms) | **MITIGATED:** Reduced to ~60ms DLL side, ~30ms exe side |
| Kernel object name fingerprinting | **MITIGATED:** Per-session random suffix in shared memory, used for all event names |

### What this still does NOT fix (architectural)

| Vector | Why it's hard |
|--------|--------------|
| `CreateRemoteThread` injection method itself | Most-detected; alternatives (manual map, APC) need kernel-level complexity |
| Target self-polling WDA during scrub | Would need to hook `GetWindowDisplayAffinity` in target — needs x64 trampoline |
| `SendInput` has `LLKHF_INJECTED` flag | Hardware-level; only kernel drivers can mask it (not needed — we type via CDP) |
