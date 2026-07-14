// BioshockVR/dllmain.cpp
//
// Entry point + logging. Deliberately contains NO absolute paths:
// the log is written next to this DLL, wherever that happens to be.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include "Hooks.h"

#include <MinHook.h>

static HMODULE           g_hSelf = nullptr;
static CRITICAL_SECTION  g_logLock;
static char              g_logPath[MAX_PATH] = {};
static char              g_iniPath[MAX_PATH] = {};

// Non-static: Hooks.cpp externs this. §6h -- BSR's live FOV slider value can't
// be read from memory, so the user declares it. Stock is 100 horizontal.
float g_cfgFovDeg = 100.0f;

// Kill switch. If the camera hook crashes the game, set EnableCameraHook=0
// in BioshockVR.ini and you're playable again with NO rebuild.
bool g_cfgCameraHook = true;

// Resolve "<folder containing this DLL>\BioshockVR.log" and ".ini" at runtime.
// Never hardcode a path -- a hardcoded path both breaks on other machines
// and ships your username inside the binary.
static void InitLogPath()
{
    char p[MAX_PATH] = {};
    if (GetModuleFileNameA(g_hSelf, p, MAX_PATH) == 0) { g_logPath[0] = 0; return; }

    char* slash = strrchr(p, '\\');
    if (!slash) { g_logPath[0] = 0; return; }
    *(slash + 1) = 0;                       // truncate to the directory

    _snprintf_s(g_logPath, MAX_PATH, _TRUNCATE, "%sBioshockVR.log", p);
    _snprintf_s(g_iniPath, MAX_PATH, _TRUNCATE, "%sBioshockVR.ini", p);
}

// XRSession.cpp declares this extern. Keep the name.
void LogFile(const char* msg)
{
    if (!g_logPath[0]) return;

    EnterCriticalSection(&g_logLock);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02u:%02u:%02u.%03u] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a;
    va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

// Read BioshockVR.ini, sitting next to this DLL. Read-only -- we never write
// it, because Program Files is UAC-protected and a write would get silently
// redirected to VirtualStore, which is a debugging nightmare.
static void LoadConfig()
{
    if (!g_iniPath[0]) { Log("config: no ini path. Using defaults."); return; }

    char buf[64] = {};
    GetPrivateProfileStringA("VR", "GameFovDegrees", "", buf, sizeof(buf), g_iniPath);

    bool got = false;
    if (buf[0])
    {
        double v = atof(buf);
        if (v > 30.0 && v < 170.0) { g_cfgFovDeg = (float)v; got = true; }
        else Log("config: GameFovDegrees '%s' is out of range. Ignoring.", buf);
    }
    Log("config: GameFovDegrees  = %.1f   (%s)", g_cfgFovDeg,
        got ? "from BioshockVR.ini" : "DEFAULT");

    g_cfgCameraHook = (GetPrivateProfileIntA("VR", "EnableCameraHook", 1, g_iniPath) != 0);
    Log("config: EnableCameraHook = %d", (int)g_cfgCameraHook);
}

// Real init happens off the loader lock. DllMain is not a safe place to
// allocate, hook, or touch other modules.
static DWORD WINAPI InitThread(LPVOID)
{
    Log("=== BioshockVR ===");

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* exeName = strrchr(exe, '\\');
    exeName = exeName ? exeName + 1 : exe;

    Log("host process : %s", exeName);
    Log("pointer size : %u bytes  (4 == x86, correct)", (unsigned)sizeof(void*));
    Log("dll base     : 0x%08X", (unsigned)(uintptr_t)g_hSelf);

    MH_STATUS s = MH_Initialize();
    Log("MH_Initialize -> %d  (0 == MH_OK)", (int)s);
    if (s != MH_OK) { Log("!!! MinHook dead. Stopping."); return 0; }

    LoadConfig();
    if (!Hooks_Install())
    {
        Log("!!! Hooks_Install FAILED. No hook installed. Game will run clean.");
        return 0;
    }

    Log("Phase 2 init done. Hook armed.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&g_logLock);
        InitLogPath();

        // Truncate any log from a previous run, so what we read is this run only.
        if (g_logPath[0])
        {
            FILE* f = nullptr;
            if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);
        }

        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}