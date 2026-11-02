#pragma once
#include <Windows.h>
#include <string>

namespace RegistrySetup {

bool IsElevated();

// --setup handler: writes AppInit_DLLs, waits for parent to exit, cleans up.
int RunSetupHelper(DWORD parentPid);

// --cleanup handler: clears AppInit_DLLs and removes active DLL copies.
int RunCleanupHelper();

// Called from normal startup: ensures AppInit_DLLs is configured.
// Spawns elevated --setup <PID> helper if needed.
void EnsureAppInit();

} // namespace RegistrySetup
