#include "MinHook.h"
#include <stdlib.h>
#include <string.h>

#define MAX_HOOKS 64
#define TRAMPOLINE_SIZE 64
#define JMP_REL32_SIZE 5
#define JMP_ABS64_SIZE 14

typedef struct {
    LPVOID pTarget;
    LPVOID pDetour;
    LPVOID pTrampoline;
    LPVOID* ppOriginal;
    BYTE   originalBytes[JMP_ABS64_SIZE];
    DWORD  originalSize;
    BOOL   isEnabled;
    BOOL   queueEnable;
} HOOK_ENTRY;

static HOOK_ENTRY g_hooks[MAX_HOOKS];
static UINT       g_hookCount = 0;
static BOOL       g_initialized = FALSE;

static UINT GetInstructionLength(LPBYTE addr) {
    /* Simplified x86-64 instruction length decoder.
       Handles the most common instruction prefixes and opcodes
       found at API entry points (mov, sub, push, lea, etc.) */
    BYTE b = addr[0];

    // REX prefixes
    BOOL hasRex = (b >= 0x40 && b <= 0x4F);
    if (hasRex) { b = addr[1]; }
    int base = hasRex ? 1 : 0;

    // Common 1-byte instructions
    if (b == 0xCC) return base + 1; // int3
    if (b == 0xC3) return base + 1; // ret
    if (b == 0x90) return base + 1; // nop
    if (b >= 0x50 && b <= 0x5F) return base + 1; // push/pop reg

    // mov reg, imm32 (B8+r)
    if (b >= 0xB8 && b <= 0xBF) return base + 5;

    // sub rsp, imm8 => 48 83 EC xx
    if (b == 0x83) {
        BYTE modrm = addr[base + 1];
        return base + 3; // opcode + modrm + imm8
    }

    // sub rsp, imm32 => 48 81 EC xx xx xx xx
    if (b == 0x81) {
        return base + 6; // opcode + modrm + imm32
    }

    // mov [rsp+x], reg or mov reg, [rsp+x] with SIB
    if (b == 0x89 || b == 0x8B) {
        BYTE modrm = addr[base + 1];
        BYTE mod = (modrm >> 6) & 3;
        BYTE rm = modrm & 7;
        int len = base + 2;
        if (rm == 4) len++; // SIB byte
        if (mod == 1) len += 1; // disp8
        else if (mod == 2) len += 4; // disp32
        else if (mod == 0 && rm == 5) len += 4; // RIP-relative
        return len;
    }

    // lea reg, [rip+disp32] => 8D 05/0D/15/1D/25/2D/35/3D + disp32
    if (b == 0x8D) {
        BYTE modrm = addr[base + 1];
        BYTE mod = (modrm >> 6) & 3;
        BYTE rm = modrm & 7;
        int len = base + 2;
        if (rm == 4) len++;
        if (mod == 0 && rm == 5) len += 4;
        else if (mod == 1) len += 1;
        else if (mod == 2) len += 4;
        return len;
    }

    // Two-byte opcodes (0F xx)
    if (b == 0x0F) {
        BYTE b2 = addr[base + 1];
        // jcc rel32
        if (b2 >= 0x80 && b2 <= 0x8F) return base + 6;
        // movzx, movsx
        if (b2 == 0xB6 || b2 == 0xB7 || b2 == 0xBE || b2 == 0xBF) {
            BYTE modrm = addr[base + 2];
            BYTE mod = (modrm >> 6) & 3;
            BYTE rm = modrm & 7;
            int len = base + 3;
            if (rm == 4) len++;
            if (mod == 1) len += 1;
            else if (mod == 2) len += 4;
            else if (mod == 0 && rm == 5) len += 4;
            return len;
        }
        return base + 2; // fallback for other 0F opcodes
    }

    // xor reg, reg => 33 /r or 31 /r
    if (b == 0x33 || b == 0x31) {
        return base + 2;
    }

    // test, cmp with modrm
    if (b == 0x85 || b == 0x3B || b == 0x39) {
        BYTE modrm = addr[base + 1];
        BYTE mod = (modrm >> 6) & 3;
        BYTE rm = modrm & 7;
        int len = base + 2;
        if (rm == 4) len++;
        if (mod == 1) len += 1;
        else if (mod == 2) len += 4;
        else if (mod == 0 && rm == 5) len += 4;
        return len;
    }

    // jmp/call rel32
    if (b == 0xE9 || b == 0xE8) return base + 5;
    // jmp/jcc rel8
    if (b == 0xEB || (b >= 0x70 && b <= 0x7F)) return base + 2;

    // Fallback: assume 1 byte
    return base + 1;
}

static LPVOID AllocateTrampoline(LPVOID pTarget) {
    /* Allocate executable memory near the target for the trampoline.
       Try within ±2GB for rel32 jumps first. */
    SYSTEM_INFO si;
    GetSystemInfo(&si);

    ULONG_PTR targetAddr = (ULONG_PTR)pTarget;
    ULONG_PTR minAddr = targetAddr > 0x7FFE0000ULL ? targetAddr - 0x7FFE0000ULL : (ULONG_PTR)si.lpMinimumApplicationAddress;
    ULONG_PTR maxAddr = targetAddr + 0x7FFE0000ULL;
    if (maxAddr > (ULONG_PTR)si.lpMaximumApplicationAddress)
        maxAddr = (ULONG_PTR)si.lpMaximumApplicationAddress;

    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR addr = minAddr;
    while (addr < maxAddr) {
        if (VirtualQuery((LPVOID)addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_FREE && mbi.RegionSize >= TRAMPOLINE_SIZE) {
            LPVOID p = VirtualAlloc((LPVOID)addr, TRAMPOLINE_SIZE,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
        addr = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
    }

    // Fallback: allocate anywhere
    return VirtualAlloc(NULL, TRAMPOLINE_SIZE,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void WriteAbsoluteJump(LPBYTE dest, LPVOID target) {
    /* Write a 14-byte absolute jump:
       FF 25 00 00 00 00   jmp qword ptr [rip+0]
       xx xx xx xx xx xx xx xx   <8-byte address> */
    dest[0] = 0xFF;
    dest[1] = 0x25;
    *(DWORD*)(dest + 2) = 0;
    *(UINT64*)(dest + 6) = (UINT64)target;
}

static HOOK_ENTRY* FindHook(LPVOID pTarget) {
    for (UINT i = 0; i < g_hookCount; i++) {
        if (g_hooks[i].pTarget == pTarget) return &g_hooks[i];
    }
    return NULL;
}

MH_STATUS WINAPI MH_Initialize(void) {
    if (g_initialized) return MH_ERROR_ALREADY_INITIALIZED;
    g_hookCount = 0;
    memset(g_hooks, 0, sizeof(g_hooks));
    g_initialized = TRUE;
    return MH_OK;
}

MH_STATUS WINAPI MH_Uninitialize(void) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;

    for (UINT i = 0; i < g_hookCount; i++) {
        if (g_hooks[i].isEnabled) MH_DisableHook(g_hooks[i].pTarget);
        if (g_hooks[i].pTrampoline) VirtualFree(g_hooks[i].pTrampoline, 0, MEM_RELEASE);
    }
    g_hookCount = 0;
    g_initialized = FALSE;
    return MH_OK;
}

MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID* ppOriginal) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;
    if (g_hookCount >= MAX_HOOKS) return MH_ERROR_MEMORY_ALLOC;
    if (FindHook(pTarget)) return MH_ERROR_ALREADY_CREATED;

    HOOK_ENTRY* hook = &g_hooks[g_hookCount];
    hook->pTarget = pTarget;
    hook->pDetour = pDetour;
    hook->ppOriginal = ppOriginal;
    hook->isEnabled = FALSE;

    // Allocate trampoline
    hook->pTrampoline = AllocateTrampoline(pTarget);
    if (!hook->pTrampoline) return MH_ERROR_MEMORY_ALLOC;

    // Calculate how many bytes to steal (need at least JMP_ABS64_SIZE = 14)
    LPBYTE src = (LPBYTE)pTarget;
    UINT totalLen = 0;
    while (totalLen < JMP_ABS64_SIZE) {
        UINT len = GetInstructionLength(src + totalLen);
        if (len == 0) return MH_ERROR_UNSUPPORTED_FUNCTION;
        totalLen += len;
    }
    hook->originalSize = totalLen;

    // Save original bytes
    memcpy(hook->originalBytes, pTarget, totalLen);

    // Build trampoline: original bytes + jump back to target + totalLen
    LPBYTE tramp = (LPBYTE)hook->pTrampoline;
    memcpy(tramp, pTarget, totalLen);
    WriteAbsoluteJump(tramp + totalLen, (LPBYTE)pTarget + totalLen);

    if (ppOriginal) *ppOriginal = hook->pTrampoline;

    g_hookCount++;
    return MH_OK;
}

MH_STATUS WINAPI MH_CreateHookApi(LPCWSTR pszModule, LPCSTR pszProcName,
                                   LPVOID pDetour, LPVOID* ppOriginal) {
    HMODULE hMod = GetModuleHandleW(pszModule);
    if (!hMod) hMod = LoadLibraryW(pszModule);
    if (!hMod) return MH_ERROR_MODULE_NOT_FOUND;

    LPVOID pTarget = (LPVOID)GetProcAddress(hMod, pszProcName);
    if (!pTarget) return MH_ERROR_FUNCTION_NOT_FOUND;

    return MH_CreateHook(pTarget, pDetour, ppOriginal);
}

MH_STATUS WINAPI MH_CreateHookApiEx(LPCWSTR pszModule, LPCSTR pszProcName,
                                     LPVOID pDetour, LPVOID* ppOriginal, LPVOID* ppTarget) {
    HMODULE hMod = GetModuleHandleW(pszModule);
    if (!hMod) hMod = LoadLibraryW(pszModule);
    if (!hMod) return MH_ERROR_MODULE_NOT_FOUND;

    LPVOID pTarget = (LPVOID)GetProcAddress(hMod, pszProcName);
    if (!pTarget) return MH_ERROR_FUNCTION_NOT_FOUND;
    if (ppTarget) *ppTarget = pTarget;

    return MH_CreateHook(pTarget, pDetour, ppOriginal);
}

MH_STATUS WINAPI MH_RemoveHook(LPVOID pTarget) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;
    HOOK_ENTRY* hook = FindHook(pTarget);
    if (!hook) return MH_ERROR_NOT_CREATED;
    if (hook->isEnabled) MH_DisableHook(pTarget);
    if (hook->pTrampoline) {
        VirtualFree(hook->pTrampoline, 0, MEM_RELEASE);
        hook->pTrampoline = NULL;
    }
    return MH_OK;
}

MH_STATUS WINAPI MH_EnableHook(LPVOID pTarget) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;

    if (pTarget == MH_ALL_HOOKS) {
        for (UINT i = 0; i < g_hookCount; i++) {
            if (!g_hooks[i].isEnabled) MH_EnableHook(g_hooks[i].pTarget);
        }
        return MH_OK;
    }

    HOOK_ENTRY* hook = FindHook(pTarget);
    if (!hook) return MH_ERROR_NOT_CREATED;
    if (hook->isEnabled) return MH_ERROR_ENABLED;

    DWORD oldProtect;
    if (!VirtualProtect(hook->pTarget, hook->originalSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        return MH_ERROR_MEMORY_PROTECT;

    WriteAbsoluteJump((LPBYTE)hook->pTarget, hook->pDetour);

    // NOP-fill remaining stolen bytes
    LPBYTE p = (LPBYTE)hook->pTarget;
    for (UINT i = JMP_ABS64_SIZE; i < hook->originalSize; i++) {
        p[i] = 0x90;
    }

    VirtualProtect(hook->pTarget, hook->originalSize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), hook->pTarget, hook->originalSize);

    hook->isEnabled = TRUE;
    return MH_OK;
}

MH_STATUS WINAPI MH_DisableHook(LPVOID pTarget) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;

    if (pTarget == MH_ALL_HOOKS) {
        for (UINT i = 0; i < g_hookCount; i++) {
            if (g_hooks[i].isEnabled) MH_DisableHook(g_hooks[i].pTarget);
        }
        return MH_OK;
    }

    HOOK_ENTRY* hook = FindHook(pTarget);
    if (!hook) return MH_ERROR_NOT_CREATED;
    if (!hook->isEnabled) return MH_ERROR_DISABLED;

    DWORD oldProtect;
    if (!VirtualProtect(hook->pTarget, hook->originalSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        return MH_ERROR_MEMORY_PROTECT;

    memcpy(hook->pTarget, hook->originalBytes, hook->originalSize);

    VirtualProtect(hook->pTarget, hook->originalSize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), hook->pTarget, hook->originalSize);

    hook->isEnabled = FALSE;
    return MH_OK;
}

MH_STATUS WINAPI MH_QueueEnableHook(LPVOID pTarget) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;
    HOOK_ENTRY* hook = (pTarget == MH_ALL_HOOKS) ? NULL : FindHook(pTarget);
    if (pTarget != MH_ALL_HOOKS && !hook) return MH_ERROR_NOT_CREATED;

    if (pTarget == MH_ALL_HOOKS) {
        for (UINT i = 0; i < g_hookCount; i++) g_hooks[i].queueEnable = TRUE;
    } else {
        hook->queueEnable = TRUE;
    }
    return MH_OK;
}

MH_STATUS WINAPI MH_QueueDisableHook(LPVOID pTarget) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;
    HOOK_ENTRY* hook = (pTarget == MH_ALL_HOOKS) ? NULL : FindHook(pTarget);
    if (pTarget != MH_ALL_HOOKS && !hook) return MH_ERROR_NOT_CREATED;

    if (pTarget == MH_ALL_HOOKS) {
        for (UINT i = 0; i < g_hookCount; i++) g_hooks[i].queueEnable = FALSE;
    } else {
        hook->queueEnable = FALSE;
    }
    return MH_OK;
}

MH_STATUS WINAPI MH_ApplyQueued(void) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;
    for (UINT i = 0; i < g_hookCount; i++) {
        if (g_hooks[i].queueEnable && !g_hooks[i].isEnabled)
            MH_EnableHook(g_hooks[i].pTarget);
        else if (!g_hooks[i].queueEnable && g_hooks[i].isEnabled)
            MH_DisableHook(g_hooks[i].pTarget);
    }
    return MH_OK;
}

const char* WINAPI MH_StatusToString(MH_STATUS status) {
    switch (status) {
        case MH_OK:                          return "OK";
        case MH_ERROR_ALREADY_INITIALIZED:   return "Already initialized";
        case MH_ERROR_NOT_INITIALIZED:       return "Not initialized";
        case MH_ERROR_ALREADY_CREATED:       return "Already created";
        case MH_ERROR_NOT_CREATED:           return "Not created";
        case MH_ERROR_ENABLED:               return "Already enabled";
        case MH_ERROR_DISABLED:              return "Already disabled";
        case MH_ERROR_NOT_EXECUTABLE:        return "Not executable";
        case MH_ERROR_UNSUPPORTED_FUNCTION:  return "Unsupported function";
        case MH_ERROR_MEMORY_ALLOC:          return "Memory allocation failed";
        case MH_ERROR_MEMORY_PROTECT:        return "Memory protection failed";
        case MH_ERROR_MODULE_NOT_FOUND:      return "Module not found";
        case MH_ERROR_FUNCTION_NOT_FOUND:    return "Function not found";
        default:                             return "Unknown";
    }
}
