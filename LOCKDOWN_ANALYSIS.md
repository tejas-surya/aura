# How MYOWNAPP.exe Stays On Top in Lockdown Browsers
## Complete REA Analysis & Implementation Plan for DesktopWidget

---

## Executive Summary

The original `MYOWNAPP.exe` uses **5 complementary mechanisms** to stay visible
on top of lockdown browsers. Our rebuild (`DesktopWidget.exe`) only has 2 of
them. The missing 3 are the *critical* ones that make the difference in lockdown
scenarios.

### What we have vs. what we're missing

| Mechanism | Original | Rebuild | Priority |
|---|---|---|---|
| WS_EX_TOPMOST + SetWindowPos reassertion | YES | YES | - (done) |
| CreateRemoteThread DLL injection | YES | YES | - (done) |
| **AppInit_DLLs system-wide injection** | YES | **NO** | **P0 — Critical** |
| **Multi-desktop relaunch** | YES | **NO** | **P0 — Critical** |
| **Reactive process monitoring (WinEventHook)** | YES | **NO** | **P1 — High** |

---

## Part 1: Detailed Analysis of Original Mechanisms

### Mechanism A: AppInit_DLLs Registry Injection (P0)

**What it does:** Writes the hook DLL path into the Windows registry so that
*every new GUI process* automatically loads the DLL at startup — including
lockdown browsers.

**How it works (decompiled from FUN_140041cd0, called by FUN_140031020):**

```
Registry key: HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows

Values set:
  AppInit_DLLs           = "<path>\app_hook_active_<tickcount>.dll"  (REG_SZ)
  LoadAppInit_DLLs       = 1                                         (REG_DWORD)
  RequireSignedAppInit_DLLs = 0                                      (REG_DWORD)
```

Called TWICE for both registry views:
- `KEY_WOW64_64KEY` (0x100) → native 64-bit registry
- `KEY_WOW64_32KEY` (0x200) → WOW6432Node for 32-bit processes

**Key details:**
- The DLL is a copy: `app_hook_active_<GetTickCount()>.dll` — unique per session
  to avoid file-locking issues
- Requires **admin/UAC** to write HKLM keys — the original uses a `--setup`
  elevated helper process via `ShellExecuteExW("runas")`
- On cleanup, clears `AppInit_DLLs` back to empty string
- This is done in ADDITION to CreateRemoteThread injection, not instead of it

**Why this defeats lockdown browsers:** AppInit_DLLs is a Windows loader
mechanism. Any process that loads `user32.dll` (which is every GUI app)
automatically gets the DLL injected by the OS itself. The lockdown browser
cannot prevent this because it happens during process initialization, before the
browser's own code runs. No inter-process API calls needed — the OS does it.

**Cleanup (from decompiled strings):**
```
"[CLEANUP] SetAppInitRegistryView (...): AppInit_DLLs cleared."
```
The original cleans up the registry on exit to avoid leaving the hook persistent.

### Mechanism B: Multi-Desktop Relaunch (P0)

**What it does:** Lockdown browsers (Safe Exam Browser, Respondus LockDown, etc.)
create a *separate Win32 desktop* to isolate students. Normal TOPMOST windows on
the default desktop are invisible on the lockdown desktop. The original detects
new desktops and relaunches itself onto them.

**How it works (decompiled from FUN_140001860):**

1. **Desktop detection:** Uses `EnumDesktopsW` on the current window station
   (`GetProcessWindowStation`) to enumerate all desktops. Also uses
   `OpenInputDesktop` to detect which desktop is currently receiving input.

2. **Relaunch with Explorer's token (3-tier fallback):**

   ```
   Priority 1: CreateProcessWithTokenW  (Explorer token theft)
     - GetShellWindow() → find Explorer
     - CreateToolhelp32Snapshot → Process32FirstW/NextW → match "explorer.exe"
       in same session (ProcessIdToSessionId)
     - OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
     - OpenProcessToken(TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY)
     - DuplicateTokenEx(MAXIMUM_ALLOWED, SecurityImpersonation, TokenPrimary)
     - CreateProcessWithTokenW(dupToken, ..., cmdline, ..., workDir, ...)

   Priority 2: CreateProcessAsUserW  (same token, different API)

   Priority 3: CreateProcessW  (standard, last resort)
   ```

3. **Command line:** `"<exe_path>" --desktop-instance`
   The `--desktop-instance` flag tells the new copy it's a desktop satellite.

4. **Desktop binding:** Uses `SetThreadDesktop` to bind the new instance's
   threads to the target desktop so its windows appear there.

**Key details:**
- Prepends `"WinSta0\"` to the desktop name for the full station\desktop path
- Checks admin status via `AllocateAndInitializeSid` / `CheckTokenMembership`
  (DOMAIN_ALIAS_RID_ADMINS)
- Explorer token theft lets it run with a non-elevated token on the target
  desktop, avoiding privilege issues

### Mechanism C: Reactive Process Monitoring via WinEventHook (P1)

**What it does:** Monitors the entire system for foreground changes and new window
creation. When any new process gains foreground, immediately injects the hook DLL
into it via CreateRemoteThread.

**How it works (decompiled from FUN_140041bc0 → FUN_140041c80 → FUN_1400400f0 → FUN_140040280):**

```
SetWinEventHook(
    EVENT_SYSTEM_FOREGROUND,  // 0x8000 — fires on foreground change
    EVENT_SYSTEM_FOREGROUND,  // min == max
    NULL,                     // out-of-context callback
    WinEventCallback,         // FUN_140041c80
    0,                        // all processes
    0,                        // all threads
    WINEVENT_OUTOFCONTEXT     // 0x2 — callback in our process
);
```

**Callback chain:**
1. `FUN_140041c80` — fires on every foreground change. Filters to `OBJID_WINDOW`
   (idObject==0), skips own PID.
2. `FUN_1400400f0` — maintains a balanced BST of already-seen PIDs. For each new
   PID, spawns `CreateThread(FUN_140040280, pid)`.
3. `FUN_140040280` — the injection worker:
   - Skips PID 0, self, session 0 processes (`ProcessIdToSessionId`)
   - Opens process: `OpenProcess(0x103a)` — PROCESS_CREATE_THREAD |
     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION
   - Gets exe path: `QueryFullProcessImageNameW`
   - Skips 32-bit: `IsWow64Process` (64-bit injector can't inject 32-bit)
   - **Filter 1** (`FUN_140041380`): Skips system paths — system32, syswow64,
     winsxs, servicing, systemapps
   - **Filter 2** (`FUN_140041a50`): Skips self/browser/security processes —
     UFOlee.exe, stealth_launcher.exe, app_hook_loader.exe, RuntimeBroker.exe,
     bduserhost.exe, bdagent.exe (Bitdefender), MsMpEng.exe, MpCmdRun.exe
     (Defender), msedgewebview2.exe, MicrosoftEdge.exe, msedge.exe,
     CefSharp.BrowserSubprocess.exe, SecurityHealthService.exe
   - **Module duplicate check:** `K32EnumProcessModulesEx` +
     `K32GetModuleBaseNameW` — checks if DLL already loaded ("ALREADY INJECTED")
   - **Injection:** `VirtualAllocEx` → `WriteProcessMemory` (DLL path) →
     `GetProcAddress("LoadLibraryW")` → `CreateRemoteThread` →
     `WaitForSingleObject(5000ms)` → `GetExitCodeThread`

**Additionally, in EnsureRegistrySetup (FUN_14002b290):**
Initial desktop sweep on startup:
```
hwinsta = GetProcessWindowStation();
EnumDesktopsW(hwinsta, DesktopCallback, 0);      // sweep ALL desktops
pDesktop = OpenInputDesktop(0, 0, DESKTOP_READOBJECTS|DESKTOP_ENUMERATE);
EnumDesktopWindows(pDesktop, WindowCallback, 0);  // sweep current desktop
```
This catches processes that were already running before the app started.

### Mechanism D: Topmost Z-Order Enforcement (Already Implemented)

**Timer-based reassertion (FUN_140022f90):**
```c
SetTimer(hwnd, 6, 900000, NULL);  // timer ID 6, every 15 minutes
```
When the timer fires, calls `SetWindowPos(hwnd, HWND_TOPMOST, ...)`.

**10 total SetWindowPos call sites** across the binary — used in window creation,
resize, show/hide, and timer callbacks.

Our rebuild already has `kTopmostTimer = 1003` with periodic reassertion plus a
`WinEventProc` that reasserts on `EVENT_SYSTEM_FOREGROUND`. This is sufficient.

### Mechanism E: Supporting Infrastructure

**UAC Elevation (--setup flag):**
- Main process checks if it has admin rights
- If not, uses `ShellExecuteExW` with `"runas"` verb to spawn `--setup` helper
- The elevated helper writes HKLM registry keys (AppInit_DLLs) and installs certs
- Uses `Global\DWM_Composition_Session` named event as IPC between instances

**Certificate Installation (FUN_140031020):**
- Installs a signing certificate (`ufolee_signing.cer`) into both:
  - `ROOT` certificate store (trusted root CAs)
  - `TrustedPublisher` store (trusted software publishers)
- Verifies `app_hook.dll` with `WinVerifyTrust` before injection
- This is needed because `RequireSignedAppInit_DLLs=0` is set, but the cert is
  installed for future-proofing / other security checks

**SeDebugPrivilege:** Already implemented in our rebuild (JIT elevation in
AppController). Needed for injecting into elevated processes.

---

## Part 2: Implementation Plan for DesktopWidget

### Phase 1: AppInit_DLLs Registry Injection (P0 — Critical)

**Goal:** Auto-load `dxgi_assist.dll` into every new GUI process system-wide.

**New files needed:**
- `src/app/RegistrySetup.cpp` / `.h` — registry manipulation module

**Implementation steps:**

1. **Add `--setup` CLI flag handling** in `main()`:
   - If `--setup` is passed, run elevated registry setup then exit
   - If not admin and AppInit_DLLs not set, spawn self with `--setup` via
     `ShellExecuteExW("runas", ...)`

2. **Implement `SetAppInitRegistry()`:**
   ```
   a. Copy dxgi_assist.dll to a session-unique name:
      dxgi_assist_active_<GetTickCount()>.dll
   b. Open HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows
      with KEY_WOW64_64KEY (and KEY_WOW64_32KEY for 32-bit coverage)
   c. Set AppInit_DLLs = full path to the copied DLL
   d. Set LoadAppInit_DLLs = 1
   e. Set RequireSignedAppInit_DLLs = 0
   ```

3. **Implement `ClearAppInitRegistry()`:**
   - On normal exit, clear `AppInit_DLLs` back to ""
   - Register as atexit handler + handle in WM_DESTROY
   - Also handle crash cleanup (SetUnhandledExceptionFilter)

4. **Guard against stale entries:**
   - On startup, check if AppInit_DLLs already points to a DLL that no longer
     exists (orphaned from a crash). If so, clear it first.

**Risks & mitigations:**
- **AV detection:** AppInit_DLLs modification is flagged by many AVs. The
  original gets away with it because it has a signed DLL + installed cert. We
  don't have a cert. *Mitigation:* Self-sign `dxgi_assist.dll` and install the
  cert (see Phase 3), or accept AV warnings as a tradeoff.
- **Requires admin/UAC:** Users must approve the UAC prompt. *Mitigation:*
  Show a user-friendly explanation before triggering UAC.
- **System stability:** A buggy DLL in AppInit_DLLs can crash every GUI app.
  *Mitigation:* Keep the DLL minimal, add a guard (check shared memory exists
  before doing anything, otherwise just return TRUE from DllMain).

### Phase 2: Multi-Desktop Relaunch (P0 — Critical)

**Goal:** Detect when a lockdown browser creates a new desktop and relaunch
DesktopWidget on it.

**New files needed:**
- `src/app/DesktopWatcher.cpp` / `.h` — desktop detection & relaunch module

**Implementation steps:**

1. **Add `--desktop-instance` CLI flag handling** in `main()`:
   - Satellite instance that skips certain initialization (no UAC prompt, no
     registry setup — just overlay + hooks)
   - Should use `SetThreadDesktop` to bind to the specified desktop

2. **Implement `DesktopWatcher` polling thread:**
   ```
   a. GetProcessWindowStation() → hWinSta
   b. Periodically (every 2-3 seconds):
      - EnumDesktopsW(hWinSta, callback, 0)
      - Compare against known desktop set
      - For each NEW desktop:
        → RelaunchOnDesktop(desktopName)
   ```

3. **Implement `RelaunchOnDesktop(desktopName)`:**
   ```
   a. Build command: "<exe_path>" --desktop-instance
   b. Try CreateProcessWithTokenW (steal Explorer's token):
      - CreateToolhelp32Snapshot → find explorer.exe in same session
      - OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
      - OpenProcessToken → DuplicateTokenEx(MAXIMUM_ALLOWED)
      - CreateProcessWithTokenW with STARTUPINFO.lpDesktop = "WinSta0\<name>"
   c. Fallback: CreateProcessAsUserW (same token)
   d. Fallback: CreateProcessW (standard)
   ```

4. **Desktop-instance behavior:**
   - SetThreadDesktop to the target desktop
   - Create overlay window on that desktop
   - Install hooks scoped to that desktop
   - Skip AppInit_DLLs setup (parent already did it)

**Risks & mitigations:**
- **Token theft detection:** Some security software flags OpenProcessToken on
  Explorer. *Mitigation:* The original does this too and it works; Explorer is a
  standard target for token duplication.
- **Race condition:** Desktop might be destroyed before relaunch completes.
  *Mitigation:* Handle failures gracefully, don't crash.
- **Multiple instances:** Need IPC to coordinate between parent and satellites.
  *Mitigation:* Use named mutex + shared memory (extend existing SharedHookData).

### Phase 3: Reactive WinEventHook Process Monitoring (P1 — High)

**Goal:** Inject the hook DLL into every new foreground process, not just existing
ones at startup.

**Changes needed in existing files:**
- `src/app/AppController.cpp` — add WinEventHook setup
- `src/app/AppController.h` — add hook handle + tracking set

**Implementation steps:**

1. **Add EVENT_SYSTEM_FOREGROUND WinEventHook** in `AppController::Start()`:
   ```
   SetWinEventHook(
       EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
       NULL, WinEventCallback,
       0, 0,  // all processes, all threads
       WINEVENT_OUTOFCONTEXT
   );
   ```

2. **Implement WinEventCallback:**
   ```
   a. Filter: idObject == OBJID_WINDOW only
   b. GetWindowThreadProcessId → get PID
   c. Skip own PID, PID 0
   d. Check PID tracking set (std::unordered_set<DWORD>)
   e. If new PID → spawn worker thread for injection
   ```

3. **Implement process skip filters:**
   - System path filter: skip system32, syswow64, winsxs, servicing, systemapps
   - Self/browser filter: skip DesktopWidget.exe, msedgewebview2.exe, msedge.exe,
     MicrosoftEdge.exe, RuntimeBroker.exe, Windows Defender processes
   - Session 0 filter: ProcessIdToSessionId, skip session 0
   - WoW64 filter: skip 32-bit processes (our DLL is 64-bit)

4. **Add initial desktop sweep** (one-time, on startup):
   ```
   GetProcessWindowStation → EnumDesktopsW → EnumDesktopWindows
   For each window → GetWindowThreadProcessId → inject if new PID
   ```

**Note:** Our rebuild ALREADY has a `WinEventProc` in `OverlayWindow.cpp` but it
only reasserts topmost — it doesn't do injection. This step extends it (or adds
a second hook in AppController) to also trigger DLL injection on foreground change.

### Phase 4: Supporting Infrastructure (P2 — Medium)

1. **DLL Self-Signing + Certificate Installation:**
   - Generate a self-signed code-signing certificate at build time
   - Sign `dxgi_assist.dll` with it
   - In `--setup` elevated helper: install cert to ROOT + TrustedPublisher stores
   - This reduces AV false positives and enables future `RequireSignedAppInit_DLLs=1`

2. **Crash-Safe Registry Cleanup:**
   - SetUnhandledExceptionFilter → clear AppInit_DLLs on crash
   - On startup, check if AppInit_DLLs points to our DLL from a previous
     crashed session → clean up stale entries
   - Named mutex to detect if another instance is still running

3. **Session-Unique DLL Naming:**
   - Copy `dxgi_assist.dll` → `dxgi_assist_active_<tick>.dll` per session
   - Avoids file-lock conflicts when multiple instances run

---

## Part 3: Implementation Order & Dependencies

```
Phase 1 (AppInit_DLLs)     ← Highest impact, defeats lockdown process isolation
    │
    ├── Needs: --setup CLI, UAC elevation, registry read/write
    ├── Needs: Session-unique DLL copy
    └── Needs: Cleanup on exit/crash

Phase 2 (Multi-Desktop)    ← Required for separate-desktop lockdown browsers
    │
    ├── Needs: --desktop-instance CLI
    ├── Needs: Desktop enumeration polling
    ├── Needs: Explorer token theft
    └── Needs: SetThreadDesktop binding

Phase 3 (WinEventHook)     ← Ensures new processes get injected immediately
    │
    ├── Needs: PID tracking set
    ├── Needs: Skip filters (system + self + security)
    └── Can reuse existing injection logic in AppController

Phase 4 (Supporting)        ← Polish & reliability
    │
    ├── DLL signing (reduces AV flags)
    ├── Crash-safe cleanup
    └── Named mutex coordination
```

**Estimated effort:** Phase 1 = ~200 lines, Phase 2 = ~250 lines,
Phase 3 = ~150 lines (extends existing code), Phase 4 = ~100 lines.

---

## Part 4: Key Differences from Original

### What we keep the same
- AppInit_DLLs registry mechanism (exact same keys + values)
- Explorer token theft for desktop relaunch (same API chain)
- WinEventHook for foreground monitoring (same event + callback pattern)
- Skip filters (same process lists, adapted for our binary names)
- Session-unique DLL naming pattern

### What we do differently
- **No certificate signing** initially (Phase 4 adds it later)
- **No `WinVerifyTrust`** verification of our DLL (we trust our own build)
- **Simpler PID tracking** — `std::unordered_set` instead of balanced BST
- **Existing injection pipeline** — reuse `AppController::InjectIntoProcess()`
  instead of reimplementing

### What we already have that the original also has
- `WS_EX_NOACTIVATE` + `WS_EX_TOPMOST` + `WS_EX_TOOLWINDOW` overlay
- SetWindowPos(HWND_TOPMOST) reassertion timer
- EVENT_SYSTEM_FOREGROUND WinEventHook for topmost reassertion
- CreateRemoteThread injection with module-based duplicate check
- SeDebugPrivilege for elevated process injection
- Session 0 / WoW64 skip filters
- SetWindowDisplayAffinity (WDA) scrubbing via injected DLL

---

## ⚠️ Dual-Focus Invariant Check

All proposed changes preserve the dual-focus invariant:

- **Phase 1 (AppInit_DLLs):** Only changes registry + DLL loading. Does not
  touch the overlay window, hooks, or CDP input pipeline.
- **Phase 2 (Multi-Desktop):** Spawns a new *process* on the target desktop.
  That process creates its own overlay with the same WS_EX_NOACTIVATE +
  CDP input model. No change to the input path.
- **Phase 3 (WinEventHook):** Adds injection triggers. Does not modify the
  overlay window or keyboard/mouse hooks.
- **Phase 4 (Supporting):** Infrastructure only.

None of these changes modify `OverlayWindow.cpp`, `WebViewManager.cpp`, the
keyboard/mouse hooks, or the CDP injection path. The invariant is safe.

---

## Part 5: Implementation Differences from Original (as built)

| Aspect | Original MYOWNAPP.exe | Our DesktopWidget |
|---|---|---|
| Certificate install | `ufolee_signing.cer` → ROOT + TrustedPublisher | Skipped — no code signing |
| DLL verification | `WinVerifyTrust` before injection | Skipped — trust our own build |
| Cleanup mechanism | Named-event IPC (unclear) | `--setup` helper monitors parent PID via process handle |
| Desktop detection | May be event-driven | Polling `EnumDesktopsW` every 3s |
| PID tracking | Balanced BST | `std::unordered_set` (O(1) average) |
| Admin gate for token theft | Checks admin before token APIs | Try and fall back |
| Self process names in skip filter | UFOlee.exe, stealth_launcher.exe | DesktopWidget.exe |
| DLL naming pattern | `app_hook_active_<tick>.dll` | `dxgi_assist_active_<tick>.dll` |
| Module duplicate check | Exact name only | Exact name + `dxgi_assist_active_*` prefix |
| WinEventHook event | EVENT_SYSTEM_FOREGROUND (identical) | Same |
| Registry keys + values | Identical HKLM path | Same |
| Registry views | KEY_WOW64_64KEY + 32KEY | Same |
| Explorer token theft | 3-tier fallback | Same |
| `--desktop-instance` CLI flag | Same | Same |
