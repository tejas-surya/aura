@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo   MYOWNAPP - Automated Build Script
echo ============================================================
echo.

:: Check for Visual Studio / Build Tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio or Build Tools not found.
    echo Install "Desktop development with C++" workload.
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:: Ensure NuGet packages
if not exist "packages\Microsoft.Web.WebView2.1.0.2478.35" (
    echo [INFO] Downloading WebView2 NuGet package...
    where nuget >nul 2>&1
    if !errorlevel! neq 0 (
        echo [WARN] nuget.exe not on PATH. Download WebView2 NuGet manually or install via vcpkg.
        echo   vcpkg install webview2:x64-windows
    ) else (
        nuget install Microsoft.Web.WebView2 -Version 1.0.2478.35 -OutputDirectory packages
    )
)

if not exist "packages\nlohmann_json" (
    echo [INFO] Downloading nlohmann/json...
    where nuget >nul 2>&1
    if !errorlevel! neq 0 (
        echo [WARN] Install nlohmann-json via vcpkg:
        echo   vcpkg install nlohmann-json:x64-windows
    ) else (
        nuget install nlohmann.json -Version 3.11.3 -OutputDirectory packages
    )
)

:: Build main application with CMake
echo.
echo [BUILD] Configuring CMake...
if not exist "build" mkdir build
cmake -S . -B build -A x64
if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

echo.
echo [BUILD] Building MYOWNAPP_rebuilt.exe...
cmake --build build --config Release --target MYOWNAPP_rebuilt
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo [BUILD] Building app_hook.dll...
cmake --build build --config Release --target app_hook
if %errorlevel% neq 0 (
    echo [ERROR] Hook DLL build failed.
    exit /b 1
)

echo.
echo ============================================================
echo   BUILD SUCCESSFUL
echo   Output: build\Release\MYOWNAPP_rebuilt.exe
echo           build\hook_dll\Release\app_hook.dll
echo ============================================================
echo.
copy /y "build\Release\MYOWNAPP_rebuilt.exe" "..\MYOWNAPP_rebuilt.exe" >nul
if exist "build\hook_dll\Release\app_hook.dll" copy /y "build\hook_dll\Release\app_hook.dll" "..\app_hook_rebuilt.dll" >nul
if exist "..\WebView2Loader.dll" copy /y "..\WebView2Loader.dll" "build\Release\WebView2Loader.dll" >nul
echo Binaries deployed to workspace root and build directory.
