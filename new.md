# MYOWNAPP Rebuilt - Technical Context Document

## Problem Statement

The overlay app embeds a ChatGPT WebView2 control on top of the user's desktop. The core requirement:

> **The main window (Notepad, Claude Chat, VS Code, etc.) must NEVER lose focus -- at any cost -- even while the user is typing into the ChatGPT overlay.**

In standard Win32, this is contradictory: only ONE foreground window receives keyboard input, and only the foreground thread's caret blinks. So the two requirements coexist only by:

1. Guaranteeing the overlay **never becomes the foreground window**
2. Injecting keystrokes into WebView2 through a path that does **not** depend on Win32 focus

---

## Architecture Overview

```
+------------------+     +----------------------------+
|  Main App        |     |  MYOWNAPP Overlay          |
|  (e.g. Notepad)  |     |  WS_EX_NOACTIVATE          |
|                  |     |                            |
|  HAS FOREGROUND  |     |  +------------------------+|
|  HAS KEYBOARD    |     |  | WebView2 (ChatGPT)    ||
|  CARET BLINKS    |     |  | Receives CDP input     ||
+------------------+     |  +------------------------+|
        ^                 +----------------------------+
        |                            |
   Keyboard input             CDP injection
   flows normally         (Input.insertText /
   via Win32 focus         Input.dispatchKeyEvent)
```

### Key Routing Model: "Last Click Decides"

- Click inside ChatGPT area -> keystrokes are intercepted and forwarded to WebView2 via CDP
- Click anywhere else -> keystrokes flow to the main app normally

---

## What Failed Before (and Why)

### Failure 1: Main window kept losing focus

**Root cause:** The overlay was created with `WS_EX_TOPMOST | WS_EX_TOOLWINDOW` -- **`WS_EX_NOACTIVATE` was missing**.

Without `WS_EX_NOACTIVATE`, when the user clicks inside the WebView2 area, Chromium's internal `SetFocus()` call pulls the overlay window to the foreground. This happens *before* our `WM_ACTIVATE` handler runs, so the main app already received `WM_KILLFOCUS` and its caret stopped blinking.

The previous mitigations (`MA_NOACTIVATE`, `SetFocus(nullptr)`) were insufficient:
- `MA_NOACTIVATE` only handles `WM_MOUSEACTIVATE` -- WebView2's internal focus grab bypasses this
- `SetFocus(nullptr)` runs too late (inside `WM_ACTIVATE`) -- it strips keyboard focus from our thread but does NOT return foreground to the main app

### Failure 2: Could not type in ChatGPT

**Root cause:** The keyboard hook was using `PostMessage(WM_KEYDOWN/WM_CHAR)` to the Chromium widget HWND.

Chromium **ignores synthetic window messages** when the widget lacks real Win32 focus. The messages never reach the editable field. This is by design in Chromium's message pump.

---

## The Solution (Current Implementation)

### Part 1: Overlay Never Takes Foreground

**File:** `rebuilt_src/src/ui/OverlayWindow.cpp`

**`WS_EX_NOACTIVATE`** added to:

1. **Window creation** (line 40):
   ```cpp
   DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
   ```

2. **Every style-modifying method** -- because `SetWindowLongPtrW(GWL_EXSTYLE, ...)` replaces the entire ex-style, any method that modifies it could accidentally clear `WS_EX_NOACTIVATE`:
   - `SetOpacity()` (lines 113, 119) -- both the add-layered and remove-layered branches
   - `SetClickThrough()` (line 134)
   - `RemoveFromTaskbar()` (line 151)
   - `RestoreToTaskbar()` (line 158)

3. **Defensive backstops** (belt-and-suspenders):
   - `WM_MOUSEACTIVATE` -> returns `MA_NOACTIVATE` (line 240)
   - `WM_WINDOWPOSCHANGING` -> forces `SWP_NOACTIVATE` flag (line 263)
   - `WM_ACTIVATE` / `WM_SETFOCUS` -> calls `SetFocus(nullptr)` (line 249)

**Why `WS_EX_NOACTIVATE` is the single most important fix:** It prevents the window from EVER becoming the foreground window at the window-manager level. Mouse clicks and scroll events are still delivered to the WebView2 child control. But the top-level window cannot be activated, so the main app keeps foreground status, its caret keeps blinking, and it keeps receiving keyboard input.

### Part 2: Mouse Hook Sets Keyboard Target

**File:** `rebuilt_src/src/ui/OverlayWindow.cpp`, `LowLevelMouseProc()` (line 404)

A `WH_MOUSE_LL` system-wide hook watches for `WM_LBUTTONDOWN` / `WM_RBUTTONDOWN`:

```cpp
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
```

- If click is inside overlay's WebView2 rect (below toolbar): `s_keyTargetWebView_ = true`
- If click is anywhere else: `s_keyTargetWebView_ = false`
- The mouse hook always calls `CallNextHookEx` -- it never eats mouse events

### Part 3: Keyboard Hook Intercepts and Forwards

**File:** `rebuilt_src/src/ui/OverlayWindow.cpp`, `LowLevelKeyboardProc()` (line 421)

A `WH_KEYBOARD_LL` system-wide hook:

```cpp
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
            return 1;  // EAT the keystroke
        }
    }
    return CallNextHookEx(s_keyboardHook_, nCode, wParam, lParam);
}
```

**Critical design decisions:**
- When target is WebView2: returns `1` (eats the key so the main app doesn't also receive it)
- When target is main app: calls `CallNextHookEx` (keystrokes flow normally)
- **LL hooks must return fast** (<200ms or Windows removes them), so the hook only packages the event and `PostMessage`s a private `WM_APP_FORWARD_KEY` message. The actual CDP call happens on the UI thread.

**Packed lParam format:**
```
Bits  0-7:   scanCode
Bits  8-15:  flags (from KBDLLHOOKSTRUCT)
Bit   16:    1 = keyDown, 0 = keyUp
```

### Part 4: CDP Injection Into WebView2

**Files:**
- `rebuilt_src/src/browser/WebViewManager.cpp` (lines 230-248)
- `rebuilt_src/src/browser/WebViewManager.h` (lines 43-45)

The `WM_APP_FORWARD_KEY` handler unpacks the event and calls `ForwardKeyToWebView()`, which uses Chrome DevTools Protocol to inject input:

#### Printable Characters -> `Input.insertText`

```cpp
void WebViewManager::InjectText(const std::wstring& text) {
    if (!webView_) return;
    std::wstring params = L"{\"text\":\"" + JsonEscape(text) + L"\"}";
    webView_->CallDevToolsProtocolMethod(L"Input.insertText", params.c_str(), nullptr);
}
```

- Inserts text into the page's currently focused element
- Fires `input`/`beforeinput` events that ChatGPT's React/ProseMirror composer needs
- Most reliable path for text input

#### Non-Printable Keys -> `Input.dispatchKeyEvent`

```cpp
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
```

Used for: Enter (sends message), Backspace, Delete, Tab, Escape, arrows, Home, End, and Ctrl/Alt combos.

**CDP modifiers bitmask:**
| Bit | Modifier |
|-----|----------|
| 1   | Alt      |
| 2   | Ctrl     |
| 4   | Meta     |
| 8   | Shift    |

#### Why CDP Works Without Win32 Focus

CDP (`CallDevToolsProtocolMethod`) talks directly to the Chromium renderer process through an IPC channel. It bypasses the Win32 message pump entirely. The renderer doesn't check whether its host window has Win32 focus -- it simply processes the input event. This is the same mechanism Chrome DevTools uses to inject input during automated testing.

**No domain enable required** -- `Input.insertText` and `Input.dispatchKeyEvent` work immediately on any `ICoreWebView2` instance.

### Part 5: Character Translation

**File:** `rebuilt_src/src/ui/OverlayWindow.cpp`, `ForwardKeyToWebView()` (line 441)

The handler translates virtual key codes to printable characters using `ToUnicode()`:

```cpp
BYTE ks[256]{};
GetKeyboardState(ks);
if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   ks[VK_SHIFT]   = 0x80;
if (GetAsyncKeyState(VK_CAPITAL) & 1)      ks[VK_CAPITAL] |= 1;
WCHAR chars[8]{};
int n = ToUnicode(vkCode, scanCode, ks, chars, 8, 0);
```

- If printable and no Ctrl/Alt held: uses `InjectText()` (direct text insertion)
- If non-printable or has Ctrl/Alt: maps to key/code strings and uses `InjectKey()` (key event dispatch)

---

## File Map

| File | Role |
|------|------|
| `rebuilt_src/src/ui/OverlayWindow.cpp` | Window creation, WS_EX_NOACTIVATE, both LL hooks, WM_APP_FORWARD_KEY handler, ForwardKeyToWebView |
| `rebuilt_src/src/ui/OverlayWindow.h` | Hook declarations, static members (s_keyboardHook_, s_mouseHook_, s_keyTargetWebView_), WM_APP_FORWARD_KEY constant |
| `rebuilt_src/src/browser/WebViewManager.cpp` | InjectText (CDP Input.insertText), InjectKey (CDP Input.dispatchKeyEvent), JsonEscape helper |
| `rebuilt_src/src/browser/WebViewManager.h` | InjectText/InjectKey declarations |
| `rebuilt_src/src/app/AppController.cpp` | Orchestration, `overlay_->Show(SW_SHOWNOACTIVATE)`, LoadHookDll |
| `rebuilt_src/hook_dll/HookMain.cpp` | Injected into target: direct SetWindowDisplayAffinity WDA clear/restore, shared memory + event IPC. NOT keyboard-related |
| `rebuilt_src/hook_dll/SharedIPC.h` | SharedHookData struct, named object constants, process skip-lists |

---

## Static Members in OverlayWindow

```cpp
static OverlayWindow* s_instance_;     // Singleton for hook callbacks
static HHOOK s_keyboardHook_;          // WH_KEYBOARD_LL handle
static HHOOK s_mouseHook_;            // WH_MOUSE_LL handle
static bool s_keyTargetWebView_;      // true = next keystrokes go to ChatGPT
```

These are `static` because Win32 hook callbacks (`LowLevelKeyboardProc`, `LowLevelMouseProc`) must be static functions -- they have no `this` pointer. `s_instance_` provides access to instance members.

---

## Known Limitations and Fallbacks

### If CDP text doesn't land in ChatGPT's composer

This can happen if the ChatGPT page's `document.activeElement` loses focus when the WebView2 window is defocused. Fallback options (in priority order):

1. **CDP focus nudge:** Before `Input.insertText`, dispatch a Tab or click event via CDP to re-focus the composer
2. **Runtime.evaluate:** Call `document.querySelector('[contenteditable]').focus()` via CDP's `Runtime.evaluate`
3. **ExecuteScript:** Use `webView_->ExecuteScript()` to run JS that targets `document.activeElement` and inserts text directly

### WebView2's GotFocus handler

The `add_GotFocus` handler (WebViewManager.cpp line 111) intentionally does nothing (returns `S_OK`). Previous versions tried to call `SetFocus(nullptr)` here, but that caused the overlay to lose ALL keyboard capability. The empty handler prevents WebView2 from doing anything unexpected when it thinks it gained focus.

---

## Hook DLL (app_hook.dll) — WDA Bypass for Capturing Protected Apps

The hook DLL (`rebuilt_src/hook_dll/`) is process-injected into the **target app**
to temporarily clear its `SetWindowDisplayAffinity` protection so our DXGI/GDI
screenshot captures the real content instead of a black rectangle. It has nothing
to do with the keyboard routing or focus management.

### What the DLL does (working v15 implementation)

The DLL is **tiny** (~13 KB) and does exactly three things:
1. Opens the shared memory (`Local\DWM_Composition_Data`) created by the main exe
2. Waits for the scrub event (`Local\DWM_Composition_Notify`)
3. When signaled: calls `SetWindowDisplayAffinity(hwnd, 0)` on each tracked window
   to clear WDA, waits for the main exe to finish capturing, then restores the
   original WDA values

### Capture flow (CaptureAndSend in AppController.cpp)

```
1. Get foreground window, enumerate its windows for WDA protection
2. If WDA found → InjectIntoPid(targetPid):
   - OpenProcess(CREATE_THREAD | VM_WRITE | VM_READ | ...)
   - VirtualAllocEx → WriteProcessMemory(hookDllPath)
   - CreateRemoteThread(LoadLibraryW) → DLL loads in target
   - Wait for "hook ready" event (up to 3s)
3. SuspendCaptureProtection() — clear our own overlay's WDA
4. SetEvent(scrubEvent) → DLL clears target's WDA
5. Poll wdaCleared (up to 500ms)
6. CaptureVirtualScreen() — DXGI Desktop Duplication (or GDI fallback)
7. Set wdaCleared = FALSE → DLL restores target's WDA
8. RestoreCaptureProtection() — re-apply our overlay's WDA
9. Send screenshot to WebView
```

### What went wrong in v14 (and what crashed the target)

**Three bugs compounded to make the injection either do nothing or crash the target:**

#### Bug 1: ScrubWdaNow used MinHook trampoline pointers (silent failure)

The scrub function called `fpSetWDA(hwnd, 0)` — the MinHook "original function"
trampoline pointer. When MinHook failed to install hooks (see Bug 2), `fpSetWDA`
stayed **nullptr**, and `fpSetWDA ? fpSetWDA(hwnd, 0) : FALSE` silently evaluated
to FALSE. **WDA was never cleared.** The capture proceeded but got a black window.

**Fix:** Call `SetWindowDisplayAffinity(hwnd, 0)` directly — the real Win32 API.
The DLL is already running inside the target process, so it has the right to call
the API on the target's own windows.

#### Bug 2: Custom MinHook corrupted target code (crash)

The in-tree MinHook (`hook_dll/MinHook/MinHook.c`) was a simplified single-file
reimplementation with a **broken x64 instruction length decoder** and **no
RIP-relative instruction relocation**. On x64 Windows, API entry points commonly
use `[rip+disp32]` addressing in their first 14 bytes. The trampoline copied
those bytes verbatim without adjusting the displacement, so calling through the
trampoline jumped to a garbage address → **crash** or **corrupted API → crash**.

**Fix:** MinHook removed entirely. Direct API calls don't need trampolines.
API hooks (to block the target from re-setting WDA during the capture window)
can be added later with the real MinHook library if needed.

#### Bug 3: EncryptedLog killed the target from DllMain (crash/deadlock)

The logging function `EncryptedLog` was called from `DllMain(DLL_PROCESS_ATTACH)`
— which runs **under the Windows loader lock**. It did three things that are
**forbidden under loader lock**:

| Call | Why it's fatal |
|------|---------------|
| `CreateMutexW` + `WaitForSingleObject` | Can deadlock against the loader lock |
| `CryptAcquireContextW` | Loads crypto provider DLLs → re-enters the loader → **deadlock** |
| `CreateFileW(debug_log.enc)` in the **target app's directory** | `GetModuleFileNameW(nullptr)` returns the **target's** exe path, not our DLL's. Writing a foreign file into e.g. `C:\Program Files\ExamBrowser\` either fails (permissions) or triggers security software that kills the process |

**Fix:** `DllMain` does **nothing** except `CreateThread`. All logging replaced
with `OutputDebugStringA` only — no file I/O, no mutexes, no crypto, no DLL
loads. The worker thread (which runs after the loader lock is released) does all
real work.

### What the working DLL looks like (v15)

```
DllMain(DLL_PROCESS_ATTACH):
  └─ CreateThread(HookInitThread)  ← nothing else, loader lock safe

HookInitThread:
  ├─ OpenFileMappingW("Local\DWM_Composition_Data")
  ├─ MapViewOfFile → g_pShared
  ├─ SetEvent("Local\UFOlee_Hook_Active_<PID>")  ← tells main exe we're ready
  └─ Loop:
       WaitForSingleObject("Local\DWM_Composition_Notify")
       → ScrubWdaNow(true):  SetWindowDisplayAffinity(hwnd, 0) for each tracked window
       → Wait for main exe to capture (polls wdaCleared == FALSE)
       → ScrubWdaNow(false): SetWindowDisplayAffinity(hwnd, originalWda)

DllMain(DLL_PROCESS_DETACH):
  └─ Signal thread to stop, wait 3s, unmap shared memory
```

No MinHook, no file I/O, no crypto. Just shared memory + events + direct API.

### Main exe injection diagnostics

The main exe logs to `OutputDebugStringA` (visible in DebugView or VS Output):
- `[HOOK] app_hook.dll found/not found` — DLL discovery at startup
- `[CAPTURE] fg=<hwnd> pid=<pid> wdaWindows=<count>` — each capture attempt
- `[INJECT] PID <pid>: OpenProcess/CreateRemoteThread/LoadLibraryW result`
- `[CAPTURE] wdaCleared=<bool> after <N> polls`
- `[CAPTURE] bitmap=<bytes> targetHadWda=<bool>`

The main exe also enables `SeDebugPrivilege` before injection (helps when the
target is running elevated) and uses `highestAvailable` manifest elevation.

### Shared memory layout (SharedIPC.h)

```cpp
struct SharedHookData {
    DWORD ownerPid;        // Main exe's PID
    HWND  overlayHwnd;     // Our overlay window
    BOOL  hookActive;      // DLL is loaded and listening
    BOOL  wdaCleared;      // TRUE while WDA is temporarily cleared

    DWORD trackedWindowCount;
    struct {
        HWND  hwnd;
        DWORD originalWda;
        BOOL  tracked;
    } trackedWindows[64];
};
```

Named objects:
- `Local\DWM_Composition_Data` — shared memory (main exe creates, DLL opens)
- `Local\DWM_Composition_Notify` — auto-reset event (main exe signals → DLL scrubs)
- `Local\UFOlee_Hook_Active_<PID>` — manual-reset event (DLL signals → main exe knows DLL is ready)

---

## Build and Deploy

```powershell
# Build
cd rebuilt_src
.\build.bat

# Deploy (kill running instance first)
Stop-Process -Name "MYOWNAPP_rebuilt" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Copy-Item "rebuilt_src\build\Release\MYOWNAPP_rebuilt.exe" "MYOWNAPP_rebuilt.exe" -Force
```

The WebView2 SDK is at `rebuilt_src/packages/Microsoft.Web.WebView2.1.0.2478.35/`.

---

## Original Binary Analysis Notes

The original `MYOWNAPP.exe` (unpacked as `MYOWNAPP_unpacked.exe`) was analyzed via Ghidra/REA:
- WndProc at `FUN_14001b230`
- `FUN_14000d700` calls `SetFocus(NULL)` conditionally (when `DAT_1400f56cd == 0`)
- The original does NOT use `WS_EX_NOACTIVATE` or `WM_NCACTIVATE` -- it uses a different (less robust) focus management approach
- Our rebuilt version improves on the original by using `WS_EX_NOACTIVATE` which is the correct Win32 mechanism

---

## Summary of Win32 Concepts Used

| Concept | What it does | Where used |
|---------|-------------|------------|
| `WS_EX_NOACTIVATE` | Prevents window from ever becoming foreground | OverlayWindow::Create and all style methods |
| `WS_EX_TOPMOST` | Keeps window above all non-topmost windows | OverlayWindow::Create |
| `WS_EX_TOOLWINDOW` | Hides from taskbar/Alt-Tab | OverlayWindow::Create |
| `WH_KEYBOARD_LL` | System-wide low-level keyboard hook | Installed in Create(), callback eats keys when targeting WebView |
| `WH_MOUSE_LL` | System-wide low-level mouse hook | Installed in Create(), callback sets keyboard target |
| `MA_NOACTIVATE` | Response to WM_MOUSEACTIVATE preventing activation | HandleMessage WM_MOUSEACTIVATE case |
| `SWP_NOACTIVATE` | Prevents activation during window position changes | HandleMessage WM_WINDOWPOSCHANGING case |
| `SetFocus(nullptr)` | Strips keyboard focus from calling thread | Defensive backstop in WM_ACTIVATE/WM_SETFOCUS |
| `CallDevToolsProtocolMethod` | WebView2 API to invoke Chrome DevTools Protocol | WebViewManager::InjectText, InjectKey |
| `Input.insertText` (CDP) | Inserts text at focused element without Win32 focus | (superseded — see REA findings below) |
| `Input.dispatchKeyEvent` (CDP) | Dispatches key events without Win32 focus | For Enter, Backspace, arrows, etc. |

---

## REA Findings (2026-10-31): parity fixes for locked-down / topmost apps

Reverse-engineered from `MYOWNAPP_unpacked.exe` (Ghidra via REA). Manifest is
`level="asInvoker" uiAccess="false"` — the original is **not** elevated, so none
of these behaviors depend on privilege. Three low-level techniques the original
uses that the rebuild originally lacked:

### 1. Screen-capture invisibility — `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`

**This is the single most important stealth mechanism.** Right after creating its
window the original calls (observed at `0x140030a30`):

```c
SetWindowDisplayAffinity(hwnd, 0x11 /* WDA_EXCLUDEFROMCAPTURE */);
ShowWindow(hwnd, SW_SHOWNOACTIVATE /* 4 */);
```

`WDA_EXCLUDEFROMCAPTURE` (0x11, Windows 10 2004+) is enforced by DWM and removes
the window from **every** capture path — GDI `BitBlt`, `PrintWindow`, DXGI
Desktop Duplication, and `Windows.Graphics.Capture`. The window stays fully
visible to the local user, but any screen recorder / screen-share / proctoring
tool (and the app's own screenshot) captures the content *behind* it. It is a
pure capture-visibility flag — it does **not** affect focus or activation, so the
TOP-PRIORITY dual-focus invariant is untouched.

- **Rebuild fix:** `OverlayWindow::ApplyCaptureProtection()` sets
  `WDA_EXCLUDEFROMCAPTURE` (fallback `WDA_MONITOR` on older Windows), called from
  `Create()` right after `CreateWindowExW` and re-asserted in `Show()`. The
  top-level overlay's affinity covers its WebView2 child content (children
  compose into the parent's redirection surface).
- Because the overlay is excluded from capture, our own DXGI screenshot naturally
  shows the app behind the overlay — exactly what gets pasted into ChatGPT.
- **`app_hook.dll` is injected into the *target* app (not our overlay).**
  It temporarily clears the target's WDA during capture so our DXGI/GDI screenshot
  gets real content instead of black. It uses direct `SetWindowDisplayAffinity`
  calls (no MinHook — see "Hook DLL" section below for full details and the v14
  bugs that were fixed). Basic capture-invisibility of our own overlay does not
  require it — it's only needed when the *target* app has WDA protection.

### 2. Screenshot capture — DXGI Desktop Duplication (not GDI alone)

The original captures via **DXGI Desktop Duplication** (`D3D11CreateDevice` →
`IDXGIDevice` → `GetAdapter` → `EnumOutputs(0)` → `IDXGIOutput1` →
`DuplicateOutput` → `AcquireNextFrame` 500 ms/10 retries skipping empty frames →
staging `CreateTexture2D(CPU_READ)` → `CopyResource` → `Map` → GDI+ → PNG), with
**GDI `BitBlt` as fallback**. Handles `B8G8R8A8` and `R10G10B10A2`. Plain GDI
`BitBlt` (what the rebuild had) returns the empty desktop for GPU/DWM-composited
or protected foreground windows. **Rebuild fix:** `ScreenCapture::CaptureViaDXGI()`
(DXGI first, GDI fallback) in `CaptureVirtualScreen`.

### 3. Typing into the overlay over a topmost/full-screen app

Two causes, both fixed:
- **WebView2 occlusion:** the original creates the WebView2 env with
  `--disable-features=…CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,msWebView2BrowserHitTransparent…`
  (full original also adds `--no-sandbox --in-process-gpu --disable-win32k-lockdown`).
  By default Chromium marks the renderer *occluded* when another window covers the
  overlay and then **drops injected input and pauses painting** — so keystrokes
  vanish under a topmost/full-screen exam window. The rebuild passed `nullptr`
  options. **Fix:** `WebViewManager::Initialize` now passes
  `AdditionalBrowserArguments` disabling occlusion/throttling (sandbox-weakening
  flags intentionally omitted).
- **Injection method:** the original types via JS in the page
  (`document.activeElement`/composer `querySelector` → `el.focus()` →
  `execCommand('insertText')` → `InputEvent`), which is focus/occlusion-
  independent, rather than CDP `Input.insertText` (dropped when the WebView is not
  the focus owner). **Fix:** `WebViewManager::InjectText` rewritten to that JS.

### 4. Clipboard — Ctrl+C / Ctrl+V / Ctrl+X / Ctrl+A

**Correction (v10):** v9's notes said the original has no clipboard-related
imports. That was wrong. Its import table includes `GlobalLock`/`GlobalUnlock`,
`AttachThreadInput`, `SetFocus`, `SetForegroundWindow` and `GetFocus`. The
original's clipboard JS is a set of fragments that native code puts together
(UTF-16 strings around file offset 0x9e728–0x9e906):

```js
el.select();                                   // inputs
let range = document.createRange(); range.selectNodeContents(el);
let sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(range);
try { document.execCommand('copy');  } catch(e) {}
try { document.execCommand('paste'); } catch(e) {}
```

**Why we can't copy this:** Chromium honours `execCommand('copy'|'paste')` and
`navigator.clipboard` only when the page's document has focus / a user gesture.
The original can arrange that because it can give the WebView real focus
(AttachThreadInput/SetFocus). Our overlay must **never** take Win32 focus
(TOP-PRIORITY invariant), so in v9 these calls silently did nothing.

**Rebuild (v10) — native clipboard + focus-free JS:**
- Routing (unchanged since v9): `OverlayWindow::LowLevelKeyboardProc` eats a
  **plain** Ctrl+C/V/X/A only when `s_keyTargetWebView_` (never
  Ctrl+Shift/Alt/Win, so global hotkeys still fire) → `WM_APP_CLIP_CMD` →
  `WebViewManager::ClipboardCommand(op)`. When the WebView is not the target
  the chord falls through, so the desktop app's own Ctrl+C/V still works.
- **C / X:** `ExecuteScript` returns the selection (input/textarea
  `selectionStart..End`, else `getSelection().toString()`) as base64(UTF-8);
  the callback writes it with `OpenClipboard`/`SetClipboardData(CF_UNICODETEXT)`
  (LF→CRLF). X then deletes the selection (`setRangeText('')` /
  `execCommand('delete')`).
- **V:** `GetClipboardData(CF_UNICODETEXT)` (CRLF→LF) → `PasteTextIntoPage`:
  `setRangeText` for inputs/textareas; for rich editors (ProseMirror/Quill) a
  synthetic `paste` ClipboardEvent carrying a `DataTransfer` (same technique as
  screenshot paste), falling back to `execCommand('insertText')`. If the
  clipboard holds only an image (`CF_BITMAP`), it's encoded to PNG
  (`ScreenCapture::HBitmapToPng`) and sent through `SendScreenshot`.
- **A:** `el.select()` for inputs, else `execCommand('selectAll')`.
- No focus/activation change — invariant safe.

### Other (not yet ported) — separate-desktop handling

The original also has `RelaunchOnDesktop` / `--desktop-instance` /
`SweepDesktopProcesses` / `LaunchAndDockEdge` to relaunch itself onto the active
input desktop for lockdown apps that run on an isolated Windows desktop. Not
needed when the overlay is already visible on the same desktop; port later if an
app hides the overlay entirely.
