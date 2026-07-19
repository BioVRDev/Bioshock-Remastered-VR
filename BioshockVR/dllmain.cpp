// BioshockVR/dllmain.cpp
//
// Entry point + logging

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include "Hooks.h"
#include "GameIni.h"

#include <MinHook.h>

static HMODULE           g_hSelf = nullptr;
static CRITICAL_SECTION  g_logLock;
static char              g_logPath[MAX_PATH] = {};
static char              g_iniPath[MAX_PATH] = {};

// Non-static: Hooks.cpp / CameraHook.cpp extern these.

// BSR's live FOV slider can't be read from memory, so the user declares it.
// MUST equal Bioshock.ini HorizontalFOV (§10) or turn-warp returns.
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

// Phase 11 kill switch: compose HMD head orientation onto the camera. Default OFF
// so a fresh deploy is known-good stereo; set EnableHeadTracking=1 to test.
bool g_cfgHeadTracking = false;

// 6DOF positional tracking: apply the head's translation to the camera.
bool g_cfgHeadPosition = false;

// The game presents with SyncInterval=1. Overriding it to 0 turned out NOT to be
// the framerate limiter, but the monitor is meaningless in VR, so we keep it off.
bool g_cfgDisableVSync = true;

// Push our FOV/resolution into the game's Bioshock.ini so the two files can
// never drift. 0 = leave Bioshock.ini strictly alone.
bool g_cfgSyncGameIni = true;

// Game render resolution, written to WindowedViewportX/Y. 0 = don't touch.
// Match the runtime's recommended per-eye size (logged at XR_Init).
int g_cfgResX = 0;
int g_cfgResY = 0;

// Draw-call fingerprinting / HUD suppression.
bool g_cfgDrawHook = true;
char g_cfgSuppressList[256] = {};
char g_cfgMenuList[256] = {};

// Isolate stepper (S18): the shortlist Numpad - walks, one count at a time.
// Default = the counts that appeared exactly ONCE PER FRAME in 100% of frames
// across two independent gameplay samples from the same spot. That is the
// signature of a singleton object (weapon, reticle, HUD root) rather than
// scenery, which varies with facing.
char g_cfgIsolateList[256] = {};

// S23: which draws are the first-person weapon, and how much to shrink their
// projection. MEASURED: the weapon renders at 44.3 deg horizontal FOV while the
// world renders at 110, so it appears 3.5x too large. 0 == leave it alone.
char  g_cfgWeaponList[256] = {};
float g_cfgWeaponScale = 0.0f;

// S25: health (23d) and EVE (11d) sit in the top-left corner of a 110-degree
// frame, outside where a headset can comfortably look. A viewport scaled about
// the centre pulls corner elements inward. 0 == leave them where they are.
char  g_cfgHudList[256] = {};
float g_cfgHudScale = 0.0f;

// Manual nudge for the menu quad's height, metres. The Quest accessibility
// height setting moves the LOCAL space origin, so the automatic anchor can sit
// low or high relative to where you actually are.
float g_cfgMenuHeight = 0.0f;

// Frames with this few DrawIndexed calls are a menu. Pre-game menus are pure
// Scaleform (ONE indexed draw); gameplay runs hundreds. 0 == off.
int g_cfgMenuMaxIndexed = 8;

// RETRACTED (S31). The "menus draw fewer calls" figure came from summing dump
// columns that DumpTable truncates at 64 rows, so it was a lower bound treated
// as exact. Measured properly with the untruncated per-frame counters:
//     gameplay    Draw 116-120   Indexed 387-399
//     pause menu  Draw 145-150   Indexed 382-386
// The menu draws MORE, not fewer, and Indexed is identical because the world
// renders behind it. A 25-call gap that moves with subtitles and pickup prompts
// is not a discriminator. OFF by default; do not re-enable without new data.
int g_cfgMenuMaxDraw = 0;

// S26: hooking DrawIndexedInstanced/DrawInstanced needs vtable slots 20 and 21.
// Slots 14/15 are Map/Unmap, and detouring THOSE with draw-shaped handlers
// crashed the game the moment a menu streamed textures. Off by default: the
// cursor turned out to be 5 Draw, so these were never needed.
bool g_cfgHookInstanced = false;

// Our own reticle: a head-locked quad layer, not a game draw call.
bool  g_cfgCrosshair = true;
float g_cfgXhSize = 0.012f;   // DOT diameter in metres at CrosshairDistance
float g_cfgXhDist = 2.0f;

// Menus/loading shown as a head-locked virtual screen (quad layer) instead of
// the wall-sized projection. Kill switch if it misbehaves.
bool g_cfgMenuScreen = true;

// Pair-lock (§14): render both eyes of a pair from the same camera instant.
bool g_cfgPairLock = true;

// Head-aim (§15): make Controller.Rotation follow the head. Default OFF -- it
// also steers movement direction in UE, so it changes how walking feels.
bool g_cfgHeadAim = false;

// S19 head-aim composition. 0 = legacy euler ADDITION (has the turn artifact),
// 1 = compose in the base's local frame, 2 = also drop mouse pitch so the
// horizon stays level and ALL pitch comes from the head.
int g_cfgHeadAimMode = 1;

// Menu quad geometry, metres. Angular size = 2*atan(size/2 / dist).
float g_cfgMenuSize = 1.5f;
float g_cfgMenuDist = 1.75f;

// Head roll onto the view. Suspect for the pitched-turn swivel (§16): if the
// roll axis is world-space rather than view-space, the error grows with pitch.
bool g_cfgHeadRoll = true;

// ---- Phase 15: Touch controllers as a virtual Xbox pad (InputHook.cpp) ----
// ControllerMode 0 = merge (a REAL pad in slot 0 wins, we only fill in when
// none is connected) / 1 = replace (always synthesize).
// ControllerPitch stays 0: HeadAimMode=2 erases injected pitch every CalcView
// and it reads as a fight. 1 only to A/B that artifact deliberately.
bool  g_cfgController = true;
int   g_cfgControllerMode  = 0;      // 0 merge, 1 replace
bool  g_cfgControllerPitch = false;
bool  g_cfgStickYToDpad    = false;
float g_cfgStickDeadzone   = 0.15f;
bool  g_cfgControllerLog   = true;
int g_cfgDpadModifier = 1;   // 0 off / 1 right thumbrest / 2 R3 / 3 left grip
int g_cfgControllerLayout = 1;   // 0 literal Xbox / 1 jump on right-A

int   g_cfgFgFovOffset = 0;      // ForegroundFovOffset, 0 == off
float g_cfgFgFovValue = 0.0f;   // ForegroundFovValue, 0 == use GameFovDegrees
bool g_cfgGameState = true;   // read the game's own input context (GameState.cpp)
int g_cfgFgFovSrc = 0;   // ForegroundFovSrcOffset, 0 == off
float g_cfgHeightOffset = 0.0f;   // CameraHeightOffset, cm. +up.


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

    g_cfgHeadTracking = (GetPrivateProfileIntA("VR", "EnableHeadTracking", 0, g_iniPath) != 0);
    Log("config: EnableHeadTracking = %d   %s", (int)g_cfgHeadTracking,
        g_cfgHeadTracking ? "(head-look composed onto camera)" : "(off -- stereo only)");

    g_cfgHeadPosition = (GetPrivateProfileIntA("VR", "EnableHeadPosition", 0, g_iniPath) != 0);
    Log("config: EnableHeadPosition = %d", (int)g_cfgHeadPosition);

    g_cfgDisableVSync = (GetPrivateProfileIntA("VR", "DisableVSync", 1, g_iniPath) != 0);
    Log("config: DisableVSync      = %d", (int)g_cfgDisableVSync);

    g_cfgMenuScreen = (GetPrivateProfileIntA("VR", "EnableMenuScreen", 1, g_iniPath) != 0);
    Log("config: EnableMenuScreen  = %d", (int)g_cfgMenuScreen);

    g_cfgSyncGameIni = (GetPrivateProfileIntA("VR", "SyncGameIni", 1, g_iniPath) != 0);
    Log("config: SyncGameIni       = %d", (int)g_cfgSyncGameIni);

    g_cfgResX = GetPrivateProfileIntA("VR", "ResolutionX", 0, g_iniPath);
    g_cfgResY = GetPrivateProfileIntA("VR", "ResolutionY", 0, g_iniPath);

    if (g_cfgResX < 0 || g_cfgResY < 0 || g_cfgResX > 16384 || g_cfgResY > 16384)
    {
        Log("config: Resolution out of range. Ignoring.");
        g_cfgResX = g_cfgResY = 0;
    }

    Log("config: Resolution        = %d x %d   (%s)", g_cfgResX, g_cfgResY,
        (g_cfgResX && g_cfgResY) ? "will be written to Bioshock.ini" : "not managed");

    g_cfgDrawHook = (GetPrivateProfileIntA("VR", "EnableDrawHook", 1, g_iniPath) != 0);
    Log("config: EnableDrawHook     = %d", (int)g_cfgDrawHook);

    GetPrivateProfileStringA("VR", "SuppressIndexCounts", "",
        g_cfgSuppressList, sizeof(g_cfgSuppressList), g_iniPath);
    Log("config: SuppressIndexCounts = '%s'", g_cfgSuppressList);

    GetPrivateProfileStringA("VR", "MenuIndexCounts", "1769,63,49,95,21,87",
        g_cfgMenuList, sizeof(g_cfgMenuList), g_iniPath);
    Log("config: MenuIndexCounts    = '%s'", g_cfgMenuList);

    GetPrivateProfileStringA("VR", "IsolateCounts",
        "7425,3360,600,381,297,174,153,144,129,33,63021,9,105,139,83,7,2,17,23",
        g_cfgIsolateList, sizeof(g_cfgIsolateList), g_iniPath);
    Log("config: IsolateCounts      = '%s'", g_cfgIsolateList);

    GetPrivateProfileStringA("VR", "WeaponCounts", "",
        g_cfgWeaponList, sizeof(g_cfgWeaponList), g_iniPath);
    {
        char b[64] = {};
        GetPrivateProfileStringA("VR", "WeaponScale", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v >= 0.0 && v <= 4.0) g_cfgWeaponScale = (float)v; }
    }
    Log("config: WeaponCounts       = '%s'  scale %.3f %s", g_cfgWeaponList,
        g_cfgWeaponScale, g_cfgWeaponScale > 0.0f ? "" : "(OFF)");

    GetPrivateProfileStringA("VR", "HudCounts", "",
        g_cfgHudList, sizeof(g_cfgHudList), g_iniPath);
    {
        char b[64] = {};
        GetPrivateProfileStringA("VR", "HudScale", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v >= 0.0 && v <= 4.0) g_cfgHudScale = (float)v; }
    }
    Log("config: HudCounts          = '%s'  scale %.3f %s", g_cfgHudList,
        g_cfgHudScale, g_cfgHudScale > 0.0f ? "" : "(OFF)");

    {
        char b[64] = {};
        GetPrivateProfileStringA("VR", "MenuScreenHeight", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v > -3.0 && v < 3.0) g_cfgMenuHeight = (float)v; }
        Log("config: MenuScreenHeight  = %+.2f m", g_cfgMenuHeight);
    }

    g_cfgHookInstanced = (GetPrivateProfileIntA("VR", "HookInstanced", 0, g_iniPath) != 0);
    Log("config: HookInstanced      = %d", (int)g_cfgHookInstanced);

    g_cfgMenuMaxDraw = GetPrivateProfileIntA("VR", "MenuMaxDraw", 0, g_iniPath);
    if (g_cfgMenuMaxDraw < 0 || g_cfgMenuMaxDraw > 100000) g_cfgMenuMaxDraw = 60;
    Log("config: MenuMaxDraw        = %d", g_cfgMenuMaxDraw);

    g_cfgMenuMaxIndexed = GetPrivateProfileIntA("VR", "MenuMaxIndexed", 8, g_iniPath);
    if (g_cfgMenuMaxIndexed < 0 || g_cfgMenuMaxIndexed > 100000) g_cfgMenuMaxIndexed = 8;
    Log("config: MenuMaxIndexed     = %d", g_cfgMenuMaxIndexed);

    g_cfgCrosshair = (GetPrivateProfileIntA("VR", "EnableCrosshair", 1, g_iniPath) != 0);
    Log("config: EnableCrosshair    = %d", (int)g_cfgCrosshair);

    {
        char b[64] = {};
        GetPrivateProfileStringA("VR", "CrosshairSize", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v > 0.0005 && v < 0.5) g_cfgXhSize = (float)v; }
        b[0] = 0;
        GetPrivateProfileStringA("VR", "CrosshairDistance", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v > 0.2 && v < 50.0) g_cfgXhDist = (float)v; }
        Log("config: Crosshair         = %.1f mm dot at %.2f m",
            g_cfgXhSize * 1000.f, g_cfgXhDist);
    }

    g_cfgPairLock = (GetPrivateProfileIntA("VR", "PairLockCamera", 1, g_iniPath) != 0);
    Log("config: PairLockCamera    = %d", (int)g_cfgPairLock);

    g_cfgHeadAim = (GetPrivateProfileIntA("VR", "EnableHeadAim", 0, g_iniPath) != 0);
    Log("config: EnableHeadAim      = %d", (int)g_cfgHeadAim);

    g_cfgHeadAimMode = GetPrivateProfileIntA("VR", "HeadAimMode", 1, g_iniPath);
    if (g_cfgHeadAimMode < 0 || g_cfgHeadAimMode > 2) g_cfgHeadAimMode = 1;
    Log("config: HeadAimMode        = %d   %s", g_cfgHeadAimMode,
        g_cfgHeadAimMode == 0 ? "(LEGACY additive -- turn artifact expected)" :
        g_cfgHeadAimMode == 1 ? "(local compose, mouse pitch kept)" :
        "(local compose, PITCH DECOUPLED)");

    {
        char b[64] = {};
        GetPrivateProfileStringA("VR", "MenuScreenSize", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v > 0.2 && v < 10.0) g_cfgMenuSize = (float)v; }
        b[0] = 0;
        GetPrivateProfileStringA("VR", "MenuScreenDistance", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v > 0.3 && v < 20.0) g_cfgMenuDist = (float)v; }
        Log("config: MenuScreen        = %.2f m at %.2f m", g_cfgMenuSize, g_cfgMenuDist);
    }

    g_cfgHeadRoll = (GetPrivateProfileIntA("VR", "EnableHeadRoll", 1, g_iniPath) != 0);
    Log("config: EnableHeadRoll     = %d", (int)g_cfgHeadRoll);

    g_cfgController = (GetPrivateProfileIntA("VR", "EnableController", 1, g_iniPath) != 0);
    g_cfgControllerMode = GetPrivateProfileIntA("VR", "ControllerMode", 0, g_iniPath);
    if (g_cfgControllerMode < 0 || g_cfgControllerMode > 1) g_cfgControllerMode = 0;
    g_cfgControllerPitch = (GetPrivateProfileIntA("VR", "ControllerPitch", 0, g_iniPath) != 0);
    g_cfgStickYToDpad = (GetPrivateProfileIntA("VR", "ControllerStickYToDpad", 0, g_iniPath) != 0);
    g_cfgControllerLog = (GetPrivateProfileIntA("VR", "ControllerLog", 1, g_iniPath) != 0);
    g_cfgDpadModifier = GetPrivateProfileIntA("VR", "ControllerDpadModifier", 1, g_iniPath);
    if (g_cfgDpadModifier < 0 || g_cfgDpadModifier > 3) g_cfgDpadModifier = 1;

    Log("config: DpadModifier      = %d   %s", g_cfgDpadModifier,
        g_cfgDpadModifier == 0 ? "(off)" :
        g_cfgDpadModifier == 1 ? "(right thumbrest touch)" :
        g_cfgDpadModifier == 2 ? "(right stick click)" : "(left grip)");

    {
        char b[64] = {};
        GetPrivateProfileStringA("VR", "ControllerDeadzone", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v >= 0.0 && v < 0.9) g_cfgStickDeadzone = (float)v; }
    }

    Log("config: EnableController   = %d   mode=%s", (int)g_cfgController,
        g_cfgControllerMode ? "replace" : "merge");
    Log("config: ControllerPitch    = %d   %s", (int)g_cfgControllerPitch,
        g_cfgControllerPitch ? "(right-stick Y PASSED THROUGH -- expect pitch fighting)"
        : "(right-stick Y dropped -- correct for HeadAimMode=2)");
    Log("config: ControllerDeadzone = %.2f   StickYToDpad=%d",
        g_cfgStickDeadzone, (int)g_cfgStickYToDpad);

    g_cfgControllerLayout = GetPrivateProfileIntA("VR", "ControllerLayout", 0, g_iniPath);
    if (g_cfgControllerLayout < 0 || g_cfgControllerLayout > 1) g_cfgControllerLayout = 0;
    Log("config: ControllerLayout  = %d   %s", g_cfgControllerLayout,
        g_cfgControllerLayout ? "(jump on right-A, use on right-B)"
        : "(literal Xbox: jump on left-Y, use on right-A)");

    g_cfgGameState = (GetPrivateProfileIntA("VR", "EnableGameState", 1, g_iniPath) != 0);
    Log("config: EnableGameState   = %d", (int)g_cfgGameState);

    {
        char b[64] = {};
        GetPrivateProfileStringA("VR", "ForegroundFovOffset", "", b, sizeof(b), g_iniPath);
        if (b[0]) g_cfgFgFovOffset = (int)strtol(b, nullptr, 0);   // accepts 0x1A4
        GetPrivateProfileStringA("VR", "ForegroundFovValue", "", b, sizeof(b), g_iniPath);
        if (b[0]) g_cfgFgFovValue = (float)atof(b);
        GetPrivateProfileStringA("VR", "ForegroundFovSrcOffset", "", b, sizeof(b), g_iniPath);
        if (b[0]) g_cfgFgFovSrc = (int)strtol(b, nullptr, 0);
    }
    Log("config: ForegroundFov     = offset 0x%X  value %.1f",
        g_cfgFgFovOffset, g_cfgFgFovValue);

    {
        char b[64] = {};
        GetPrivateProfileStringA("VR", "CameraHeightOffset", "", b, sizeof(b), g_iniPath);
        if (b[0]) { double v = atof(b); if (v > -100.0 && v < 100.0) g_cfgHeightOffset = (float)v; }
    }
    Log("config: CameraHeightOffset = %.1f cm", g_cfgHeightOffset);

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
    SyncGameIni();

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