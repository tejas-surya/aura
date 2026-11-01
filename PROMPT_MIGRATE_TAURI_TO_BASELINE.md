# Master Implementation & Migration Prompt: Tauri+React to Native Stealth Baseline Architecture

You are an expert Systems Architect, C++ Win32 Systems Programmer, and Reverse Engineering Specialist.
Your mission is to analyze two codebases on this machine:
1. **The Native Baseline Architecture**: `C:\Users\LENOVO\Desktop\REA2` (`MYOWNAPP.exe`, `rebuilt_src`, `app_hook.dll`, `debug_log.enc`, `settings.json`).
2. **The High-Level Application**: `C:\Users\LENOVO\Desktop\master` (A Tauri v2 + React 18 + TypeScript desktop stealth co-pilot called `RuntimeBroker`).

Your objective is to migrate all features and the modern React frontend of `C:\Users\LENOVO\Desktop\master` into the exact low-level Win32 baseline architecture of `C:\Users\LENOVO\Desktop\REA2`, ensuring that the resulting `.exe` **runs strictly under Windows Task Manager's "Background processes" category (not under "Apps")**, with zero taskbar/Alt+Tab presence, full anti-detection display affinity spoofing via `app_hook.dll`, encrypted telemetry (`debug_log.enc`), and focus-neutral interaction.

---

## 1. Deep Architectural Breakdown: How the Baseline (`MYOWNAPP`) Works

Through complete reverse engineering of `MYOWNAPP.exe` and its codebase in `C:\Users\LENOVO\Desktop\REA2`, the mechanism that allows it to operate invisibly as a background process while rendering on screen consists of the following foundational pillars:

### A. Windows Task Manager "Background Process" Classification
Windows Task Manager groups processes into **"Apps"** and **"Background processes"** using strict Win32 heuristic criteria:
- **Apps Category**: Any process owning a visible top-level window eligible for the Windows Taskbar and Alt+Tab switcher (`WS_EX_APPWINDOW` or default overlapped window styling).
- **Background Processes Category**: Any process that has **no Taskbar-eligible windows**. 
In `MYOWNAPP`:
```cpp
// Win32 Extended Window Styles in OverlayWindow.cpp
DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
DWORD style = WS_POPUP | WS_CLIPCHILDREN;

HWND hwnd = CreateWindowExW(
    exStyle, kClassName, L"RuntimeBroker",
    style, x, y, width, height,
    nullptr, nullptr, hInstance, this
);
```
- `WS_EX_TOOLWINDOW`: Explicitly instructs the Windows Shell to omit the window from the Taskbar and Alt+Tab list.
- **Absence of `WS_EX_APPWINDOW`**: Ensures Windows never forces a taskbar entry.
- `WS_POPUP`: Removes standard titlebar and borders.
- **Result**: Even though the window is actively floating on the screen, Windows Task Manager classifies the executable under **"Background processes"**.

---

### B. The Anti-Detection & Screen Protection Companion DLL (`app_hook.dll`)
`MYOWNAPP` compiles a companion dynamic library (`rebuilt_src/hook_dll/HookMain.cpp`) that utilizes MinHook to intercept graphics and display affinity APIs:
1. **Hooked APIs**:
   - `SetWindowDisplayAffinity`: Intercepted for tracked windows. Even if a monitoring tool checks or attempts to alter window protection, `app_hook.dll` reports successful spoofed states.
   - `GetWindowDisplayAffinity`: Returns `0` (unprotected) to querying proctoring/monitoring tools while real capture protection is actively enforced.
   - `PrintWindow` & `BitBlt`: Intercepted so that when third-party screen recorders or proctoring software attempt to capture the region occupied by the overlay window, the hook renders clean background or pitch black, scrubbing the overlay from recordings.
   - `ChildWindowFromPointEx`: Spoofs window hierarchy so window inspection tools believe they hit the parent window below.
2. **Shared Memory IPC (`SharedIPC.h`)**:
   - Communicates between the main `.exe` and `app_hook.dll` using a named shared memory map (`Local\MYOWNAPP_SharedHookMap`) guarded by a mutex.

---

### C. Encrypted Telemetry & State (`debug_log.enc` & `settings.json`)
1. **`debug_log.enc`**:
   - The application never writes plaintext log files to disk.
   - All runtime events, hook triggers, and exceptions are encrypted via Windows CryptoAPI (`CryptAcquireContextW`, `MS_ENH_RSA_AES_PROV_W`, AES-CBC or XOR session cipher) and appended to `debug_log.enc` under mutex `kEncLogMutex`.
2. **`settings.json` & `shortcuts.txt`**:
   - Decoupled configuration for dimensions, opacity, typing delay, hotkeys, and API URLs.

---

### D. Stealth HUD Dual-Focus & Focus-Neutrality
1. **Focus Stealing Prevention**:
   - `WM_MOUSEACTIVATE` returns `MA_NOACTIVATE`.
   - `WM_WINDOWPOSCHANGING` forces `flags |= SWP_NOACTIVATE` (`0x10`).
   - `AttachThreadInput(GetCurrentThreadId(), GetWindowThreadProcessId(hForeground, NULL), TRUE)`: Synchronizes input threads so user interaction with the overlay does not de-activate or strip the caret from the underlying editor (e.g., VS Code or testing environment).

---

### E. Embedded WebView2 Engine
- Houses the modern web interface inside the native Win32 window via Microsoft Edge WebView2 (`CreateCoreWebView2EnvironmentWithOptions`).
- Serves the frontend directly with dark/transparent background configuration and programmatic focus management.

---

## 2. Feature Inventory of `C:\Users\LENOVO\Desktop\master` (Tauri+React App)

Inspect `C:\Users\LENOVO\Desktop\master` to identify all features that must be preserved:
1. **React 18 Frontend (`/src`)**:
   - Floating pill HUD (600x54) centered at top, dynamically expanding downwards.
   - Modern TailwindCSS styling, dark mode, collapsible tabs (Coding, MCQ, Voice, Settings).
   - Markdown rendering with KaTeX math and syntax-highlighted code blocks.
2. **System Audio Loopback & Mic Capture (`/src-tauri/src/speaker`)**:
   - Windows WASAPI loopback capture: captures internal system audio (interviewer/meeting audio).
   - Microphone input capture.
   - Silero VAD (Voice Activity Detection) logic triggering speech-to-text.
3. **Screen & Region Capture (`/src-tauri/src/capture.rs`)**:
   - Fullscreen and multi-monitor capture using GDI / `xcap`.
   - Snip/region selection overlay.
4. **AI Provider Pipeline (`/src-tauri/src/api.rs`)**:
   - Multi-provider support (OpenAI, Anthropic Claude, Gemini, Groq, Whisper STT).
   - Streaming responses with chunked token processing.
5. **SQLite Persistence (`/src-tauri/src/db`)**:
   - Migrations and schema for session history, prompts, API keys, and settings.
6. **Global Shortcuts Suite (`/src-tauri/src/shortcuts.rs`)**:
   - Hide/Show pill, trigger capture, mute/unmute, emergency panic dismiss.

---

## 3. Migration Pathways (Choose the Optimal Strategy)

You have two architectural options to achieve the goal:

### Strategy A (Recommended: Pure C++ Native Engine with Embedded React Frontend)
Completely replace Tauri's binary wrapper with the baseline C++ architecture:
1. **Frontend**: Build the React application in `C:\Users\LENOVO\Desktop\master` (`npm run build` -> `dist/`).
2. **Native Win32 Shell**: Use the baseline C++ engine (`rebuilt_src`), configuring WebView2 to serve the `dist/` bundle via `SetVirtualHostNameToFolderMapping(L"app.local", distPath, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW)`.
3. **Bridge Layer**: Replace `invoke()` calls from Tauri with `window.chrome.webview.postMessage()` and `window.chrome.webview.addEventListener('message', ...)`.
4. **Backend Implementation**:
   - Port WASAPI audio loopback and GDI screen capture into clean C++ modules.
   - Implement HTTP streaming to AI providers using WinHTTP or `libcurl`.
   - Embed SQLite using standard amalgamation (`sqlite3.c` / `sqlite3.h`).
5. **Compiled Binaries**: `RuntimeBroker.exe` + `app_hook.dll` + `settings.json` + `debug_log.enc`.

### Strategy B (Tauri Native Win32 Hardening)
Retain Tauri v2, but deeply patch `src-tauri/src/main.rs` and `src-tauri/src/window.rs`:
1. Use `windows-sys` to retrieve the window's `HWND`:
   ```rust
   let hwnd = window.hwnd().unwrap().0 as HWND;
   let mut ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
   ex_style |= WS_EX_TOOLWINDOW as isize;
   ex_style &= !(WS_EX_APPWINDOW as isize);
   SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
   ```
2. Build and inject `app_hook.dll` at process startup via `LoadLibraryW`.
3. Port the `debug_log.enc` encrypted logger to Rust.

---

## 4. Step-by-Step Execution Plan

### Step 1: Detailed Codebase Inspection
- Read `C:\Users\LENOVO\Desktop\master\src-tauri\src\window.rs`, `api.rs`, `capture.rs`, and `speaker\windows.rs`.
- Read `C:\Users\LENOVO\Desktop\REA2\rebuilt_src\src\ui\OverlayWindow.cpp`, `hook_dll\HookMain.cpp`, and `CMakeLists.txt`.

### Step 2: Establish the Native Build Environment
- Set up a Visual Studio 2026 / MSBuild / CMake project structure with dependencies:
  - Microsoft Edge WebView2 SDK
  - MinHook (for `app_hook.dll`)
  - nlohmann/json
  - SQLite3
  - Windows CryptoAPI (`advapi32.lib`, `crypt32.lib`)
  - WASAPI / Core Audio APIs (`ole32.lib`, `mmdevapi.lib`)

### Step 3: Implement the Core Background Process Window
- Implement the exact window creation with `WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE` and `WS_POPUP`.
- Verify in Windows Task Manager that the compiled binary is listed under **"Background processes"**.

### Step 4: Implement the WebView2 React Bridge
- Bundle the React frontend from `master/src` using Vite (`npm run build`).
- Host the `dist/` directory inside WebView2.
- Provide bi-directional message handlers matching the original Tauri command signatures:
  - `start_screen_capture`
  - `start_audio_capture` / `stop_audio_capture`
  - `send_ai_query` (with streaming token callbacks)
  - `get_history` / `save_settings`

### Step 5: Integrate `app_hook.dll` and `debug_log.enc`
- Compile `app_hook.dll` with MinHook to spoof `GetWindowDisplayAffinity` and block `PrintWindow`/`BitBlt` capture of the overlay.
- Implement encrypted logging to `debug_log.enc`.

### Step 6: Verification & Handover
- Verify that launching the executable displays the React UI, supports typing and interactions, runs in the background process category in Task Manager, and leaves no taskbar icon.
- Provide a `build.bat` script for one-click compilation.

