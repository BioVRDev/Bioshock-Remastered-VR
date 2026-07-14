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

// Non-static: Hooks.cpp / CameraHook.cpp extern these.

// §6h. BSR's live FOV slider can't be read from memory, so the user declares it.
// Stock is 100 horizontal. Slider and ini MUST match.
float g_cfgFovDeg = 100.0f;

// Phase 5 kill switch: if the camera hook crashes the game, set 0. No rebuild.
bool g_cfgCameraHook = true;

// Phase 6 kill switch, SEPARATE from the above. This is the one that lets the
// hook actually MODIFY the game's camera.
bool g_cfgCameraWrite = false;

// Half-IPD in game units. 1 unit == 1 cm, MEASURED (§6b-note). 3.2 == 64mm IPD.
float g_cfgEyeSep = 3.2f;

// If depth comes out INVERTED (world feels like a hollow mask), flip this.
bool g_cfgSwapEyes = false;

// The game presents with SyncInterval=1. Overriding it to 0 turned out NOT to be
// the framerate limiter, but the monitor is meaningless in VR, so we keep it off.
bool g_cfgDisableVSync = true;

// XRMode:
//   0 = XR never initializes. Flat game, with VD Streamer running and the headset
//       connected. MEASURES the game's true Present rate under VD's load.
//   1 = PHASE 5. Full XR cycle every Present, same image to both eyes. MONO.
//       KNOWN GOOD: 118 submits/sec, headset at 120. The floor.
//   2 = What §4 prescribed: submit every SECOND Present. HALVES the submit rate
//       to 59/sec and drops the headset to 60. THE REGRESSION. Kept to reproduce.
//   3 = AER. Full XR cycle EVERY Present, so the submit rate stays at 118/sec --
//       but each eye carries its own image, one Present apart. Stereo never
//       required starving the compositor. THIS IS THE ONE.
int g_cfgXRMode = 3;

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

// Read-only. We never WRITE the ini -- Program Files is UAC-protected and a
// write gets silently redirected to VirtualStore (a debugging nightmare, §8).
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
    Log("config: GameFovDegrees   = %.1f   (%s)", g_cfgFovDeg,
        got ? "from BioshockVR.ini" : "DEFAULT");

    g_cfgCameraHook = (GetPrivateProfileIntA("VR", "EnableCameraHook", 1, g_iniPath) != 0);
    Log("config: EnableCameraHook  = %d", (int)g_cfgCameraHook);

    g_cfgCameraWrite = (GetPrivateProfileIntA("VR", "EnableCameraWrite", 0, g_iniPath) != 0);
    Log("config: EnableCameraWrite = %d   %s", (int)g_cfgCameraWrite,
        g_cfgCameraWrite ? "(the camera WILL be modified)" : "(read-only, no stereo)");

    buf[0] = 0;
    GetPrivateProfileStringA("VR", "EyeSeparation", "", buf, sizeof(buf), g_iniPath);
    got = false;
    if (buf[0])
    {
        double v = atof(buf);
        if (v >= 0.0 && v <= 20.0) { g_cfgEyeSep = (float)v; got = true; }
        else Log("config: EyeSeparation '%s' is out of range (0..20). Ignoring.", buf);
    }
    Log("config: EyeSeparation     = %.2f units (cm)   (%s)", g_cfgEyeSep,
        got ? "from BioshockVR.ini" : "DEFAULT");

    g_cfgSwapEyes = (GetPrivateProfileIntA("VR", "SwapEyes", 0, g_iniPath) != 0);
    Log("config: SwapEyes          = %d", (int)g_cfgSwapEyes);

    g_cfgDisableVSync = (GetPrivateProfileIntA("VR", "DisableVSync", 1, g_iniPath) != 0);
    Log("config: DisableVSync      = %d", (int)g_cfgDisableVSync);

    g_cfgXRMode = GetPrivateProfileIntA("VR", "XRMode", 3, g_iniPath);
    if (g_cfgXRMode < 0 || g_cfgXRMode > 4) { Log("config: XRMode out of range. Forcing 3."); g_cfgXRMode = 3; }
    Log("config: XRMode            = %d   %s", g_cfgXRMode,
        g_cfgXRMode == 0 ? "(NO XR -- flat)" :
        g_cfgXRMode == 1 ? "(PHASE 5 mono -- KNOWN GOOD)" :
        g_cfgXRMode == 2 ? "(submit every 2nd Present -- THE REGRESSION)" :
        g_cfgXRMode == 3 ? "(AER, flipping eye pairing -- WORKS, shimmers on turns)" :
        "(AER, STABLE eye pairing -- no shimmer)");
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

    Log("Init done. Present hook armed.");
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