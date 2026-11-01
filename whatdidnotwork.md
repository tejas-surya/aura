# What Did Not Work — Failed Approaches Log

Every failed approach during the rebuild, what went wrong, and why.

---

## 1. Missing `WS_EX_NOACTIVATE` — Main App Kept Losing Focus

**What we tried:** Created the overlay with `WS_EX_TOPMOST | WS_EX_TOOLWINDOW`
only, relying on `WM_MOUSEACTIVATE → MA_NOACTIVATE` and
`WM_ACTIVATE → SetFocus(nullptr)` to keep the main app in the foreground.

**What happened:** Every click inside the WebView2 area stole focus from the
main app. Its caret stopped blinking and keyboard input went to the overlay
instead of the foreground window.

**Why it failed:**
- `MA_NOACTIVATE` only handles `WM_MOUSEACTIVATE` messages. WebView2's internal
  Chromium code calls `SetFocus()` directly on its child HWND, which bypasses
  `WM_MOUSEACTIVATE` entirely.
- `SetFocus(nullptr)` in `WM_ACTIVATE` runs too late — by the time our handler
  fires, the main app already received `WM_KILLFOCUS` and its caret already
  stopped blinking. Setting focus to null strips keyboard focus from our thread
  but does NOT return foreground status to the main app.
- Without `WS_EX_NOACTIVATE` on the top-level window, Windows allows the window
  to become the foreground window through any activation path.

**What fixed it:** Adding `WS_EX_NOACTIVATE` to the window creation and
re-applying it in every `SetWindowLongPtrW(GWL_EXSTYLE, ...)` call. This
prevents activation at the window-manager level, before any message handler runs.

---

## 2. PostMessage(WM_KEYDOWN/WM_CHAR) to Chromium — Typing Didn't Work

**What we tried:** The keyboard hook captured keystrokes and forwarded them
to the Chromium widget HWND using `PostMessage(WM_KEYDOWN, ...)` and
`PostMessage(WM_CHAR, ...)`.

**What happened:** Text never appeared in the ChatGPT input field. Keystrokes
were silently dropped.

**Why it failed:** Chromium ignores synthetic `WM_KEYDOWN`/`WM_CHAR` messages
when the widget does not have real Win32 focus. This is by design in Chromium's
message pump — it validates the message source and discards messages that arrive
without the widget being the focus owner. Since our overlay has
`WS_EX_NOACTIVATE`, the Chromium widget never gets real Win32 focus.

**What fixed it:** CDP (Chrome DevTools Protocol) injection via
`CallDevToolsProtocolMethod`. CDP talks directly to the Chromium renderer
through an IPC channel, bypassing the Win32 message pump entirely. The renderer
does not check whether its host window has Win32 focus — it just processes the
input event.

---

## 3. CDP `Input.insertText` Dropped Under Topmost/Fullscreen Apps

**What we tried:** Used CDP `Input.insertText` for typing into the ChatGPT
overlay when it was covered by a topmost or fullscreen exam window.

**What happened:** Keystrokes vanished. Text did not appear in the composer.

**Why it failed:** Chromium has an occlusion detection feature. When another
window fully covers the WebView2's host window, Chromium marks the renderer as
"occluded" and then:
- Drops injected input events (including CDP input)
- Pauses painting
- Throttles timers (`IntensiveWakeUpThrottling`)

The default WebView2 creation options do not disable this behavior.

**What fixed it (two-part):**
1. **Disable occlusion detection:** Pass `--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,msWebView2BrowserHitTransparent`
   via `AdditionalBrowserArguments` when creating the WebView2 environment.
2. **Switch from CDP to JS injection:** Replace `Input.insertText` with
   `ExecuteScript` calling `document.activeElement` → `execCommand('insertText')`
   → `InputEvent` dispatching. JS execution via `ExecuteScript` is not gated on
   the renderer's occlusion state the way CDP input events are.

---

## 4. Hook DLL v14 — Three Bugs That Crashed Target Apps

### Bug 4a: MinHook Trampoline Pointers (Silent WDA Failure)

**What we tried:** The injected DLL used a custom MinHook implementation to hook
`SetWindowDisplayAffinity` in the target process. The scrub function called
through the trampoline pointer: `fpSetWDA(hwnd, 0)`.

**What happened:** WDA was never actually cleared. Screenshots captured a black
rectangle instead of real content. No crash, no error — completely silent.

**Why it failed:** When MinHook failed to install the hook (see Bug 4b),
`fpSetWDA` stayed `nullptr`. The code `fpSetWDA ? fpSetWDA(hwnd, 0) : FALSE`
silently evaluated to `FALSE`. The capture proceeded but got an empty frame.

**What fixed it:** Call `SetWindowDisplayAffinity(hwnd, 0)` directly — the real
Win32 API. The DLL runs inside the target process, so it has full rights to call
the API on the target's windows. No trampoline needed.

### Bug 4b: Custom MinHook Corrupted Target Process (Crash)

**What we tried:** Included a simplified single-file MinHook reimplementation
(`hook_dll/MinHook/MinHook.c`) for hooking APIs in the target process.

**What happened:** Target apps crashed on injection. Different crash addresses
each time, always in API functions.

**Why it failed:** The custom MinHook had two fatal flaws on x64:
1. **Broken instruction length decoder** — misidentified instruction boundaries
   at API entry points.
2. **No RIP-relative instruction relocation** — x64 Windows API entry points
   commonly use `[rip+disp32]` addressing in their first 14 bytes. The
   trampoline copied these bytes verbatim without adjusting the displacement.
   Calling through the trampoline executed a `[rip+disp32]` instruction at a
   new address, where the displacement pointed to garbage memory.

**What fixed it:** Removed MinHook entirely. Direct
`SetWindowDisplayAffinity` API calls don't need trampolines. The hook was
originally meant to intercept the target re-setting its own WDA during the
capture window, but direct clear-and-restore is sufficient.

### Bug 4c: EncryptedLog Called from DllMain (Crash/Deadlock)

**What we tried:** The DLL's `DllMain(DLL_PROCESS_ATTACH)` called
`EncryptedLog()` for diagnostic logging.

**What happened:** Target apps either deadlocked (hung forever) or crashed
during DLL injection.

**Why it failed:** `DllMain` runs under the Windows loader lock. Three calls
inside `EncryptedLog` are forbidden under loader lock:

| Call | Why fatal |
|------|-----------|
| `CreateMutexW` + `WaitForSingleObject` | Can deadlock against the loader lock if another thread holds the mutex and is waiting for a DLL load |
| `CryptAcquireContextW` | Loads crypto provider DLLs internally, which re-enters the loader → instant deadlock |
| `CreateFileW(debug_log.enc)` in the target's directory | `GetModuleFileNameW(nullptr)` returns the **target's** exe path, not our DLL's. Writing to e.g. `C:\Program Files\ExamBrowser\` either fails (NTFS permissions) or triggers security software that kills the process |

**What fixed it:** `DllMain` now does nothing except `CreateThread`. All real
work runs on the worker thread (after the loader lock is released). Logging
replaced with `OutputDebugStringA` only — no file I/O, no mutexes, no crypto,
no DLL loads.

---

## 5. `execCommand('copy'|'paste')` in WebView2 Without Focus — Clipboard Broken

**What we tried:** Replicated the original's clipboard JS pattern:
```js
document.execCommand('copy');
document.execCommand('paste');
```

**What happened:** Copy and paste silently did nothing. No errors thrown, but
the clipboard contents didn't change (copy) and no text was inserted (paste).

**Why it failed:** Chromium honours `execCommand('copy'|'paste')` and
`navigator.clipboard` only when the page's document has focus AND there is a
user gesture. The original app can arrange this because it uses
`AttachThreadInput` / `SetFocus` / `SetForegroundWindow` to temporarily give
the WebView real focus. Our overlay must NEVER take Win32 focus (the
top-priority dual-focus invariant), so these `execCommand` calls silently fail.

**What fixed it:** Native clipboard implementation:
- **Copy/Cut:** `ExecuteScript` extracts the selection as text → native
  `OpenClipboard` / `SetClipboardData(CF_UNICODETEXT)` on the C++ side.
- **Paste:** Native `GetClipboardData(CF_UNICODETEXT)` → JS injection via
  `setRangeText` (for inputs/textareas) or synthetic `paste` ClipboardEvent
  with `DataTransfer` (for rich editors like ProseMirror/Quill), falling back
  to `execCommand('insertText')`.
- **Select All:** `el.select()` for inputs, `execCommand('selectAll')` for
  rich editors.

---

## 6. Plain GDI BitBlt for Screen Capture — Black Window

**What we tried:** Used `BitBlt` from the desktop HDC to capture the screen
for pasting into ChatGPT.

**What happened:** Screenshots of GPU-composited, DWM-protected, or
WDA-protected foreground windows returned a solid black rectangle.

**Why it failed:** GDI `BitBlt` captures from the legacy GDI surface, which
does not include content from GPU-composited windows, hardware-accelerated
apps, or windows with `SetWindowDisplayAffinity` protection. DWM composites
these into the final desktop image through a separate pipeline that GDI cannot
access.

**What fixed it:** DXGI Desktop Duplication (`D3D11CreateDevice` →
`IDXGIOutput1::DuplicateOutput` → `AcquireNextFrame` → staging texture →
`Map` → GDI+ → PNG). DXGI captures the composed desktop directly from the
GPU, including GPU-rendered and DWM-composited content. GDI `BitBlt` kept as
a fallback for systems where DXGI is unavailable.

---

## 7. WebView2 `nullptr` Environment Options — Input Dropped Under Topmost Apps

**What we tried:** Created the WebView2 environment with default options
(`nullptr` for `ICoreWebView2EnvironmentOptions`).

**What happened:** When the overlay was behind a topmost/fullscreen exam
window, the WebView2 stopped accepting any input (keyboard via CDP, mouse
clicks) and stopped painting.

**Why it failed:** Default Chromium behavior includes:
- `CalculateNativeWinOcclusion` — marks renderer as occluded when covered
- `IntensiveWakeUpThrottling` — throttles timers in occluded renderers
- `msWebView2BrowserHitTransparent` — WebView2-specific hit-testing that
  can interfere with input routing

These features assume a normal application where an occluded window should
save resources. For an always-on-top overlay that must work even when covered,
they actively break functionality.

**What fixed it:** Pass `AdditionalBrowserArguments` disabling the problematic
features when creating the WebView2 environment. Sandbox-weakening flags
(`--no-sandbox`, `--in-process-gpu`) found in the original were intentionally
omitted.
