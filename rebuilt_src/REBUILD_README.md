# MYOWNAPP — Rebuilt Source Code Documentation

## System Overview

MYOWNAPP is a stealth overlay application built as a Win32 + WebView2 hybrid. It renders a floating, always-on-top, transparent browser window designed for discrete AI assistant access during any foreground activity. The application consists of:

1. **MYOWNAPP_rebuilt.exe** — The main application
2. **app_hook.dll** — A MinHook-based DLL that hooks Windows Display Affinity (WDA) APIs to make the overlay invisible to screen capture software

### Architecture

```
┌──────────────────────────────────────────┐
│            MYOWNAPP.exe                  │
│  ┌──────────┐  ┌────────────────────┐    │
│  │ AppCtrl  │──│ ConfigManager      │    │
│  │          │  │ (settings.json)    │    │
│  └────┬─────┘  └────────────────────┘    │
│       │                                   │
│  ┌────┴─────┐  ┌────────────────────┐    │
│  │ Overlay  │  │ WebViewManager     │    │
│  │ Window   │──│ (Edge WebView2)    │    │
│  │ (WS_EX_  │  │ chatgpt/google/   │    │
│  │ LAYERED) │  │ claude/gemini     │    │
│  └──────────┘  └────────────────────┘    │
│       │                                   │
│  ┌────┴─────┐  ┌────────────────────┐    │
│  │ Hotkey   │  │ ScreenCapture      │    │
│  │ Manager  │  │ TextExtractor      │    │
│  │ (global) │  │ TypingSimulator    │    │
│  └──────────┘  └────────────────────┘    │
└──────────────────┬───────────────────────┘
                   │ LoadLibrary + SharedMem
         ┌─────────┴──────────┐
         │  app_hook.dll      │
         │  (MinHook engine)  │
         │  Hooks:            │
         │  - SetWindowDA     │
         │  - GetWindowDA     │
         │  - PrintWindow     │
         │  - BitBlt          │
         │  - ChildWindowPt   │
         └────────────────────┘
```

### Core Subsystems

| Subsystem | Files | Purpose |
|-----------|-------|---------|
| **App Core** | `src/app/AppController.*` | Central coordinator for all subsystems |
| **Config** | `src/app/ConfigManager.*` | JSON settings & hotkey persistence |
| **State** | `src/app/StateManager.*` | Auth data (DPAPI) and encrypted debug logs (AES) |
| **Overlay UI** | `src/ui/OverlayWindow.*` | Win32 layered/topmost window with drag, opacity, click-through |
| **Viewport** | `src/ui/ViewportManager.*` | Multi-monitor DPI-aware positioning |
| **Browser** | `src/browser/WebViewManager.*` | Edge WebView2 integration with JS bridge |
| **Bridge** | `src/browser/WebBridgeEvents.*` | Native ↔ JS message dispatch |
| **Hotkeys** | `src/input/HotkeyManager.*` | Global hotkey registration from config |
| **Typing** | `src/input/TypingSimulator.*` | Human-like `SendInput` keystroke simulation |
| **Capture** | `src/automation/ScreenCapture.*` | Window/screen capture via GDI `BitBlt` + GDI+ PNG |
| **Extract** | `src/automation/TextExtractor.*` | UIAutomation COM text scraping |
| **Crypto** | `src/crypto/CryptoUtils.*` | AES-256-CBC encryption + DPAPI |
| **Hook DLL** | `hook_dll/HookMain.cpp` | WDA spoofing, PrintWindow/BitBlt interception |
| **IPC** | `hook_dll/SharedIPC.h` | Shared memory structures between exe and dll |

---

## Directory & File Map

```
rebuilt_src/
├── CMakeLists.txt              # Top-level CMake build (exe + dll)
├── build.bat                   # One-click MSVC build script
├── vcpkg.json                  # Dependency manifest
├── REBUILD_README.md           # This file
├── res/
│   ├── resource.h              # Resource IDs
│   ├── app.rc                  # Version info, icon, manifest
│   └── app.manifest            # DPI awareness, UAC, common controls
├── src/
│   ├── main.cpp                # wWinMain: mutex, COM, GDI+ init, message loop
│   ├── app/
│   │   ├── AppController.*     # Orchestrates all subsystems, hotkey dispatch
│   │   ├── ConfigManager.*     # Reads/writes settings.json
│   │   └── StateManager.*      # auth.dat (DPAPI), debug_log.enc (AES)
│   ├── ui/
│   │   ├── OverlayWindow.*     # WS_EX_TOPMOST|LAYERED|TOOLWINDOW window
│   │   └── ViewportManager.*   # Monitor enumeration, DPI scaling
│   ├── browser/
│   │   ├── WebViewManager.*    # WebView2 lifecycle, navigation, script injection
│   │   └── WebBridgeEvents.*   # JSON message routing
│   ├── input/
│   │   ├── HotkeyManager.*     # RegisterHotKey with config-driven bindings
│   │   └── TypingSimulator.*   # Unicode SendInput with Gaussian jitter
│   ├── automation/
│   │   ├── ScreenCapture.*     # BitBlt → GDI+ Bitmap → PNG bytes
│   │   └── TextExtractor.*     # IUIAutomation recursive text extraction
│   └── crypto/
│       └── CryptoUtils.*       # AES-256-CBC (WinCrypt), DPAPI, random gen
└── hook_dll/
    ├── CMakeLists.txt          # DLL build definition
    ├── HookMain.cpp            # DllMain, hooks, WDA scrubbing, encrypted log
    ├── SharedIPC.h             # Shared memory layout and constants
    └── MinHook/                # MinHook library source (obtain from GitHub)
        ├── MinHook.h
        ├── MinHook.c
        ├── hde64.h / hde64.c
        ├── trampoline.h / trampoline.c
        └── buffer.h / buffer.c
```

---

## Build Instructions

### Prerequisites

- **Visual Studio 2022** (or Build Tools) with "Desktop development with C++" workload
- **CMake 3.20+**
- **WebView2 SDK** (NuGet or vcpkg)
- **nlohmann/json** (NuGet or vcpkg)
- **MinHook** source (https://github.com/TsudaKageworthy/minhook — place in `hook_dll/MinHook/`)

### Option A: build.bat (recommended)

```cmd
cd rebuilt_src
build.bat
```

### Option B: Manual CMake

```cmd
cd rebuilt_src
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### Option C: vcpkg

```cmd
vcpkg install nlohmann-json:x64-windows webview2:x64-windows
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Runtime Dependencies

Copy these next to the built exe:
- `WebView2Loader.dll` (from WebView2 SDK)
- `app_hook.dll` (built by this project)
- `settings.json` (created automatically on first run)

---

## Customization Guide

### Adding or Modifying Hotkeys

Edit `settings.json` → `hotkeys` array. Each entry:
```json
{ "id": 99, "modifiers": 6, "vk": 75, "action": "My custom action" }
```

**Modifier flags**: Alt=1, Ctrl=2, Shift=4, Win=8 (combine by adding: Ctrl+Shift = 6)

**VK codes**: Standard Windows virtual key codes (e.g., A=65, F1=112, etc.)

Then in `src/app/AppController.cpp` → `OnHotkeyAction()`, add a case for your new ID.

### Tweaking Overlay Transparency

- Default opacity: `settings.json` → `"opacity": 100` (range 0–100)
- Opacity step per hotkey: `AppController::AdjustOpacity()` → change the `5` parameter
- Mouse-only fade speed: `OverlayWindow::OnTimer()` → adjust the `10` step value

### Changing Default AI Website URLs

In `src/browser/WebViewManager.cpp` → `ResolveUrl()`:
```cpp
if (siteKey == L"chatgpt")  return L"https://chatgpt.com";
// Add your own:
if (siteKey == L"perplexity") return L"https://perplexity.ai";
```

And add to `siteRotation_` in `WebViewManager.h`.

### Adjusting UIAutomation Scraping

- Max traverse depth: `TextExtractor.h` → `kMaxDepth` (default 30)
- Max children per node: `TextExtractor.cpp` → the `200` limit in `TraverseElement()`
- Control types to extract text from: modify the `controlType ==` checks

### Typing Simulation Speed

- Base delay: `settings.json` → `"typeDelayMs": 200` (milliseconds per character)
- Jitter: `TypingSimulator.cpp` → `CalculateDelay()` uses Gaussian distribution at 30% stddev
- Human mode toggle: `settings.json` → `"humanTyping": true`

### Text-Only Mode CSS

Modify the injected stylesheet in `WebViewManager::InjectTextOnlyMode()` to customize which elements are hidden and how text is styled.

### Window Size and Position

- Default size: `settings.json` → `"width": 350, "height": 500`
- Move step: `AppController::MoveWindow()` → change the `30` pixel step
- Initial position: `OverlayWindow::Create()` → calculated from work area

---

## Reverse Engineering Evidence

This source code was reconstructed from `MYOWNAPP.exe` (SHA-256: `195fe697...`) and `app_hook.dll` (SHA-256: `3be9ab52...`) using:

- **PE Import/Export Analysis**: Complete IAT catalog of both binaries
- **String Extraction**: Wide and ASCII string tables from the uncompressed hook DLL
- **Export Table**: 40 exports from app_hook.dll confirming MinHook + hde64 engine
- **Resource Analysis**: Icon group, manifest, version info
- **Configuration Files**: `settings.json` (hotkey registry) and `shortcuts.txt` (user guide)
- **PE Load Config**: TLS callback presence (UPX unpacking stub)

The main executable was UPX-packed; structural intelligence was derived from its IAT (15 DLLs, 18 import entries) and the rich, uncompressed hook DLL which contained complete debug strings revealing the internal architecture, IPC protocol, and hook targets.
