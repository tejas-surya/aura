# MYOWNAPP_rebuilt — Version History

Each build is copied to `exports/MYOWNAPP_rebuilt_v<N>_<YYYYMMDD-HHMMSS>.exe`.
The live copy in the workspace root (`MYOWNAPP_rebuilt.exe`) always matches the
newest entry below.

---

## v22 — 2026-11-02 15:37:48
File: `exports/DesktopWidget_20261102-153748.exe`

**LaunchAndDockEdge — match the original's lockdown approach**: The original
MYOWNAPP.exe does NOT use WebView2 on lockdown desktops at all. It launches
Microsoft Edge as a standalone process and embeds its window. Replicated
this exactly:

1. **Satellite uses standalone Edge, not WebView2**: When `--desktop-instance`,
   skips WebView2 entirely and calls `LaunchAndDockEdge()`.
2. **Edge launched with `--app=<URL>`**: Opens in app mode (no browser chrome).
   Full flags match the original: `--remote-debugging-port=<PORT>
   --remote-allow-origins=* --no-sandbox --test-type --no-first-run
   --no-default-browser-check --disable-features=Translate
   --disable-blink-features=AutomationControlled --force-device-scale-factor=1`.
3. **Free port allocation**: Binds TCP socket to `127.0.0.1:0` to get an
   OS-assigned port for `--remote-debugging-port`, matching original's approach.
   Fallback: port 9222.
4. **Worker thread finds Edge window**: `EnumWindows` polls for 10 seconds
   looking for `Chrome_WidgetWin_1`/`Chrome_WidgetWin_0` belonging to the
   Edge PID.
5. **SetParent embedding**: Edge window is stripped of its chrome
   (WS_CAPTION/WS_THICKFRAME) and embedded as a child of the
   WS_EX_NOACTIVATE overlay, positioned below the toolbar.
6. **Primary (normal desktop) unchanged**: Still uses WebView2 with the
   `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` env var from v21.

---

## v21 — 2026-11-02 15:05:51
File: `exports/DesktopWidget_20261102-150551.exe`

**Match original's WebView2 approach** — reverse-engineered how MYOWNAPP.exe
makes WebView2 work on lockdown desktops and replicated it exactly:

1. **`WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` env var**: The original sets
   browser flags via the environment variable, not `put_AdditionalBrowserArguments`.
   The env var is read by the WebView2 loader DLL itself and inherited by all
   child processes. Set in `main.cpp` before any WebView2 initialization.
2. **Exact original flags**: `--no-sandbox --in-process-gpu
   --force-device-scale-factor=1 --disable-features=RendererCodeIntegrity,
   FlashDeprecation,CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,
   msWebView2BrowserHitTransparent --allow-no-sandbox-job
   --disable-win32k-lockdown`. Key flags we were missing:
   `--allow-no-sandbox-job` (allows browser without job object restriction),
   `--force-device-scale-factor=1`, `--disable-features=RendererCodeIntegrity`.
3. **User data folder next to exe**: Changed from `%LOCALAPPDATA%\Microsoft\WebToolkit`
   to `webview2_data_Default` / `webview2_data_<PID>` next to the executable,
   matching the original's pattern.
4. **Removed `put_AdditionalBrowserArguments`**: The API call was removed since
   the env var approach is superior — passes `nullptr` for options now.

---

## v20 — 2026-11-02 14:32:28
File: `exports/DesktopWidget_20261102-143228.exe`

**Reverted explicit browser path** — v19's `FindBrowserExecutableFolder` found
a wrong-architecture runtime (x86 vs x64 mismatch), causing WebView2 env
creation to fail silently on the normal desktop. Reverted to `nullptr`
(auto-detect). Satellite still uses per-PID data folder + sandbox flags.

---

## v19 — 2026-11-02 14:25:44
File: `exports/DesktopWidget_20261102-142544.exe`

**WebView2 lockdown-desktop initialization fix** — WebView2 was silently failing
to create on lockdown desktops (blank window, no navigation):

1. **Per-PID user data folder**: Satellite instances now use
   `Satellite_<PID>` instead of a shared `Satellite` folder. Crashed v17
   satellites left the folder locked, causing all subsequent satellites to
   fail silently on `CreateCoreWebView2EnvironmentWithOptions`. Primary
   instance cleans up stale `Satellite_*` folders on startup.
2. **Explicit browser path**: Finds the WebView2 runtime at
   `C:\Program Files (x86)\Microsoft\EdgeWebView\Application\<ver>` and
   passes it directly, bypassing registry lookup that can fail on restricted
   desktops.
3. **Restored `--in-process-gpu`**: The original uses this flag — it runs
   GPU work inside our process instead of spawning a separate GPU process
   that can't start on a lockdown desktop. Safe now because DACL protection
   (v18) prevents external TerminateProcess and the crash guard catches
   internal GPU exceptions.
4. **Retry mechanism**: If async environment creation fails, retries up to
   3 times with fresh temp-directory user data folders.

---

## v18 — 2026-11-02 14:11:04
File: `exports/DesktopWidget_20261102-141104.exe`

**Anti-kill + crash fix** — fixes the v17 regression (appear/disappear loop returned):

1. **Process DACL protection**: Denies `PROCESS_TERMINATE` to Everyone via
   `SetSecurityInfo`. Lockdown browsers that call `TerminateProcess` on our
   overlay now get ACCESS_DENIED. The process can still exit normally via
   `PostQuitMessage` (close button).
2. **Removed `--in-process-gpu`**: This flag forced GPU work into the main
   process — if the GPU fails on a restricted lockdown desktop, the entire
   process crashes (explaining the v17 regression). Replaced with
   `--disable-gpu --disable-gpu-compositing` for satellite instances only
   (forces safe software rendering).
3. **Satellite-only sandbox bypass**: `--no-sandbox --disable-win32k-lockdown`
   now only applies to `--desktop-instance` satellites. The primary instance
   on the normal desktop uses standard WebView2 without sandbox bypass, keeping
   its browser processes less visible to lockdown scanners.

---

## v17 — 2026-11-02 13:56:24
File: `exports/DesktopWidget_20261102-135624.exe`

**WebView2 lockdown-desktop initialization fix** — two root causes for the
"window appears but hangs / sites don't load" issue on lockdown desktops:

1. **Sandbox-bypass flags**: Added `--no-sandbox --in-process-gpu
   --disable-win32k-lockdown` to WebView2 browser arguments (matches the
   original, which always passes these). Without them, Chromium's sandbox
   and GPU subprocess can't initialize on a restricted desktop.
2. **Per-instance user data folder**: Desktop-instance satellites now use
   `WebToolkit\Satellite` instead of `WebToolkit\Default`. WebView2 locks
   its user data folder, so the primary and satellite were deadlocking on
   the same profile directory.

---

## v16 — 2026-11-02 13:07:28
File: `exports/DesktopWidget_20261102-130728.exe`

**Lockdown survival — 4 critical fixes** from deep REA analysis of original binary:

1. **WM_DESTROY gate** (CRITICAL): External WM_DESTROY messages are now silently
   eaten unless the user's close button set the `closeRequested_` flag. The original
   never calls PostQuitMessage (zero xrefs confirmed); our rebuild now only calls it
   on the user-initiated close path. This stops the appear/disappear loop where
   lockdown browsers escalated from WM_CLOSE to WM_DESTROY to kill the satellite.
2. **DWM-disguised mutex name**: Single-instance mutex renamed from
   `Local\WinRT_Comp_SessionLock` to `Local\DWM_Composition_Lock` to match the
   original's pattern of impersonating Windows DWM kernel objects.
3. **Crash guard**: `SetUnhandledExceptionFilter` prevents unhandled exceptions from
   killing the process (matches original's structured exception handler).
4. WM_CLOSE gate (already in v15) remains intact.

---

## v15 — 2026-11-01 23:20:13
File: `exports/DesktopWidget_20261101-232013.exe`

**Lockdown browser bypass — 3 new mechanisms** reverse-engineered from original
MYOWNAPP.exe and implemented. The overlay now stays on top even inside lockdown
browsers that create separate desktops or use process isolation.

### Phase 1: AppInit_DLLs System-Wide Injection (P0)
- Writes `dxgi_assist.dll` path to `HKLM\...\Windows\AppInit_DLLs` with
  `LoadAppInit_DLLs=1` and `RequireSignedAppInit_DLLs=0` in both 64-bit and
  32-bit registry views — causes every new GUI process to auto-load the DLL.
- Elevated `--setup <PID>` helper (UAC prompt once at startup) writes registry,
  stays alive monitoring the parent PID, then clears registry on parent exit.
- `--cleanup` helper for manual orphan cleanup.
- Session-unique DLL copy: `dxgi_assist_active_<tickcount>.dll` avoids file locks.
- Stale DLL copies from crashed sessions cleaned on startup.

### Phase 2: Multi-Desktop Relaunch (P0)
- `DesktopWatcher` thread polls `EnumDesktopsW` every 3 seconds for new desktops
  (lockdown browsers create separate Win32 desktops).
- `RelaunchOnDesktop` spawns a satellite instance with `--desktop-instance` flag
  on the new desktop using Explorer's stolen process token (3-tier fallback:
  `CreateProcessWithTokenW` → `CreateProcessAsUserW` → `CreateProcessW`),
  matching the original's token-theft mechanism.
- Satellite instances skip mutex, AppInit setup, and desktop watching.

### Phase 3: Reactive WinEventHook Injection (P1)
- `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` monitors all foreground changes;
  each new PID triggers a worker thread that injects `dxgi_assist.dll`.
- Initial desktop sweep via `EnumWindows` injects into pre-existing processes.
- System path filter (system32, syswow64, winsxs, servicing, systemapps) +
  security process filter (Defender, Bitdefender) added to skip lists.
- Module-based duplicate check now also detects `dxgi_assist_active_*` copies
  (loaded via AppInit_DLLs) to avoid double injection.
- Thread-safe `injectedPids_` via CRITICAL_SECTION.

### Differences from original MYOWNAPP.exe:
- No certificate installation or WinVerifyTrust DLL verification.
- Cleanup via process-handle monitoring (setup helper watches parent PID) instead
  of named events.
- Polling-based desktop detection (3s interval) vs. original's event-driven.
- `std::unordered_set` PID tracking vs. original's balanced BST.
- No admin-check gating on token APIs — just try and fall back.

No focus/activation change — invariant safe (none of these mechanisms touch the
overlay window, CDP input, or keyboard/mouse hooks).

## v14 — 2026-10-31 20:44:03
File: `exports/MYOWNAPP_rebuilt_20261031-204403.exe`

Full `app_hook.dll` process injection + WDA scrub for target apps with capture
protection (REA-confirmed from original `InjectIntoPid` at 0x140040280).

- **What it does:** when the foreground app has `SetWindowDisplayAffinity` capture
  protection (e.g. exam/proctoring software), the screenshot hotkey now injects
  `app_hook.dll` into the target process via `CreateRemoteThread` + `LoadLibraryW`.
  The DLL hooks `SetWindowDisplayAffinity` / `GetWindowDisplayAffinity` via MinHook
  to block re-protection, then temporarily clears WDA on signal so DXGI/GDI capture
  sees the real window content.
- **Shared memory IPC:** main exe creates `Local\DWM_Composition_Data` (SharedHookData
  struct) with overlay HWND, tracked windows, and wdaCleared flag. DLL opens the
  same mapping. `Local\DWM_Composition_Notify` event signals scrub start; main exe
  resets `wdaCleared` after capture to signal restore.
- **Injection flow (matches original):** `OpenProcess` → `VirtualAllocEx` →
  `WriteProcessMemory` (DLL path) → `CreateRemoteThread` (LoadLibraryW) →
  `WaitForSingleObject(5s)`. PID tracking prevents double injection. Skips
  system/browser/self processes (SharedIPC.h ignore lists).
- **Capture flow:** enumerate target's WDA windows → inject if needed → suspend own
  overlay WDA → signal scrub event → poll wdaCleared (500ms) → capture → reset
  wdaCleared (DLL auto-restores) → restore own overlay WDA.
- **DLL scrub timing fix:** replaced fixed `Sleep(100)` with polling loop (up to 2s)
  that waits for main exe to reset `wdaCleared` after capture, preventing premature
  WDA restoration.
- **Build fix:** `app_hook.dll` now deployed to workspace root with correct name
  (was `app_hook_rebuilt.dll`); added `MYOWNAPP_rebuilt.exe` to self-process skip list.
- No focus/activation change — invariant safe.

## v13 — 2026-10-31 20:30:00
File: `exports/MYOWNAPP_rebuilt_20261031-203000.exe`

Screenshot capture fixed: WDA scrub before capture (REA-confirmed from original).

- **Root cause (REA of original `CaptureAndInjectScreenshot` at 0x1400059d0):** the
  original temporarily clears `WDA_EXCLUDEFROMCAPTURE` from its own overlay window
  BEFORE capturing the screen, then restores it after. It does this via:
  (1) zeroing tracked WDA entries in shared memory (`Local\DWM_Composition_Data`),
  (2) signaling `g_hScrubEvent` to tell hooked processes to clear their WDA,
  (3) polling up to 500ms until all WDA windows confirm cleared,
  (4) capturing via DXGI Desktop Duplication (primary) or GDI BitBlt (fallback),
  (5) resetting the event to restore WDA on all windows.
  Our rebuild was setting `WDA_EXCLUDEFROMCAPTURE` but never suspending it before
  capture, so the DWM compositor's excluded-window rendering could interfere.
- **Fix:** `OverlayWindow::SuspendCaptureProtection()` calls
  `SetWindowDisplayAffinity(hwnd, 0)` before capture;
  `RestoreCaptureProtection()` re-applies `WDA_EXCLUDEFROMCAPTURE` after.
  `AppController::CaptureAndSend` now wraps the capture in suspend/restore with
  a 50ms Sleep to let DWM update the composition.
- No focus/activation change — invariant safe.

## v12 — 2026-10-31 19:38:10
File: `exports/MYOWNAPP_rebuilt_20261031-193810.exe`

Clipboard (Ctrl+C/V/X/A) rewritten to match the original's mechanism (REA-confirmed).

- **Root cause of v10 failures:** v10 read the WebView selection via JS, base64-
  encoded it, decoded in C++, and wrote the Win32 clipboard — too many moving
  parts. The original does none of this for copy/cut/select-all.
- **How the original works (keyboard hook 0x140018580):** for copy/cut/select-all
  it posts the **native Ctrl+key sequence** (`WM_KEYDOWN`/`WM_KEYUP` for both
  `VK_CONTROL` and the key char) directly to the WebView's `Chrome_WidgetWin_*`
  child HWND via `PostMessageA`. Chromium's own accelerator table fires, reads
  the DOM selection, and writes the OS clipboard — all handled internally. For
  paste (Ctrl+V) the original intercepts the key and runs a JS snippet via
  `ExecuteScript` (because Chromium's `execCommand('paste')` needs a user
  gesture); we read the clipboard with the Win32 API and inject text via JS.
- **Fix:** `WebViewManager::PostClipboardKeys` finds the `Chrome_WidgetWin_*`
  child via `EnumChildWindows` and posts the 4-message key sequence (Ctrl↓ →
  key↓ → key↑ → Ctrl↑) with correct scan codes from `MapVirtualKeyW`.
  `ClipboardCommand` now calls this for C/X/A. Paste (V) keeps the Win32
  clipboard read → JS insert path (focus-independent).
- No focus/activation change — invariant safe.

## v11 — 2026-10-31 19:18:01
File: `exports/MYOWNAPP_rebuilt_20261031-191801.exe`

Fixes capture protection (WDA_EXCLUDEFROMCAPTURE) not actually working — overlay
was still visible in screen recordings/captures.

- **Root cause (REA-confirmed):** the original creates the window with
  `WS_EX_LAYERED` in the initial `CreateWindowExA` call (exStyle `0x8080088` =
  `TOPMOST | TOOLWINDOW | LAYERED | NOACTIVATE`). Our rebuild omitted
  `WS_EX_LAYERED` at creation time and only added it conditionally when opacity
  < 100%. Without `WS_EX_LAYERED` from creation, DWM composites WebView2's
  DirectComposition surface separately from the parent window, so
  `SetWindowDisplayAffinity` on the parent didn't cover the WebView2 content.
- **Fix:** Window is now always created with `WS_EX_LAYERED` (matching original).
  `SetLayeredWindowAttributes` is always called (alpha = 255 at full opacity).
  `WS_EX_LAYERED` is never removed by `SetOpacity` or `SetClickThrough`.
  `ApplyCaptureProtection` is unconditional (no `WDA_MONITOR` fallback), matching
  the original's single `SetWindowDisplayAffinity(hwnd, 0x11)` call.
- No focus/activation change — invariant safe.

## v10 — 2026-10-31 18:52:07
File: `exports/MYOWNAPP_rebuilt_20261031-185207.exe`

Clipboard (Ctrl+C / Ctrl+V / Ctrl+X / Ctrl+A) re-implemented — v9 did nothing.

- **Why v9 failed:** it ran `document.execCommand('copy'|'paste')` and
  `navigator.clipboard.*` inside the page. Chromium rejects these unless the
  page's document has focus / a user gesture, and our overlay never has Win32
  focus (dual-focus invariant), so every call silently did nothing.
- **Re-check of the original:** its import table includes `AttachThreadInput`,
  `SetFocus`, `SetForegroundWindow`, `GetFocus` and `GlobalLock`/`GlobalUnlock`
  (v9's notes said it had no clipboard-related imports). In-page execCommand
  copy/paste only works when the WebView actually has focus. We must not hand
  focus over, so we don't copy that part of the original.
- **Fix (`WebViewManager::ClipboardCommand`):** the OS clipboard is driven
  **natively** with the Win32 API; only the selection read and text insert run
  as JS (focus-independent).
  - Ctrl+C: JS returns the selection (input/textarea selection range, otherwise
    `getSelection()`) as base64 → `SetClipboardData(CF_UNICODETEXT)` (CRLF).
  - Ctrl+X: same, then deletes the selection (`setRangeText` /
    `execCommand('delete')`).
  - Ctrl+V: `GetClipboardData(CF_UNICODETEXT)` → inserted at the caret
    (`setRangeText` for inputs; synthetic `paste` ClipboardEvent with a
    DataTransfer for rich editors, `insertText` fallback). If the clipboard
    holds an image (CF_BITMAP, e.g. a Win+Shift+S snip), it's encoded to PNG and
    pasted via the screenshot-paste path.
  - Ctrl+A: `el.select()` for inputs, else `execCommand('selectAll')`.
- Keyboard-hook routing unchanged from v9 (plain Ctrl+C/V/X/A eaten only when
  the WebView is the target; Ctrl+Shift/Alt/Win chords pass through).
- No focus/activation change — invariant safe.

## v9 — 2026-10-31 12:27:45
File: `exports/MYOWNAPP_rebuilt_20261031-122745.exe`

Clipboard (Ctrl+C / Ctrl+V / Ctrl+X / Ctrl+A) now works inside the overlay and
across any desktop app, reconstructed from the original.

- **Finding:** the original imports no Win32 clipboard API and uses no
  `navigator.clipboard`; it runs `document.execCommand('copy'|'paste'|'cut'|
  'delete')` inside the WebView (strings at 0x1400a069c / 0x1400a0706 / 0x14009fbce
  with `window.getSelection()` selection setup). The overlay never owns Win32
  focus, so clipboard must be injected as JS; the shared OS clipboard bridges to
  other apps.
- **Fix:** `OverlayWindow::LowLevelKeyboardProc` eats a *plain* Ctrl+C/V/X/A
  (never Ctrl+Shift/Alt/Win — global hotkeys still fire) **only when the WebView
  is the active target**, posting `WM_APP_CLIP_CMD` →
  `WebViewManager::ClipboardCommand()` which runs the matching `execCommand`
  (with a `navigator.clipboard` fallback). Clipboard-read permission auto-granted
  via `add_PermissionRequested`. When the WebView is not the target the chord
  falls through so the desktop app's own Ctrl+C/V keeps working.
- Input-routing + in-page JS only; no focus/activation change (invariant safe).
- Full low-level detail in `context.md` → "REA Findings (2026-10-31)", section 4.

## v8 — 2026-10-31 12:14:33
File: `exports/MYOWNAPP_rebuilt_20261031-121433.exe`

Makes the overlay invisible to screen capture / recording, matching the original.

- **`OverlayWindow::ApplyCaptureProtection()`** calls
  `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE /*0x11*/)` right after
  window creation (and re-asserts it on every `Show()`), falling back to
  `WDA_MONITOR` on pre-2004 Windows. The original does exactly this
  (`SetWindowDisplayAffinity(hwnd, 0x11)` at `0x140030a30`). The window stays
  visible to the local user but is excluded from GDI BitBlt, PrintWindow, DXGI
  Desktop Duplication and Windows.Graphics.Capture — so screen-share / recording
  / proctoring tools (and our own screenshot) see the content behind it, never
  the overlay. Capture-only; does not touch focus/activation (invariant safe).
- Full low-level findings documented in `context.md` → "REA Findings (2026-10-31)".

## v7 — 2026-10-31 12:02:25
File: `exports/MYOWNAPP_rebuilt_20261031-120225.exe`

Fixes both failures seen inside locked-down / topmost foreground apps (e.g. an
exam window), reconstructed from the original `MYOWNAPP.exe` via REA. The
never-lose-focus invariant is untouched (no activation/focus/hook changes).

- **Screenshot now captures the foreground app, not the desktop background.**
  `ScreenCapture::CaptureVirtualScreen` now tries **DXGI Desktop Duplication**
  first (reads the real composited GPU output, so GPU/DWM-composited or protected
  windows are captured) and falls back to the legacy GDI BitBlt. Mirrors the
  original's D3D11/DXGI pipeline (primary output, 500ms/10-retry AcquireNextFrame,
  staging texture + Map → GDI+ → PNG; B8G8R8A8 handled directly, other formats
  fall back to GDI).
- **Typing into the overlay now works under a topmost/full-screen app.** The
  WebView2 environment is created with
  `--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,msWebView2BrowserHitTransparent`
  (previously no options were passed). By default Chromium marks the renderer
  "occluded" when another window covers the overlay and silently drops injected
  input; disabling occlusion detection keeps it live. (Original also passes
  --no-sandbox/--in-process-gpu/--disable-win32k-lockdown — omitted here to avoid
  weakening the sandbox; add for exact parity if needed.)
- **`InjectText` switched from CDP `Input.insertText` to JS injection** (locate +
  focus the composer, `execCommand('insertText')` + `InputEvent`), matching the
  original — focus/occlusion-independent so it lands even when the overlay never
  holds Win32 focus.

## v6 — 2026-10-30 23:21:20
File: `exports/MYOWNAPP_rebuilt_v6_20261030-232120.exe`

Added a real browser chrome (two-row toolbar) and zoom, all while preserving the
never-lose-focus invariant.

- **Browser nav bar (row 2):** Back `‹`, Forward `›`, Refresh `⟳`, and an
  **omnibox** — type a URL to navigate or any text to Google-search it (Enter
  commits, Esc cancels). The omnibox captures keystrokes through the existing
  keyboard hook (no Win32 focus), so the real app still keeps focus; it shows the
  current page URL when idle.
- **Drag grip** at the top-left (row 1) to move the window; row 1 also holds the
  AI tabs + minimize/close.
- **Zoom:** `Ctrl+Shift++` zoom in / `Ctrl+Shift+-` zoom out (ids 26/27), applied
  to the WebView and gated to when the overlay area is active.
- Toolbar height grew to 62px (30 tabs + 32 nav); WebView bounds adjust to match.
- `exports/` kept self-contained (WebView2Loader.dll + app_hook.dll copied in).

## v5 — 2026-10-30 22:44:36
File: `exports/MYOWNAPP_rebuilt_v5_20261030-224436.exe`

Focus model corrected to satisfy BOTH requirements at once (per `context.md`):
the user's real app keeps focus/caret **and** typing into the ChatGPT overlay works.

- Restored the never-lose-focus architecture: overlay is `WS_EX_NOACTIVATE`
  again (re-applied across every ex-style method), `WM_MOUSEACTIVATE` →
  `MA_NOACTIVATE`, and keystrokes reach the WebView through **CDP**
  (`Input.insertText` / `Input.dispatchKeyEvent`) instead of Win32 focus.
- Fixed the Shift/capitals bug in the low-level keyboard hook: **modifier keys
  (Shift/Ctrl/Alt/Win/CapsLock) are no longer eaten**, so `ToUnicode` sees the
  real Shift/Caps state → capital letters and shifted symbols now type.
- Fixed global hotkeys while the WebView has key focus: any **Ctrl/Alt/Win
  chord passes through**, so Ctrl+Shift+H (show/hide), Ctrl+Shift+D
  (click-through), Ctrl+Shift+A, tab switches, etc. fire instead of being
  swallowed.
- Kept from v4: human-typing engine (newline→Enter, cadence, typo simulation),
  visible clickable AI tab strip (ChatGPT/Claude/Gemini/Google), click-through
  layered fix, auto-type-highlighted (Ctrl+Shift+F) into the external editor.

## v4 — 2026-10-30 (pre-versioning, overwritten in root)
- Reworked keyboard to native WebView input + removed `WS_EX_NOACTIVATE`.
  (Superseded by v5: this fixed Shift but broke the "main app keeps focus"
  requirement, so it was reverted.)
- Added visible clickable tabs; `Ctrl+Shift+←/→` switch AI tabs.
- Human typing engine; auto-type highlighted selection.

## v3 and earlier — (pre-versioning)
- Screenshot → AI (full virtual-screen capture, debounce, paste-injection JS).
- Confirmed no licensing/quota in the rebuild.
- Initial 6-feature reconstruction.
