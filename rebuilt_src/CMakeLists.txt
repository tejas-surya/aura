cmake_minimum_required(VERSION 3.20)
project(DesktopWidget VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find WebView2 NuGet package
find_path(WEBVIEW2_INCLUDE_DIR
    NAMES WebView2.h
    PATHS "${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2.1.0.2478.35/build/native/include"
          "$ENV{NUGET_PACKAGES}/microsoft.web.webview2/1.0.2478.35/build/native/include"
)

find_library(WEBVIEW2_LOADER_LIB
    NAMES WebView2Loader WebView2LoaderStatic
    PATHS "${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2.1.0.2478.35/build/native/x64"
          "$ENV{NUGET_PACKAGES}/microsoft.web.webview2/1.0.2478.35/build/native/x64"
)

# nlohmann/json header-only
find_path(NLOHMANN_JSON_INCLUDE_DIR
    NAMES nlohmann/json.hpp
    PATHS "${CMAKE_SOURCE_DIR}/packages/nlohmann_json/include"
          "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
)

# Main executable
add_executable(DesktopWidget WIN32
    src/main.cpp
    src/app/AppController.cpp
    src/app/ConfigManager.cpp
    src/app/StateManager.cpp
    src/app/RegistrySetup.cpp
    src/app/DesktopWatcher.cpp
    src/ui/OverlayWindow.cpp
    src/ui/ViewportManager.cpp
    src/browser/WebViewManager.cpp
    src/browser/WebBridgeEvents.cpp
    src/input/HotkeyManager.cpp
    src/input/TypingSimulator.cpp
    src/automation/ScreenCapture.cpp
    src/automation/TextExtractor.cpp
    src/crypto/CryptoUtils.cpp
    res/app.rc
)

target_include_directories(DesktopWidget PRIVATE
    src
    hook_dll
    ${WEBVIEW2_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)

target_link_libraries(DesktopWidget PRIVATE
    ${WEBVIEW2_LOADER_LIB}
    user32 gdi32 gdiplus ole32 oleaut32 shell32
    dwmapi d3d11 dxgi
    advapi32 crypt32 bcrypt
    winhttp wintrust ws2_32
    uiautomationcore shlwapi comctl32
)

target_compile_definitions(DesktopWidget PRIVATE
    UNICODE _UNICODE
    NOMINMAX
)

if(MSVC)
    target_compile_options(DesktopWidget PRIVATE /W4 /utf-8)
    target_link_options(DesktopWidget PRIVATE /MANIFEST:NO)
else()
    target_compile_options(DesktopWidget PRIVATE -Wall -Wextra)
endif()

# Hook DLL (MinGW or MSVC)
add_subdirectory(hook_dll)
