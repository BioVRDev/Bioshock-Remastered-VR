// BioshockVR/dllmain.cpp
//
// Entry point + logging + config.
//
// The config loader is split into two halves so it reads cleanly: a block of
// terse Cfg* reads that fill the globals, then a single grouped, aligned echo.
// Every key, default and range is the same as before -- only the shape changed.

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

// ============================================================================
//  CONFIG GLOBALS  (non-static: other .cpp files extern these)
// ============================================================================

// core -----------------------------------------------------------------------
float g_cfgFovDeg = 100.0f;      // MUST equal Bioshock.ini HorizontalFOV
int   g_cfgResX = 0;
int   g_cfgResY = 0;
int   g_cfgFullscreen = -1;      // -1 leave alone / 0 windowed / 1 exclusive
bool  g_cfgForceFlip = false;    // rewrite the swapchain flip-model + ALLOW_TEARING
int   g_cfgMirrorEvery = 1;      // present the desktop mirror every Nth frame. 0 = never
float g_cfgEyeSep = 3.2f;        // half-IPD, game units == cm. 3.2 = 64mm IPD
bool  g_cfgSwapEyes = false;     // flip if depth is inverted
bool  g_cfgDisableVSync = true;
bool  g_cfgSyncGameIni = true;   // push FOV/res into Bioshock.ini; 0 = leave

// camera & comfort -----------------------------------------------------------
bool  g_cfgCameraHook = true;    // install the camera hook at all
bool  g_cfgCameraWrite = false;  // let it MODIFY the camera (Phase 6 switch)
bool  g_cfgHeadTracking = false; // compose HMD orientation onto the camera
bool  g_cfgHeadPosition = false; // apply the head's translation
bool  g_cfgHeadRoll = true;
bool  g_cfgHeadAim = false;      // make Controller.Rotation follow the aim
int   g_cfgHeadAimMode = 1;      // 0 additive, 1 local compose, 2 pitch-decoupled
bool  g_cfgPairLock = true;      // render both eyes from the same instant
float g_cfgHeightOffset = 0.0f;  // CameraHeightOffset, cm, +up
bool  g_cfgCutsceneTheater = false;  // show cutscenes on the flat quad
int   g_cfgDeltaClamp = 0;           // 0 off, 1 player world, 2 BOTH worlds

// weapon & arm rendering -----------------------------------------------------
int   g_cfgFgFovOffset = 0;      // ForegroundFovOffset, 0 == off
float g_cfgFgFovValue = 0.0f;    // ForegroundFovValue, 0 == use GameFovDegrees
bool  g_cfgFgFovAuto = false;    // derive ForegroundFovValue from the backbuffer
int   g_cfgFgFovSrc = 0;         // ForegroundFovSrcOffset, 0 == off
int   g_cfgWorldFovOff = 0;      // controller+N -> world FOV. 0 == off
int   g_cfgWorldFovOff2 = 0;     // its mirror. 0 == off
float g_cfgWorldFovMax = 0.0f;   // above this, snap back. 0 == off
float g_cfgWorldFovMin = 0.0f;   // below this, snap back. 0 == off
float g_cfgWorldFovVal = 75.0f;  // the value to snap back to
float g_cfgHandsScale = 0.0f;    // DrawScale for the arms. 0 == leave alone
float g_cfgGunScale = 0.0f;      // DrawScale for the weapon actor. 0 == off
int   g_cfgGunPtrOff = 0;        // GunPtrOffset. 0 == run the sweep
int   g_cfgGunPtrBase = 1;       // 0 == pawn, 1 == Hands
int   g_cfgGunChildren = 0;      // 0 off, 1 sweep, 2 scale all

// 6-DOF hands ----------------------------------------------------------------
bool  g_cfg6DofHands = false;
bool  g_cfgHandsProbe = false;
int   g_cfgHandsPtrOff = 0;      // HandsPtrOffset, e.g. 0x724
int   g_cfgHandsPosOff = 0;      // HandsPosOffset, e.g. 0x1D8
float g_cfgHandsGrip[3] = { 0.0f, 0.0f, 0.0f };   // fwd,right,up cm
float g_cfgGripTunedFgFov = 0.0f;  // fg FOV the grip offsets were tuned at. 0 == off
float g_cfgHandsRot[3] = { 0.0f, 0.0f, 0.0f };   // pitch,yaw,roll deg  LIVE
float g_cfgGripSlot[9][3] = {};                  // per-weapon position, from ini
float g_cfgRotSlot[9][3] = {};                   // per-weapon rotation, from ini
float g_cfgCursorRot[3] = { 0.f, 0.f, 0.f };     // CursorOffset p,y,r deg. LIVE
float g_cfgCursorSlot[9][3] = {};                // per-weapon, from the ini
int   g_cfgHandsArmCalls = 600;    // CalcView calls before the probe arms
int   g_cfgHandsRetryCalls = 600;  // calls between STAGE A retries
int   g_cfgIdleAnimMode = 0;       // 0 off, 1 entry[0], 2 HandsDown, 3 Equipping
int   g_cfgIdleModeSlot[9] = {};   // per-weapon override
int   g_cfgHideArmsSlot[9] = {};                 // per-weapon arm suppression
float g_cfgHandsNudgeZ = 0.0f;     // probe only
float g_cfgHandsNudgeYaw = 0.0f;   // probe only
float g_cfgHandsNudgePitch = 0.0f; // probe only

// aiming / crosshair ---------------------------------------------------------
int   g_cfgAimSource = 0;        // 0 head, 1 right controller
float g_cfgAimClampDeg = 20.0f;
float g_cfgAimSmooth = 0.35f;
float g_cfgPlasmidAimPitch = -50.0f;   // deg added to the plasmid hand's aim pitch
bool  g_cfgCrosshair = true;
float g_cfgXhSize = 0.012f;      // dot diameter, metres, at CrosshairDistance
float g_cfgXhDist = 2.0f;

// controller -----------------------------------------------------------------
bool  g_cfgController = true;
int   g_cfgControllerMode = 0;   // 0 merge (real pad wins), 1 replace
int   g_cfgControllerLayout = 1; // 0 literal Xbox, 1 jump on right-A
bool  g_cfgControllerPitch = false;
bool  g_cfgStickYToDpad = false;
float g_cfgStickDeadzone = 0.15f;
bool  g_cfgControllerLog = true;
int   g_cfgDpadModifier = 1;     // 0 off / 1 right thumbrest / 2 R3 / 3 left grip
bool  g_cfgJumpOnR3 = false;     // R3 -> jump instead of zoom

// menus ----------------------------------------------------------------------
bool  g_cfgMenuScreen = true;
float g_cfgMenuSize = 1.5f;
float g_cfgMenuDist = 1.75f;
float g_cfgMenuHeight = 0.0f;
int   g_cfgMenuMaxIndexed = 8;
int   g_cfgMenuMaxDraw = 0;      // RETRACTED (S31), default 0
char  g_cfgMenuList[256] = {};
char  g_cfgAnchorList[256] = {};   // in-game UIs that belong on the world-locked quad

// debug / probe --------------------------------------------------------------
bool  g_cfgDrawHook = true;
bool  g_cfgGameState = true;     // read the game's own input context
bool  g_cfgHookInstanced = false;
char  g_cfgSuppressList[256] = {};
char  g_cfgIsolateList[256] = {};
char  g_cfgWeaponList[256] = {};
float g_cfgWeaponScale = 0.0f;
char  g_cfgHudList[256] = {};
float g_cfgHudScale = 0.0f;
char  g_cfgArrowList[256] = {};   // ArrowCounts, e.g. 234i@512x512
float g_cfgArrowScale = 1.0f;     // 0 == leave size alone
float g_cfgArrowX = 0.0f;         // fraction of viewport width, + == right
float g_cfgArrowY = 0.0f;         // fraction of viewport height, - == up
int   g_cfgArrowPtrOff = 0;                    // pawn+N -> the arrow actor. 0 == off
float g_cfgArrowWorld[3] = { 0.f, 0.f, 60.f }; // fwd,right,up from the camera, cm

// ============================================================================
//  LOGGING
// ============================================================================

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

// ============================================================================
//  CONFIG READERS  (one call per key; range-checked)
// ============================================================================

static bool CfgBool(const char* key, bool def)
{
    return GetPrivateProfileIntA("VR", key, def ? 1 : 0, g_iniPath) != 0;
}

static int CfgInt(const char* key, int def)
{
    return GetPrivateProfileIntA("VR", key, def, g_iniPath);
}

// Integer with a valid range; out-of-range falls back to def.
static int CfgIntRange(const char* key, int def, int lo, int hi)
{
    int v = GetPrivateProfileIntA("VR", key, def, g_iniPath);
    return (v < lo || v > hi) ? def : v;
}

// Decimal or 0x-hex. Absent key -> cur unchanged.
static int CfgHex(const char* key, int cur)
{
    char b[64] = {};
    GetPrivateProfileStringA("VR", key, "", b, sizeof(b), g_iniPath);
    return b[0] ? (int)strtol(b, nullptr, 0) : cur;
}

// Float in [lo,hi]. Absent -> cur unchanged. Present-but-out-of-range warns.
static float CfgFloat(const char* key, float cur, float lo, float hi)
{
    char b[64] = {};
    GetPrivateProfileStringA("VR", key, "", b, sizeof(b), g_iniPath);
    if (!b[0]) return cur;
    double v = atof(b);
    if (v < lo || v > hi)
    {
        Log("config: %s '%s' out of range [%.3g..%.3g] -- keeping %.3g",
            key, b, (double)lo, (double)hi, (double)cur);
        return cur;
    }
    return (float)v;
}

static void CfgStr(const char* key, const char* def, char* out, size_t sz)
{
    GetPrivateProfileStringA("VR", key, def, out, (DWORD)sz, g_iniPath);
}

// "fwd,right,up" -> out[3]; only assigns when all three parse.
static void CfgVec3(const char* key, float out[3])
{
    char b[64] = {};
    GetPrivateProfileStringA("VR", key, "", b, sizeof(b), g_iniPath);
    if (!b[0]) return;
    float f[3] = {};
    if (sscanf_s(b, "%f,%f,%f", &f[0], &f[1], &f[2]) == 3)
    {
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    }
}

// One echo line: indent, fixed-width name, value text.
static void CfgEcho(const char* name, const char* fmt, ...)
{
    char val[256];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(val, sizeof(val), _TRUNCATE, fmt, a);
    va_end(a);
    Log("  %-20s %s", name, val);
}

// ============================================================================
//  LOAD
// ============================================================================
// Read-only. We never WRITE the ini -- Program Files is UAC-protected and a
// write gets silently redirected to VirtualStore.
static void LoadConfig()
{
    if (!g_iniPath[0]) { Log("config: no ini path. Using defaults."); return; }

    // ---- read ----
    // core
    g_cfgFovDeg = CfgFloat("GameFovDegrees", g_cfgFovDeg, 30.f, 170.f);
    g_cfgResX = CfgIntRange("ResolutionX", 0, 0, 16384);
    g_cfgResY = CfgIntRange("ResolutionY", 0, 0, 16384);
    g_cfgEyeSep = CfgFloat("EyeSeparation", g_cfgEyeSep, 0.f, 20.f);
    g_cfgSwapEyes = CfgBool("SwapEyes", false);
    g_cfgDisableVSync = CfgBool("DisableVSync", true);
    g_cfgForceFlip = CfgBool("ForceFlipModel", false);
    g_cfgMirrorEvery = CfgIntRange("MirrorPresentEvery", 1, 0, 240);
    g_cfgSyncGameIni = CfgBool("SyncGameIni", true);
    // Via CfgFloat, not CfgInt: CfgFloat's (name, default, min, max) form is
    // the one proven to compile here, and this needs the min/max clamp because
    // -1 / 0 / 1 are three distinct meanings and a typo must not become mode 7.
    g_cfgFullscreen = CfgIntRange("Fullscreen", -1, -1, 1);

    // camera & comfort
    g_cfgCameraHook = CfgBool("EnableCameraHook", true);
    g_cfgCameraWrite = CfgBool("EnableCameraWrite", false);
    g_cfgHeadTracking = CfgBool("EnableHeadTracking", false);
    g_cfgHeadPosition = CfgBool("EnableHeadPosition", false);
    g_cfgHeadRoll = CfgBool("EnableHeadRoll", true);
    g_cfgHeadAim = CfgBool("EnableHeadAim", false);
    g_cfgHeadAimMode = CfgIntRange("HeadAimMode", 1, 0, 2);
    g_cfgPairLock = CfgBool("PairLockCamera", true);
    g_cfgHeightOffset = CfgFloat("CameraHeightOffset", g_cfgHeightOffset, -100.f, 100.f);
    g_cfgCutsceneTheater = CfgBool("CutsceneTheater", false);
    g_cfgDeltaClamp = CfgInt("DeltaClamp", 0);

    // weapon & arm rendering
    g_cfgFgFovOffset = CfgHex("ForegroundFovOffset", g_cfgFgFovOffset);
    g_cfgFgFovValue = CfgFloat("ForegroundFovValue", g_cfgFgFovValue, 0.f, 360.f);
    g_cfgFgFovAuto = CfgBool("ForegroundFovAuto", false);
    g_cfgFgFovSrc = CfgHex("ForegroundFovSrcOffset", g_cfgFgFovSrc);
    g_cfgWorldFovOff = CfgHex("WorldFovOffset", g_cfgWorldFovOff);
    g_cfgWorldFovOff2 = CfgHex("WorldFovOffset2", g_cfgWorldFovOff2);
    g_cfgWorldFovMax = CfgFloat("WorldFovMax", g_cfgWorldFovMax, 0.f, 170.f);
    g_cfgWorldFovMin = CfgFloat("WorldFovMin", g_cfgWorldFovMin, 0.f, 170.f);
    g_cfgWorldFovVal = CfgFloat("WorldFovValue", g_cfgWorldFovVal, 5.f, 170.f);
    g_cfgHandsScale = CfgFloat("HandsScale", g_cfgHandsScale, 0.05f, 5.f);
    g_cfgGunScale = CfgFloat("GunScale", g_cfgGunScale, 0.05f, 5.f);
    g_cfgGunPtrOff = CfgHex("GunPtrOffset", g_cfgGunPtrOff);
    g_cfgGunPtrBase = CfgInt("GunPtrBase", 1);
    g_cfgGunChildren = CfgInt("GunChildren", 0);

    // 6-DOF hands
    g_cfg6DofHands = CfgBool("Enable6DofHands", false);
    g_cfgHandsProbe = CfgBool("EnableHandsProbe", false);
    g_cfgHandsPtrOff = CfgHex("HandsPtrOffset", g_cfgHandsPtrOff);
    g_cfgHandsPosOff = CfgHex("HandsPosOffset", g_cfgHandsPosOff);
    CfgVec3("HandsGripOffset", g_cfgHandsGrip);
    g_cfgGripTunedFgFov = CfgFloat("GripTunedFgFov", g_cfgGripTunedFgFov, 0.f, 360.f);
    CfgVec3("HandsRotOffset", g_cfgHandsRot);
    g_cfgHandsArmCalls = CfgIntRange("HandsArmCalls", 600, 1, 5000);
    g_cfgHandsRetryCalls = CfgIntRange("HandsRetryCalls", 600, 1, 5000);
    g_cfgIdleAnimMode = CfgIntRange("IdleAnimMode", 0, 0, 3);

    // Per-weapon tables. Slot order is AllPossibleWeaponClasses -- 0 Wrench,
    // 1 Pistol, 2 Shotgun, 3 Crossbow, 4 GrenadeLauncher, 5 MachineGun,
    // 6 ChemicalThrower, 7 ResearchCamera -- plus 8 = plasmid. Each slot is
    // seeded from the two globals first, so a slot with no ini key inherits them
    // instead of snapping to zero.
    for (int s = 0; s < 9; ++s)
    {
        for (int a = 0; a < 3; ++a)
        {
            g_cfgGripSlot[s][a] = g_cfgHandsGrip[a];
            g_cfgRotSlot[s][a] = g_cfgHandsRot[a];
            g_cfgCursorSlot[s][a] = 0.0f;
        }
        char key[32];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "GripOffset%d", s);
        CfgVec3(key, g_cfgGripSlot[s]);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "RotOffset%d", s);
        CfgVec3(key, g_cfgRotSlot[s]);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "CursorOffset%d", s);
        CfgVec3(key, g_cfgCursorSlot[s]);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "IdleAnimMode%d", s);
        g_cfgIdleModeSlot[s] = CfgIntRange(key, g_cfgIdleAnimMode, 0, 3);

        // HideArmsN: auto-arm the SuppressIndexCounts list while this weapon is
        // held. Guns hide the arms; the wrench and plasmids keep them.
        _snprintf_s(key, sizeof(key), _TRUNCATE, "HideArms%d", s);
        g_cfgHideArmsSlot[s] = CfgIntRange(key, 0, 0, 1);
    }
    g_cfgHandsNudgeZ = CfgFloat("HandsNudgeZ", g_cfgHandsNudgeZ, -500.f, 500.f);
    g_cfgHandsNudgeYaw = CfgFloat("HandsNudgeYaw", g_cfgHandsNudgeYaw, -180.f, 180.f);
    g_cfgHandsNudgePitch = CfgFloat("HandsNudgePitch", g_cfgHandsNudgePitch, -180.f, 180.f);

    // aiming / crosshair
    g_cfgAimSource = CfgInt("AimSource", 0);
    g_cfgAimClampDeg = CfgFloat("AimClampDeg", g_cfgAimClampDeg, 1.f, 80.f);
    g_cfgPlasmidAimPitch = CfgFloat("PlasmidAimPitch", g_cfgPlasmidAimPitch, -90.f, 90.f);
    g_cfgAimSmooth = CfgFloat("AimSmoothing", g_cfgAimSmooth, 0.f, 0.95f);
    g_cfgCrosshair = CfgBool("EnableCrosshair", true);
    g_cfgXhSize = CfgFloat("CrosshairSize", g_cfgXhSize, 0.0005f, 0.5f);
    g_cfgXhDist = CfgFloat("CrosshairDistance", g_cfgXhDist, 0.2f, 50.f);

    // controller
    g_cfgController = CfgBool("EnableController", true);
    g_cfgControllerMode = CfgIntRange("ControllerMode", 0, 0, 1);
    g_cfgControllerLayout = CfgIntRange("ControllerLayout", 0, 0, 1);
    g_cfgControllerPitch = CfgBool("ControllerPitch", false);
    g_cfgStickDeadzone = CfgFloat("ControllerDeadzone", g_cfgStickDeadzone, 0.f, 0.9f);
    g_cfgDpadModifier = CfgIntRange("ControllerDpadModifier", 1, 0, 3);
    g_cfgStickYToDpad = CfgBool("ControllerStickYToDpad", false);
    g_cfgControllerLog = CfgBool("ControllerLog", true);
    g_cfgJumpOnR3 = CfgBool("JumpOnR3", false);

    // menus
    g_cfgMenuScreen = CfgBool("EnableMenuScreen", true);
    g_cfgMenuSize = CfgFloat("MenuScreenSize", g_cfgMenuSize, 0.2f, 10.f);
    g_cfgMenuDist = CfgFloat("MenuScreenDistance", g_cfgMenuDist, 0.3f, 20.f);
    g_cfgMenuHeight = CfgFloat("MenuScreenHeight", g_cfgMenuHeight, -3.f, 3.f);
    g_cfgMenuMaxIndexed = CfgIntRange("MenuMaxIndexed", 8, 0, 100000);
    g_cfgMenuMaxDraw = CfgIntRange("MenuMaxDraw", 0, 0, 100000);
    CfgStr("MenuIndexCounts", "1769,63,49,95,21,87", g_cfgMenuList, sizeof(g_cfgMenuList));
    CfgStr("AnchorIndexCounts", "", g_cfgAnchorList, sizeof(g_cfgAnchorList));

    // debug / probe
    g_cfgDrawHook = CfgBool("EnableDrawHook", true);
    g_cfgGameState = CfgBool("EnableGameState", true);
    g_cfgHookInstanced = CfgBool("HookInstanced", false);
    CfgStr("SuppressIndexCounts", "", g_cfgSuppressList, sizeof(g_cfgSuppressList));
    CfgStr("IsolateCounts",
        "7425,3360,600,381,297,174,153,144,129,33,63021,9,105,139,83,7,2,17,23",
        g_cfgIsolateList, sizeof(g_cfgIsolateList));
    CfgStr("WeaponCounts", "", g_cfgWeaponList, sizeof(g_cfgWeaponList));
    g_cfgWeaponScale = CfgFloat("WeaponScale", g_cfgWeaponScale, 0.f, 4.f);
    CfgStr("HudCounts", "", g_cfgHudList, sizeof(g_cfgHudList));
    g_cfgHudScale = CfgFloat("HudScale", g_cfgHudScale, 0.f, 4.f);
    CfgStr("ArrowCounts", "", g_cfgArrowList, sizeof(g_cfgArrowList));
    g_cfgArrowScale = CfgFloat("ArrowScale", g_cfgArrowScale, 0.f, 4.f);
    g_cfgArrowX = CfgFloat("ArrowOffsetX", g_cfgArrowX, -2.f, 2.f);
    g_cfgArrowY = CfgFloat("ArrowOffsetY", g_cfgArrowY, -2.f, 2.f);
    g_cfgArrowPtrOff = CfgHex("ArrowPtrOffset", g_cfgArrowPtrOff);
    CfgVec3("ArrowWorldOffset", g_cfgArrowWorld);

    // ---- echo ----
    Log("=== BioshockVR config ===");

    Log("[core]");
    CfgEcho("GameFovDegrees", "%.1f", g_cfgFovDeg);
    CfgEcho("Resolution", "%d x %d  (%s)", g_cfgResX, g_cfgResY,
        (g_cfgResX && g_cfgResY) ? "written to Bioshock.ini" : "not managed");
    CfgEcho("EyeSeparation", "%.2f cm  (%.0f mm IPD)", g_cfgEyeSep, g_cfgEyeSep * 20.f);
    CfgEcho("SwapEyes", "%d", (int)g_cfgSwapEyes);
    CfgEcho("DisableVSync", "%d", (int)g_cfgDisableVSync);
    CfgEcho("ForceFlipModel", "%d  %s", (int)g_cfgForceFlip,
        g_cfgForceFlip ? "(rewriting swapchain for tearing)" : "(stock swapchain)");
    CfgEcho("MirrorPresentEvery", "%d  %s", g_cfgMirrorEvery,
        g_cfgMirrorEvery <= 0 ? "(time-throttled to ~58/s -- auto-tunes to any monitor)"
        : g_cfgMirrorEvery == 1 ? "(every frame == stock, compositor will cap you)"
        : "(fixed divisor)");
    CfgEcho("SyncGameIni", "%d", (int)g_cfgSyncGameIni);
    CfgEcho("Fullscreen", "%d  %s", g_cfgFullscreen,
        g_cfgFullscreen < 0 ? "(leaving the game's own choice alone)" :
        g_cfgFullscreen ? "(exclusive -- ignores the refresh cap)" : "(windowed)");

    Log("[camera]");
    CfgEcho("EnableCameraHook", "%d", (int)g_cfgCameraHook);
    CfgEcho("EnableCameraWrite", "%d  %s", (int)g_cfgCameraWrite,
        g_cfgCameraWrite ? "(camera WILL be modified)" : "(read-only, no stereo)");
    CfgEcho("EnableHeadTracking", "%d", (int)g_cfgHeadTracking);
    CfgEcho("EnableHeadPosition", "%d", (int)g_cfgHeadPosition);
    CfgEcho("EnableHeadRoll", "%d", (int)g_cfgHeadRoll);
    CfgEcho("EnableHeadAim", "%d", (int)g_cfgHeadAim);
    CfgEcho("HeadAimMode", "%d  %s", g_cfgHeadAimMode,
        g_cfgHeadAimMode == 0 ? "(legacy additive -- turn artifact)" :
        g_cfgHeadAimMode == 1 ? "(local compose, mouse pitch kept)" :
        "(local compose, PITCH DECOUPLED)");
    CfgEcho("PairLockCamera", "%d", (int)g_cfgPairLock);
    CfgEcho("CameraHeightOffset", "%.1f cm", g_cfgHeightOffset);
    CfgEcho("CutsceneTheater", "%d  %s", (int)g_cfgCutsceneTheater,
        g_cfgCutsceneTheater ? "(cutscenes on the flat quad)" : "(cutscenes in 3D)");
    CfgEcho("DeltaClamp", "%d  %s", g_cfgDeltaClamp,
        (g_cfgDeltaClamp == 2) ? "(BOTH worlds, one advance per eye pair)"
        : (g_cfgDeltaClamp == 1) ? "(player world only)" : "(off, one per eye)");

    Log("[weapon]");
    CfgEcho("ForegroundFov", "offset 0x%X  value %.1f  %s",
        g_cfgFgFovOffset, g_cfgFgFovValue,
        g_cfgFgFovAuto ? "(AUTO -- recomputed from the real backbuffer)" : "(fixed)");
    CfgEcho("WorldFov", "offset 0x%X / 0x%X   max %.1f  min %.1f  -> %.1f",
        g_cfgWorldFovOff, g_cfgWorldFovOff2, g_cfgWorldFovMax,
        g_cfgWorldFovMin, g_cfgWorldFovVal);
    CfgEcho("HandsScale", "%.2f", g_cfgHandsScale);
    CfgEcho("GunScale", "%.2f  ptr %s+0x%03X", g_cfgGunScale,
        g_cfgGunPtrBase ? "hands" : "pawn", (unsigned)g_cfgGunPtrOff);
    CfgEcho("GunChildren", "%d", g_cfgGunChildren);

    Log("[hands 6dof]");
    CfgEcho("Enable6DofHands", "%d", (int)g_cfg6DofHands);
    CfgEcho("EnableHandsProbe", "%d", (int)g_cfgHandsProbe);
    CfgEcho("HandsPtrOffset", "0x%X", g_cfgHandsPtrOff);
    CfgEcho("HandsPosOffset", "0x%X", g_cfgHandsPosOff);
    CfgEcho("HandsArm / Retry", "%d / %d calls  (~%.1f / %.1f s at 220/s)",
        g_cfgHandsArmCalls, g_cfgHandsRetryCalls,
        g_cfgHandsArmCalls / 220.0, g_cfgHandsRetryCalls / 220.0);
    CfgEcho("IdleAnimMode", "%d  %s", g_cfgIdleAnimMode,
        g_cfgIdleAnimMode == 0 ? "(off)" :
        g_cfgIdleAnimMode == 1 ? "(all entries -> entry[0], kills the wrench slap)" :
        g_cfgIdleAnimMode == 2 ? "(-> HandsOffscreenAnimationName, arms off screen)"
        : "(-> EquippingHandsAnim, no idle motion, arms visible)");
    CfgEcho("HandsGripOffset", "%.0f fwd, %.0f right, %.0f up (cm)",
        g_cfgHandsGrip[0], g_cfgHandsGrip[1], g_cfgHandsGrip[2]);
    CfgEcho("GripTunedFgFov", "%.1f  %s", g_cfgGripTunedFgFov,
        g_cfgGripTunedFgFov > 5.0f ? "(right/up auto-scaled to the live fg FOV)"
        : "(off -- offsets used exactly as written)");

    Log("[aim]");
    CfgEcho("AimSource", "%d  %s", g_cfgAimSource,
        g_cfgAimSource == 1 ? "(right controller)" : "(head)");
    CfgEcho("AimClampDeg", "%.0f", g_cfgAimClampDeg);
    CfgEcho("PlasmidAimPitch", "%.0f deg", g_cfgPlasmidAimPitch);
    for (int s = 0; s < 9; ++s)
        Log("  slot %d  pos %6.1f,%6.1f,%6.1f   rot %5.1f,%5.1f,%5.1f   cur %5.1f,%5.1f,%5.1f   idle %d  hide %d", s,
            g_cfgGripSlot[s][0], g_cfgGripSlot[s][1], g_cfgGripSlot[s][2],
            g_cfgRotSlot[s][0], g_cfgRotSlot[s][1], g_cfgRotSlot[s][2],
            g_cfgCursorSlot[s][0], g_cfgCursorSlot[s][1], g_cfgCursorSlot[s][2],
            g_cfgIdleModeSlot[s], g_cfgHideArmsSlot[s]);
    CfgEcho("AimSmoothing", "%.2f", g_cfgAimSmooth);
    CfgEcho("Crosshair", "%d  %.1f mm dot at %.2f m", (int)g_cfgCrosshair,
        g_cfgXhSize * 1000.f, g_cfgXhDist);

    Log("[controller]");
    CfgEcho("EnableController", "%d  mode=%s", (int)g_cfgController,
        g_cfgControllerMode ? "replace" : "merge");
    CfgEcho("ControllerLayout", "%d  %s", g_cfgControllerLayout,
        g_cfgControllerLayout ? "(jump right-A, use right-B)"
        : "(literal Xbox: jump left-Y, use right-A)");
    CfgEcho("ControllerPitch", "%d  %s", (int)g_cfgControllerPitch,
        g_cfgControllerPitch ? "(right-stick Y passed -- expect pitch fight)"
        : "(right-stick Y dropped -- correct for HeadAimMode=2)");
    CfgEcho("ControllerDeadzone", "%.2f", g_cfgStickDeadzone);
    CfgEcho("DpadModifier", "%d  %s", g_cfgDpadModifier,
        g_cfgDpadModifier == 0 ? "(off)" :
        g_cfgDpadModifier == 1 ? "(right thumbrest)" :
        g_cfgDpadModifier == 2 ? "(right stick click)" : "(left grip)");
    CfgEcho("StickYToDpad / Log", "%d / %d", (int)g_cfgStickYToDpad, (int)g_cfgControllerLog);
    CfgEcho("JumpOnR3", "%d  %s", (int)g_cfgJumpOnR3,
        g_cfgJumpOnR3 ? "(R3 jumps, zoom unbound)" : "(R3 is zoom, stock)");

    Log("[menus]");
    CfgEcho("EnableMenuScreen", "%d", (int)g_cfgMenuScreen);
    CfgEcho("MenuScreen", "%.2f m at %.2f m  (height %+.2f)",
        g_cfgMenuSize, g_cfgMenuDist, g_cfgMenuHeight);
    CfgEcho("MenuMaxIndexed", "%d", g_cfgMenuMaxIndexed);
    CfgEcho("MenuIndexCounts", "'%s'", g_cfgMenuList);
    CfgEcho("AnchorIndexCounts", "'%s'", g_cfgAnchorList);

    Log("[debug/probe]");
    CfgEcho("EnableDrawHook", "%d", (int)g_cfgDrawHook);
    CfgEcho("EnableGameState", "%d", (int)g_cfgGameState);
    CfgEcho("HookInstanced", "%d", (int)g_cfgHookInstanced);
    CfgEcho("SuppressIndexCounts", "'%s'", g_cfgSuppressList);
    CfgEcho("WeaponCounts", "'%s'  scale %.2f", g_cfgWeaponList, g_cfgWeaponScale);
    CfgEcho("HudCounts", "'%s'  scale %.2f", g_cfgHudList, g_cfgHudScale);
    CfgEcho("ArrowCounts", "'%s'  scale %.2f  offset %+.2f,%+.2f",
        g_cfgArrowList, g_cfgArrowScale, g_cfgArrowX, g_cfgArrowY);
    CfgEcho("ArrowPtrOffset", "0x%X   world %+.0f fwd %+.0f right %+.0f up (cm)",
        g_cfgArrowPtrOff, g_cfgArrowWorld[0], g_cfgArrowWorld[1], g_cfgArrowWorld[2]);

    Log("=========================");
}

// ============================================================================
//  INIT
// ============================================================================
// Real init happens off the loader lock. DllMain is not a safe place to
// allocate, hook, or touch other modules.
static DWORD WINAPI InitThread(LPVOID)
{
    Log("=== BioshockVR ===");
    Log("dllmain build: cleaned config  (%s %s)", __DATE__, __TIME__);

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