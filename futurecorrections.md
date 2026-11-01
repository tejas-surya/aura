# Future Corrections — Hard Detection Vectors

> Reverse-engineered from `MYOWNAPP.exe` (unpacked, SHA-256
> `15a14a617dd74705018b2e26c0c8818c65f02e66e4d55f21dd4acbaf4b5865ce`)
> using Ghidra 12.1.4 via REA.
>
> **Key finding: the original has the SAME detection vulnerabilities as our
> rebuild for every item below. It uses no stealth techniques. These are
> genuinely hard problems that even the original developer did not solve.**

---

## Vector #8 — CreateRemoteThread Injection

### What the original does (FUN_140040280 — "InjectIntoPid")

```
OpenProcess(0x103a, FALSE, pid)
  │  0x103a = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
  │           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ
  ├─ ProcessIdToSessionId → skip session 0 (system services)
  ├─ IsWow64Process → skip 32-bit processes
  ├─ K32EnumProcessModulesEx + K32GetModuleBaseNameW
  │    → check if DLL already loaded (smarter than PID-set tracking)
  ├─ VirtualAllocEx(MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
  ├─ WriteProcessMemory(DLL path string)
  ├─ GetProcAddress(GetModuleHandleW("kernel32"), "LoadLibraryW")
  ├─ CreateRemoteThread(lpStartAddress = LoadLibraryW, lpParameter = remoteBuf)
  ├─ WaitForSingleObject(hThread, 10000)
  ├─ GetExitCodeThread → verify DLL loaded
  └─ VirtualFreeEx + CloseHandle
```

No `NtCreateThreadEx`, no APC injection, no thread hijacking, no manual
mapping. Textbook `CreateRemoteThread` + `LoadLibraryW`.

### How a target detects this

| Method | Detail |
|--------|--------|
| **ETW `Microsoft-Windows-Threat-Intelligence`** | Logs every `CreateRemoteThread` cross-process call with source/target PIDs. Requires PPL or admin ETW consumer. |
| **`NtQuerySystemInformation(SystemProcessInformation)`** | Target can snapshot threads before/after and notice a new thread whose start address is `LoadLibraryW`. |
| **DLL load notifications** | Target registers `LdrRegisterDllNotification` or a TLS callback; sees an unexpected DLL load. |
| **Module enumeration** | Target calls `EnumProcessModules` on itself periodically; sees `dxgi_assist.dll`. |

### Improvement plan for our rebuild

**Option A — Replace `CreateRemoteThread` with manual map (high effort)**
- Allocate memory in target, write raw DLL image (headers + sections).
- Resolve imports, apply relocations manually.
- Execute `DllMain` via APC (`QueueUserAPC` on an alertable thread) or
  thread hijacking (`SuspendThread` → `SetThreadContext` → `ResumeThread`).
- **Pro:** No `LoadLibrary` call, no `CreateRemoteThread` in ETW log.
  No entry in the target's PEB module list.
- **Con:** Complex, fragile across Windows versions, triggers more
  `PAGE_EXECUTE_READWRITE` allocations which are themselves suspicious.

**Option B — Smarter duplicate checking (medium effort, DO FIRST)**
- Port the original's `K32EnumProcessModulesEx` check: before injecting,
  verify the DLL isn't already loaded. Our rebuild currently uses a PID set
  in `AppController` which drifts if the target restarts.
- Also filter session 0 and 32-bit processes like the original.

**Option C — Reduce privilege scope (low effort)**
- Drop from `0x103a` to minimum: `PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION
  | PROCESS_VM_WRITE` (drop `PROCESS_VM_READ` and `PROCESS_QUERY_INFORMATION`
  unless needed).

---

## Vector #9 — Target Self-Polling WDA (SetWindowDisplayAffinity)

### What the original does (FUN_1400059d0 — "CaptureAndInjectScreenshot")

The capture flow, reconstructed from debug strings:

```
1. "Screenshot request: Triggered CaptureAndInjectScreenshot"
2. Check debounce timer — drop rapid duplicate requests
3. If tracked WDA windows exist:
   a. "Resetting scrub state and signaling hooked processes..."
   b. Signal g_hScrubEvent (named event: Local\DWM_WDA_Mutex)
   c. Injected DLL in target process receives event:
      - SetWindowDisplayAffinity(hwnd, 0)  for each tracked window
      - Sets g_pShared->wdaCleared = TRUE
   d. Main exe polls/waits for wdaCleared
   e. "All WDA windows cleared. Proceeding to capture."
      OR "WARNING! Timeout waiting for all WDA to clear. Proceeding anyway."
4. If no tracked WDA windows:
   "No tracked WDA windows. Proceeding to capture immediately."
5. Capture screen (DXGI Desktop Duplication, fallback to GDI BitBlt)
6. "Capture complete. Resetting scrub event to restore WDA."
   - DLL restores original WDA values
7. Inject screenshot into ChatGPT/Claude via CDP
```

### How a target detects this

| Method | Detail |
|--------|--------|
| **WDA polling timer** | Target calls `GetWindowDisplayAffinity` on a 50–100ms timer. If it ever reads `0` when it previously set a non-zero value, something cleared it. |
| **WDA-change callback** | No official Win32 callback exists, but a target could hook `SetWindowDisplayAffinity` in its own process (IAT hook or inline hook) to detect external callers. |
| **Named event monitoring** | `NtQuerySystemInformation(SystemHandleInformation)` to find the `DWM_WDA_Mutex` or `WinRT_Comp_*` events — their existence alone is a fingerprint. |

### Improvement plan for our rebuild

**Option A — Minimize the scrub window (medium effort, DO FIRST)**
- Overlap the capture with the scrub: start DXGI `AcquireNextFrame` *before*
  signaling the DLL to restore WDA. The frame buffer is already in GPU memory;
  we just need the copy to complete before restore.
- Target: reduce the window from ~500ms (current: up to 400 iterations × 5ms
  sleep in HookMain.cpp) to under 50ms.

**Option B — Use BitBlt with `CAPTUREBLT` instead of WDA scrubbing (high effort)**
- `BitBlt` with the `CAPTUREBLT` flag captures layered windows even through
  WDA. This eliminates the need to scrub WDA at all.
- **Pro:** No WDA manipulation = nothing to detect.
- **Con:** Only works for GDI captures, not DXGI. Quality/performance trade-off.

**Option C — Randomize event/mutex names per session (low effort)**
- Already partially done (renamed to `WinRT_Comp_*`), but the names are static
  at compile time. Generate a random suffix at startup so the names change
  every session. Defeats signature-based scans.

---

## Vector #10 — SendInput `LLKHF_INJECTED` Flag

### What the original does

- Imports `SendInput` at IAT address `0x14010620e`.
- No evidence of any flag masking, `KEYEVENTF_SCANCODE` tricks, or driver-level
  input injection.
- The "Typing Engine" settings UI (embedded HTML) confirms: robotic vs. human
  mode, configurable keystroke delay — but all through plain `SendInput`.

### How a target detects this

| Method | Detail |
|--------|--------|
| **LL keyboard hook flags** | A `WH_KEYBOARD_LL` hook receives `KBDLLHOOKSTRUCT`. Bit 4 (`LLKHF_INJECTED`, value `0x10`) is set by Windows on **every** `SendInput` / `keybd_event` call. Cannot be cleared by the caller. |
| **`GetAsyncKeyState` timing** | `SendInput` events arrive with zero inter-key jitter; human typing has natural variance. Statistical analysis of timing can flag automated input. |

### Improvement plan for our rebuild

**Option A — Use a keyboard filter driver (highest effort, most effective)**
- Write a KMDF `KBDCLASS` upper filter driver that injects keystrokes at the
  device-stack level. These arrive as hardware-originated input with NO
  `LLKHF_INJECTED` flag.
- **Pro:** Completely undetectable by usermode hooks.
- **Con:** Requires driver signing (EV cert + WHQL or test-signing mode).
  Complex deployment. A realistic approach only for the long term.

**Option B — Interception driver (medium effort)**
- Use the open-source `interception` driver (requires test signing or
  self-signed cert + `bcdedit /set testsigning on`).
- Wraps the keyboard class driver and injects keystrokes that appear as
  real hardware input.
- **Pro:** No `LLKHF_INJECTED` flag. Already proven by existing projects.
- **Con:** Requires test signing mode or a signed driver certificate.

**Option C — Stick with CDP injection for our own WebView (no effort)**
- Our rebuild already uses CDP `Input.insertText` / `Input.dispatchKeyEvent`
  for typing into the ChatGPT overlay. This does NOT go through `SendInput`
  and does NOT set `LLKHF_INJECTED`.
- **The original uses `SendInput` to type answers into the target's own
  application (exam textbox etc.)** — that's the path that gets flagged.
- If we never need to type into the target app (only into our overlay),
  this vector is already mitigated. Document this as a known limitation
  for any future "auto-type into external app" feature.

---

## Vector #11 — Low-Level Hooks Detectable

### What the original does

- Imports `SetWindowsHookExW` (0x14010631e), `CallNextHookEx` (0x140105e4e),
  `UnhookWindowsHookEx` (0x14010637e).
- Also uses `SetWinEventHook` (0x1401062a6) for monitoring desktop process
  creation/destruction (the "WinEventHook watcher" in EnsureRegistrySetup).
- Standard `WH_KEYBOARD_LL` + `WH_MOUSE_LL` global hooks.

### How a target detects this

| Method | Detail |
|--------|--------|
| **`NtQuerySystemInformation` hook enumeration** | Undocumented but functional: enumerate the hook chain to find foreign `WH_KEYBOARD_LL` / `WH_MOUSE_LL` hooks. |
| **Module presence in hook DLL** | LL hooks require the installing module to be loaded. `EnumProcessModules` on `csrss.exe` or the hook chain references reveal the DLL. |
| **Timing analysis** | LL hooks add measurable latency to keyboard/mouse events. A target can measure input latency and detect the overhead. |
| **`GetModuleHandleEx` on hook address** | If the target's own LL hook sees the previous hook in the chain, it can resolve the module → identify our process. |

### Improvement plan for our rebuild

**Option A — Raw Input API instead of LL hooks for mouse (medium effort)**
- Replace `WH_MOUSE_LL` with `RegisterRawInputDevices` + `WM_INPUT`.
- Raw input doesn't install a global hook — it's a per-window registration
  that doesn't appear in the hook chain.
- **Only works for mouse**; keyboard still needs LL hook for the
  "intercept and forward to WebView" routing.

**Option B — Reduce hook footprint (low effort, DO FIRST)**
- Install hooks lazily: only when the overlay is visible. Unhook immediately
  when hidden. Minimizes the time window for detection.
- Our current code installs hooks at startup and never removes them until
  exit.

**Option C — Move to a keyboard filter driver (long term)**
- Same as Vector #10 Option A. A KMDF keyboard filter can intercept and
  route keystrokes without any hook chain entry.

---

## Vector #12 — OutputDebugString Messages Visible

### What the original does

- Imports both `OutputDebugStringA` (0x140105614) and `OutputDebugStringW`
  (0x14010562a).
- Debug strings are **verbose**: function names, PID numbers, WDA status,
  DXGI capture steps, injection attempts — essentially a full activity log.
- Examples from the original:
  - `"InjectIntoPid: ATTEMPTING CreateRemoteThread..."`
  - `"[app_hook PID:1234] Scrub: clearing WDA on 2 windows"`
  - `"Capture: DXGI Desktop Duplication capture succeeded."`
  - `"Screenshot request: All WDA windows cleared."`

### How a target detects this

| Method | Detail |
|--------|--------|
| **DebugView / OutputDebugString monitor** | Any process can capture debug output. Our strings reveal exact functionality. |
| **ETW `Microsoft-Windows-Debug-Output` provider** | Captures all `OutputDebugString` calls system-wide. Even without DebugView. |
| **Anti-cheat / proctoring software** | Many proctoring tools run an `OutputDebugString` monitor looking for known injection/hook strings. |

### Improvement plan for our rebuild

**Option A — Compile out debug strings in Release (low effort, DO FIRST)**
- Wrap all `OutputDebugStringA` calls in a `#ifdef _DEBUG` guard or a
  `DbgLog` function that compiles to nothing in Release mode.
- Our `HookMain.cpp` `DbgLog()` function currently calls
  `OutputDebugStringA` unconditionally in all builds.
- Same for `AppController.cpp` debug logging.

**Option B — Replace with encrypted log file (medium effort)**
- Already partially done: `StateManager` writes to `cache.dat` with
  encryption. Route all debug output there instead of `OutputDebugString`.
- Debug strings never appear on the system debug bus.

**Option C — No-op the function entirely in Release (easiest)**
- Make `DbgLog` an empty inline function in Release builds:
  ```cpp
  #ifdef NDEBUG
  static inline void DbgLog(const char*, ...) {}
  #else
  static void DbgLog(const char* fmt, ...) { /* existing code */ }
  #endif
  ```

---

## Vector #13 — SeDebugPrivilege Elevation Logged

### What the original does (FUN_140030fa0)

```c
void EnableDebugPrivilege(void) {
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    HANDLE hProcess = GetCurrentProcess();
    if (OpenProcessToken(hProcess, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &tp.Privileges[0].Luid)) {
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
        }
        CloseHandle(hToken);
    }
}
```

Called from `FUN_14002b290` (the injection setup function, "EnsureRegistrySetup").
Classic textbook implementation. No stealth.

### How a target detects this

| Method | Detail |
|--------|--------|
| **ETW `Microsoft-Windows-Security-Auditing`** | Event ID 4703 "A token right was adjusted" fires when `AdjustTokenPrivileges` enables `SeDebugPrivilege`. |
| **Process privilege enumeration** | `OpenProcessToken` + `GetTokenInformation(TokenPrivileges)` on our PID reveals `SeDebugPrivilege` is enabled. Any process can check this on other processes. |
| **Anti-cheat heuristics** | Many anti-cheat engines flag any non-system process that holds `SeDebugPrivilege`. |

### Improvement plan for our rebuild

**Option A — Only enable when needed, disable immediately after (low effort, DO FIRST)**
- Currently we enable `SeDebugPrivilege` once at startup and leave it on.
- Instead: enable it just before `OpenProcess` for injection, then
  immediately call `AdjustTokenPrivileges` with `SE_PRIVILEGE_REMOVED`
  (not just disabled — **removed**, so it doesn't show up at all).
- Minimizes the window where the privilege is visible.

**Option B — Avoid needing it entirely (medium effort)**
- `SeDebugPrivilege` is only required to open processes we don't own.
- If we limit injection to processes in our own session and integrity
  level, standard `PROCESS_CREATE_THREAD` access works without the
  privilege for many targets.
- Only escalate to `SeDebugPrivilege` if `OpenProcess` fails with
  `ERROR_ACCESS_DENIED`, and remove it immediately after.

**Option C — Use a helper service running as SYSTEM (high effort)**
- A Windows service running as `NT AUTHORITY\SYSTEM` already has
  `SeDebugPrivilege` by default. It can perform the injection on our
  behalf without our main process ever enabling the privilege.
- Our process communicates with the service over named pipe or COM.

---

## Vector #14 — WDA Suspend Window Timing

### What the original does

From the DLL (`HookMain.cpp` equivalent, reconstructed):

```
1. DLL receives scrub event (WaitForSingleObject on "Local\DWM_WDA_Mutex")
2. For each tracked window:
   a. GetWindowDisplayAffinity(hwnd, &currentWda)  — save original
   b. SetWindowDisplayAffinity(hwnd, 0)             — clear protection
3. Set shared memory flag: wdaCleared = TRUE
4. Wait loop: up to 400 iterations × 5ms (= 2 seconds max)
   - Exit early if wdaCleared is reset to FALSE by main process
5. Restore: SetWindowDisplayAffinity(hwnd, originalWda) for each window
6. Set wdaCleared = FALSE
```

The original also has: `"WARNING! g_hScrubEvent is NULL. WDA scrub will
not be pulsed."` — fallback handling for when the event isn't available.

### How a target detects this

| Method | Detail |
|--------|--------|
| **Rapid WDA polling** | A timer at 10–50ms intervals calling `GetWindowDisplayAffinity`. If the value flips from non-zero to zero and back, someone is scrubbing. |
| **Hooking `SetWindowDisplayAffinity` in own process** | The injected DLL calls this API in the target's address space. Target can IAT-hook or inline-hook the function to detect external callers. |
| **Named event existence** | The scrub event name (`Local\WinRT_Comp_FrameSync`) is globally visible via `NtQueryDirectoryObject` on `\BaseNamedObjects`. |
| **Timing correlation** | If the target notices WDA cleared + screen capture activity (e.g., DXGI output duplication) within the same time window, it's a clear signal. |

### Improvement plan for our rebuild

**Option A — Atomic capture without WDA scrub (ideal, highest effort)**
- Use a kernel-mode component or DXGI directly with composition APIs
  that bypass WDA entirely.
- Windows Magnification API (`MagSetWindowFilterList`) can capture
  through WDA in certain scenarios.
- `DwmGetDxSharedSurface` (undocumented) may access the raw surface
  before WDA is applied.
- **Pro:** Eliminates the scrub window entirely.
- **Con:** Undocumented APIs, may break across Windows versions.

**Option B — Minimize scrub duration to sub-frame (medium effort, DO FIRST)**
- Current max: 2000ms (400 × 5ms). Unacceptable.
- Change to:
  1. Pre-acquire DXGI output duplication before signaling scrub.
  2. Signal scrub.
  3. Call `AcquireNextFrame` with a 16ms timeout (one frame).
  4. Immediately signal restore.
- Target: total scrub window < 16ms (one vsync).
- A 10ms polling timer would need to fire in exactly that window to catch it.

**Option C — Per-session randomized event names (low effort)**
- Generate a random GUID suffix for all shared kernel object names at
  startup. Communicate it between main exe and DLL via the shared memory
  segment (which itself uses a randomized name passed as the DLL entry
  parameter or via a small bootstrap shared section).
- Defeats static name scanning. Does not fix the timing issue.

---

## Priority Matrix

| # | Vector | Original Status | Effort | Impact | Priority |
|---|--------|----------------|--------|--------|----------|
| 12 | OutputDebugString | Both vulnerable | **Low** | High | **P0 — Do Now** |
| 13 | SeDebugPrivilege | Both vulnerable | **Low** | High | **P0 — Do Now** |
| 11 | LL Hooks detectable | Both vulnerable | Low–Med | Medium | **P1 — Next** |
| 9 | WDA self-polling | Both vulnerable | Medium | High | **P1 — Next** |
| 14 | WDA suspend timing | Both vulnerable | Medium | High | **P1 — Next** |
| 8 | CreateRemoteThread | Both vulnerable | Medium | Medium | **P2 — Later** |
| 10 | SendInput LLKHF_INJECTED | Both vulnerable | High | Low* | **P3 — Long Term** |

\* Low impact for our current use case because we type into our own
WebView via CDP, not into the target app via SendInput.

---

## Additional Observations from RE

### Original's smarter injection features we should adopt
1. **Module-based duplicate check** — `K32EnumProcessModulesEx` instead of
   PID set tracking. Survives process restarts.
2. **Session 0 filtering** — `ProcessIdToSessionId` skips system services.
3. **32-bit process filtering** — `IsWow64Process` skips incompatible targets.

### Original's fingerprints we already fixed (easy vectors, completed)
- Process name: `MYOWNAPP.exe` → `DesktopWidget.exe`
- DLL name: `app_hook.dll` → `dxgi_assist.dll`
- Window class: custom → `Shell_InputOverlayWnd`
- Kernel objects: `UFOlee_*` / `DWM_Composition_*` → `WinRT_Comp_*`
- WebView2 folder: `UFOlee/edge_profile_` → `Microsoft/WebToolkit/Default`
- Debug log: `debug_log.enc` → `cache.dat`
- All documented in `context.md` Section 9.

### Original strings still present as fingerprints in THEIR binary
- `"app_hook.dll"`, `"app_hook_active_*.dll"`, `"app_hook_loader.exe"`
- `"ufolee-mouse-only-style"`, `"ufolee-throttle-toast"`, `"__ufolee_*"` JS globals
- `"Local\\DWM_WDA_Mutex"`, `"Global\\DWM_WDA_Mutex"`
- Entire settings HTML with `"ufolee.com"` upgrade prompt
