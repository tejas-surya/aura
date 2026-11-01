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

:: Kill any running instance AND its WebView2 child processes so the new
:: build uses fresh code (WebView2 reuses browser processes from the same
:: user-data folder, so stale processes mean new code never takes effect).
echo.
echo [BUILD] Stopping previous instance and its WebView2 processes...
taskkill /F /IM MYOWNAPP_rebuilt.exe >nul 2>&1
:: Kill WebView2 processes that belong to our profile (command line contains "UFOlee").
:: Uses PowerShell because wmic is deprecated and unreliable on Win11.
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"name='msedgewebview2.exe'\" | Where-Object { $_.CommandLine -match 'UFOlee' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
timeout /t 2 /nobreak >nul 2>&1

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
:: Final kill before deploy - processes may have respawned during the build.
taskkill /F /IM MYOWNAPP_rebuilt.exe >nul 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"name='msedgewebview2.exe'\" | Where-Object { $_.CommandLine -match 'UFOlee' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
copy /y "build\Release\MYOWNAPP_rebuilt.exe" "..\MYOWNAPP_rebuilt.exe" >nul
if exist "build\hook_dll\Release\app_hook.dll" copy /y "build\hook_dll\Release\app_hook.dll" "..\app_hook.dll" >nul
if exist "..\WebView2Loader.dll" copy /y "..\WebView2Loader.dll" "build\Release\WebView2Loader.dll" >nul
echo Binaries deployed to workspace root and build directory.

:: Keep a timestamped copy of every build under ..\exports\
if not exist "..\exports" mkdir "..\exports"
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%t"
copy /y "build\Release\MYOWNAPP_rebuilt.exe" "..\exports\MYOWNAPP_rebuilt_%STAMP%.exe" >nul
:: The exe needs its runtime DLLs sitting next to it to launch from exports\.
if exist "..\WebView2Loader.dll" copy /y "..\WebView2Loader.dll" "..\exports\WebView2Loader.dll" >nul
if exist "build\hook_dll\Release\app_hook.dll" copy /y "build\hook_dll\Release\app_hook.dll" "..\exports\app_hook.dll" >nul
echo Versioned copy saved: exports\MYOWNAPP_rebuilt_%STAMP%.exe (with WebView2Loader.dll + app_hook.dll)
