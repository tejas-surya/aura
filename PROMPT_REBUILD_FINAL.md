# Autonomous Source Reconstruction & UI Bug Fix Mission

> **Environment Update**: **Ghidra 12.1.4 and Java 21 are now fully installed and configured** on this machine:
> - `GHIDRA_INSTALL_DIR`: `C:\Users\LENOVO\Tools\ghidra_12.1.4_PUBLIC`
> - `JAVA_HOME`: `C:\Users\LENOVO\Tools\jdk-21.0.12.1+1`
> - REA deep analysis providers and decompiler tools (`rea decompile-all`, `rea decompile`, `rea analyze`, `rea trace`) are active and verified.

---

## 1. Problem Statement, Critical Bug Report & Root Cause

The reconstructed/rebuilt binary `MYOWNAPP.exe` was launched by the user with the following observed issues:

1. **Black / Blank Window Body**: The application window frame launches (~350x500 px dark theme), and the top header with control icons (docking, search, settings gear, minimize, maximize, close) appears, but the **entire client area below the title bar is solid pitch black** (see user screenshot).
2. **Invisible / Ghost UI in Some Runs**: In certain configurations, the process appears in Task Manager (`MYOWNAPP.exe` is running), but nothing renders on the desktop (either window opacity is 0, window coordinates are moved off-screen, or `ShowWindow` / message loop hangs).
3. **ROOT CAUSE — The Authentication Gate (`auth.dat`)**:
   - In the original application, the program presented an **ID & Password login gate** (persisted and verified against `auth.dat`), and **only transitioned to loading ChatGPT / Claude once authenticated**.
   - If the authentication gate is half-implemented, waiting for credentials that were forgotten, or failing silently, the WebView2 engine never receives a navigation URL, leaving the client area stuck on a pitch black canvas!

### Critical Requirement: Remove / Bypass Login Entirely
- **DO NOT require any ID or password in the rebuilt app.**
- Completely **strip out and bypass the login/credential barrier**.
- On startup, the rebuilt app must initialize WebView2 and **immediately navigate directly to the AI chat home page (`https://chatgpt.com` or `https://claude.ai`)** without asking for any password, showing a login dialog, or hanging on a black screen.
- Support switching between ChatGPT and Claude using the hotkey `Ctrl + Shift + U` ("Rotate AI Website") or the top toolbar buttons.

---

## 2. Deep Reverse-Engineering with Ghidra & REA

Now that Ghidra 12.1.4 is active, perform targeted decompilation and disassembly on `MYOWNAPP.exe` (and companion `app_hook.dll`) to reanalyze and solve the missing links:

### A. Authentication Flow & WebView2 Navigation Trigger
1. Decompile the state machine in `MYOWNAPP.exe` that references `auth.dat`:
   - Trace how the original app transitioned from the login state to the active browser view.
   - Locate the function that initiates WebView2 navigation (e.g., calling `ICoreWebView2::Navigate(...)` with ChatGPT/Claude URLs).
   - In the rebuilt code, ensure this navigation call is made **immediately upon WebView2 controller initialization**, completely bypassing any authentication check.

### B. WebView2 Initialization & Client Area Hosting
1. Decompile the WebView2 initialization routine (look for calls to `CreateCoreWebView2EnvironmentWithOptions` or `CreateCoreWebView2EnvironmentWithOptionsInternal`):
   - What User Data Folder path is passed to `CreateCoreWebView2EnvironmentWithOptions`? (e.g. `%LOCALAPPDATA%\MYOWNAPP\EBWebView` or current directory).
   - Trace the completion handler callback (`ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler`):
     - How does it invoke `CreateCoreWebView2Controller`?
     - How are controller bounds assigned (`put_Bounds`) relative to the top toolbar height?
     - Is `put_IsVisible(TRUE)` called?
     - Is `put_DefaultBackgroundColor` configured for transparent/dark backgrounds?
   - Trace `WM_SIZE` and `WM_MOVE` in the main window procedure (`WndProc`):
     - Does the window resize the WebView2 controller dynamically? (e.g., `controller->put_Bounds(clientRect)`).
   - Verify URLs: Load `https://chatgpt.com` by default (or `https://claude.ai`), and ensure proper network navigation error handlers are in place.

### C. Titlebar & Custom Header Controls Analysis
1. Inspect how the top icons in the titlebar are implemented:
   - Docking / view modes (`[ ]`), Search icon, Settings gear (`⚙`), Min (`_`), Max/Restore (`▢`), Close (`✕`).
   - Are these rendered natively via GDI/GDI+ in `WM_PAINT`, are they child button windows (`CreateWindowExW(L"BUTTON", ...)`), or are they part of an injected HTML/CSS banner?
   - Trace the click/hit-test handlers (`WM_NCHITTEST`, `WM_COMMAND`, `WM_LBUTTONDOWN`) for these icon buttons.

### D. Window Styling, Layering & Panic-Hide State
1. Decompile window creation and style assignment:
   - Base styles and extended styles (`WS_POPUP`, `WS_VISIBLE`, `WS_EX_TOPMOST`, `WS_EX_LAYERED`, `WS_EX_TOOLWINDOW`, `WS_EX_TRANSPARENT`).
   - Opacity application: Decompile how `settings.json` `"opacity"` (e.g. 100) is translated into Win32 `SetLayeredWindowAttributes(hwnd, 0, (BYTE)(opacity * 255 / 100), LWA_ALPHA)`. Ensure the code does not treat 100 as 100/255 (which renders as very faint/dark) or pass 0.
   - Panic Hide logic: Check if `"panicHidden": true` in `settings.json` causes the window to immediately start off-screen (`x = -32000`) or with `alpha = 0`. Ensure the app defaults to visible and restores on `Ctrl + Shift + H`.

### E. Companion Hook DLL & Shared IPC
1. Decompile `app_hook.dll` functions (`KeyboardProc`, `MouseProc`, exported installer functions):
   - How does `app_hook.dll` share state with `MYOWNAPP.exe`? (Named shared memory section `#pragma data_seg`, registered window message `RegisterWindowMessageW`, or IPC pipe).
   - Decompile "Mouse-Only Mode" (Ctrl + Shift + M): how does mouse hovering reveal the window opacity?

---

## 3. Implementation Requirements: The Working Codebase

Update and synthesize the final, production-ready, clean C++20 / Win32 source code project in `./rebuilt_src/`:

### Core Fixes Required in `rebuilt_src/`:

1. **Direct-to-AI Startup (Zero Authentication Gate)**:
   - **Do not prompt for ID or password.**
   - Skip any login validation dialog or blocking state.
   - On startup, navigate WebView2 directly to `https://chatgpt.com` (or `https://claude.ai`).
   - Implement seamless URL switching (`Ctrl + Shift + U`) between:
     - `https://chatgpt.com`
     - `https://claude.ai`
     - Custom URL from `settings.json` (`customWebsiteUrl`).

2. **Reliable WebView2 Controller Lifecycle**:
   - Ensure `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)` is called on the main UI thread.
   - Pass an explicit, writable local app data directory for WebView2 user data (e.g., `%LOCALAPPDATA%\MYOWNAPP\EBWebView`).
   - In `CreateCoreWebView2EnvironmentCompletedHandler`, immediately call `CreateCoreWebView2Controller(hWnd, ...)` with complete error handling.
   - In `CreateCoreWebView2ControllerCompletedHandler`:
     - Synchronize bounds: calculate `GetClientRect(hWnd, &rc); rc.top += TOP_BAR_HEIGHT;` and call `m_controller->put_Bounds(rc)`.
     - Explicitly call `m_controller->put_IsVisible(TRUE)`.
     - Set default background color to dark `#1e1e1e` or transparent.
     - Call `m_webview->Navigate(L"https://chatgpt.com")` immediately.
     - Add navigation error handlers so that if offline or delayed, it displays a friendly status message rather than staying pitch black.
   - In `WM_SIZE` handler of `WndProc`:
     - If `m_controller` exists, immediately resize the controller bounds to match the new client rect.

3. **Proper Header & Toolbar Rendering**:
   - Accurately draw or layout the top toolbar icons (split view modes, search, settings, minimize, maximize, close).
   - Handle hit testing (`WM_NCHITTEST` or mouse clicks) so window dragging works smoothly by grabbing the top bar.

4. **Correct Layered Window Opacity**:
   - Correctly normalize opacity: `BYTE alpha = (BYTE)((settings.opacity * 255) / 100);`
   - Only add `WS_EX_TRANSPARENT` when Click-Through mode is explicitly toggled on (`Ctrl + Shift + D`).

5. **Complete Hotkey & Hook Engine**:
   - Register all 20+ hotkeys from `shortcuts.txt` and `settings.json`:
     - Show/Hide (`Ctrl + Shift + H`)
     - Panic Hide (`Ctrl + Shift + Backspace`)
     - Text-Only Mode (`Ctrl + Shift + T`)
     - Mouse-Only Mode (`Ctrl + Shift + M`)
     - Click-Through Mode (`Ctrl + Shift + D`)
     - Rotate AI Website (`Ctrl + Shift + U`) -> toggles between ChatGPT and Claude
     - Opacity +/- (`Ctrl + Shift + ,` / `Ctrl + Shift + .`)
     - Move Window (`Ctrl + Shift + Arrows`)
     - Simulated Typing (`Ctrl + Shift + F`)
     - UI Text Extraction (`Ctrl + Shift + E`)
     - Screenshot (`Ctrl + Shift + A`)
   - Implement `app_hook.dll` and compile it alongside the main executable.

6. **Automation Services**:
   - Screen capture via `BitBlt`.
   - UIAutomation text extraction via `IUIAutomation`.
   - Human-like typing simulation via `SendInput` with `typeDelayMs`.

---

## 4. Deliverables & Build System

1. **Compilable Project**: Complete source files inside `./rebuilt_src/` (`src/`, `hook_dll/`, `res/`, `CMakeLists.txt`).
2. **One-Click Build Script (`build.bat`)**:
   - Automatically downloads WebView2 SDK via NuGet / vcpkg or links `WebView2Loader.dll`.
   - Compiles both `MYOWNAPP.exe` and `app_hook.dll` cleanly with MSVC / Visual Studio.
3. **Verification**:
   - Build the project and verify that launching the resulting binary opens the window with the top toolbar AND **the active ChatGPT / Claude web view immediately rendering without any ID/password prompt and without any black screen**.
   - Verify hotkeys respond.
4. **Documentation**: Write a clear `REBUILD_README.md` explaining how the code works and where the user can customize it.
