#include "RegistrySetup.h"
#include <filesystem>

namespace fs = std::filesystem;

static const wchar_t* kRegPath =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows";
static const wchar_t* kSetupEvent = L"Local\\DWM_Comp_SetupReady";

static bool SetAppInitView(REGSAM view, const std::wstring& dllPath) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0,
            KEY_SET_VALUE | view, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD one = 1, zero = 0;
    RegSetValueExW(hKey, L"AppInit_DLLs", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(dllPath.c_str()),
        static_cast<DWORD>((dllPath.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(hKey, L"LoadAppInit_DLLs", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&one), sizeof(DWORD));
    RegSetValueExW(hKey, L"RequireSignedAppInit_DLLs", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&zero), sizeof(DWORD));

    RegCloseKey(hKey);
    return true;
}

static bool ClearAppInitView(REGSAM view) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0,
            KEY_SET_VALUE | view, &hKey) != ERROR_SUCCESS)
        return false;

    const wchar_t empty[] = L"";
    RegSetValueExW(hKey, L"AppInit_DLLs", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
    DWORD zero = 0;
    RegSetValueExW(hKey, L"LoadAppInit_DLLs", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&zero), sizeof(DWORD));

    RegCloseKey(hKey);
    return true;
}

static void CleanupActiveDlls(const fs::path& dir) {
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        auto name = entry.path().filename().wstring();
        if (name.find(L"dxgi_assist_active_") == 0 &&
            name.size() > 4 &&
            name.substr(name.size() - 4) == L".dll") {
            fs::remove(entry.path(), ec);
        }
    }
}

bool RegistrySetup::IsElevated() {
    BOOL elevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev{};
        DWORD size = sizeof(elev);
        if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &size))
            elevated = elev.TokenIsElevated;
        CloseHandle(hToken);
    }
    return elevated != FALSE;
}

int RegistrySetup::RunSetupHelper(DWORD parentPid) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    fs::path srcDll = exeDir / L"dxgi_assist.dll";

    if (!fs::exists(srcDll)) return 1;

    CleanupActiveDlls(exeDir);

    wchar_t uniqueName[64]{};
    wsprintfW(uniqueName, L"dxgi_assist_active_%lu.dll", GetTickCount());
    fs::path activeDll = exeDir / uniqueName;

    std::error_code ec;
    fs::copy_file(srcDll, activeDll, fs::copy_options::overwrite_existing, ec);
    if (ec) return 1;

    std::wstring dllFullPath = activeDll.wstring();
    SetAppInitView(KEY_WOW64_64KEY, dllFullPath);
    SetAppInitView(KEY_WOW64_32KEY, dllFullPath);

    HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kSetupEvent);
    if (hEvent) {
        SetEvent(hEvent);
        CloseHandle(hEvent);
    }

    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (hParent) {
        WaitForSingleObject(hParent, INFINITE);
        CloseHandle(hParent);
    }

    ClearAppInitView(KEY_WOW64_64KEY);
    ClearAppInitView(KEY_WOW64_32KEY);
    CleanupActiveDlls(exeDir);

    return 0;
}

int RegistrySetup::RunCleanupHelper() {
    ClearAppInitView(KEY_WOW64_64KEY);
    ClearAppInitView(KEY_WOW64_32KEY);

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    CleanupActiveDlls(fs::path(exePath).parent_path());
    return 0;
}

void RegistrySetup::EnsureAppInit() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH]{};
        DWORD size = sizeof(value);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"AppInit_DLLs", nullptr, &type,
                reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS) {
            if (wcslen(value) > 0 && wcsstr(value, L"dxgi_assist_active_") &&
                fs::exists(value)) {
                RegCloseKey(hKey);
                return;
            }
        }
        RegCloseKey(hKey);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    sa.lpSecurityDescriptor = &sd;

    HANDLE hEvent = CreateEventW(&sa, TRUE, FALSE, kSetupEvent);

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t params[64]{};
    wsprintfW(params, L"--setup %lu", GetCurrentProcessId());

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (ShellExecuteExW(&sei)) {
        if (hEvent) WaitForSingleObject(hEvent, 10000);
        if (sei.hProcess) CloseHandle(sei.hProcess);
    }
    if (hEvent) CloseHandle(hEvent);
}
