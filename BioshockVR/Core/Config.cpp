// BioshockVR/Core/Config.cpp
//
// Reading BioshockVR.ini into g_cfg, and echoing what was actually read.
//
// Split out of dllmain.cpp, which was carrying the settings for the whole mod
// alongside the entry point and the logger. The reads, the clamps, the
// cross-setting fixups and the echo are all here; nothing else in the mod parses
// the ini.
//
// The echo is not decoration. It is the only reliable answer to "did my setting
// take" -- a read-only ini, the wrong storefront profile, or VirtualStore
// redirection all leave the editor showing one thing and the mod reading
// another, and every one of those has cost a debugging session.

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Core/Config.h"

extern void LogFile(const char* msg);

VrConfig g_cfg;

// Retained from Config_Load so the live-tuning writer can find the same file.
static char g_iniPath[MAX_PATH] = {};

static void Log(const char* fmt, ...)
{
    char message[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
    va_end(args);
    LogFile(message);
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

// Live-tuned values written straight back to the ini, so a grip found in the
// headset cannot be lost by closing the game. Same "%.2f,%.2f,%.2f" shape
// CfgVec3 reads, so a value written here reloads exactly as it was.
void Cfg_WriteVec3(const char* key, const float v[3])
{
    if (!g_iniPath[0] || !key || !v) return;

    char b[64];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%.2f,%.2f,%.2f", v[0], v[1], v[2]);

    if (!WritePrivateProfileStringA("VR", key, b, g_iniPath))
        return;

    // Windows caches ini writes. Without this flush a crash or a hard exit
    // loses the value that was just "saved".
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_iniPath);
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

// Three signed 1-based lane selectors, "1,2,3" style. VALIDATED, not just
// parsed: a map that repeats or drops a lane builds a degenerate transform, and
// the symptom of that is a hand in the wrong place -- indistinguishable from the
// feature simply not working. Anything invalid keeps the caller's default.
static void CfgAxisMap(const char* key, int out[3])
{
    char b[64] = {};
    GetPrivateProfileStringA("VR", key, "", b, sizeof(b), g_iniPath);
    if (!b[0]) return;

    int v[3] = {};
    if (sscanf_s(b, "%d,%d,%d", &v[0], &v[1], &v[2]) != 3) return;

    int seen = 0;
    for (int i = 0; i < 3; ++i)
    {
        const int a = (v[i] < 0) ? -v[i] : v[i];
        if (a < 1 || a > 3) return;
        seen |= (1 << a);
    }
    if (seen != 0x0E)          // bits 1,2,3 -- each lane used exactly once
    {
        Log("  !!! %s: every lane must appear exactly once. Keeping the default.", key);
        return;
    }
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
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
void Config_Load(const char* iniPath)
{
    // Retained for Cfg_WriteVec3, which is the one path that writes back.
    if (iniPath) strncpy_s(g_iniPath, sizeof(g_iniPath), iniPath, _TRUNCATE);

    if (!g_iniPath[0]) { Log("config: no ini path. Using defaults."); return; }

    // ---- read ----
    // core
    g_cfg.fovDeg = CfgFloat("GameFovDegrees", g_cfg.fovDeg, 30.f, 170.f);
    g_cfg.resX = CfgIntRange("ResolutionX", 0, 0, 16384);
    g_cfg.resY = CfgIntRange("ResolutionY", 0, 0, 16384);
    g_cfg.eyeSep = CfgFloat("EyeSeparation", g_cfg.eyeSep, 0.f, 20.f);
    g_cfg.swapEyes = CfgBool("SwapEyes", false);
    g_cfg.disableVSync = CfgBool("DisableVSync", true);
    g_cfg.forceFlip = CfgBool("ForceFlipModel", false);
    g_cfg.mirrorEvery = CfgIntRange("MirrorPresentEvery", 1, 0, 240);
    g_cfg.syncGameIni = CfgBool("SyncGameIni", true);
    // Via CfgFloat, not CfgInt: CfgFloat's (name, default, min, max) form is
    // the one proven to compile here, and this needs the min/max clamp because
    // -1 / 0 / 1 are three distinct meanings and a typo must not become mode 7.
    g_cfg.fullscreen = CfgIntRange("Fullscreen", -1, -1, 1);

    // camera & comfort
    g_cfg.cameraHook = CfgBool("EnableCameraHook", true);
    g_cfg.cameraWrite = CfgBool("EnableCameraWrite", false);
    g_cfg.headTracking = CfgBool("EnableHeadTracking", false);
    g_cfg.headPosition = CfgBool("EnableHeadPosition", false);
    g_cfg.headRoll = CfgBool("EnableHeadRoll", true);
    g_cfg.headAim = CfgBool("EnableHeadAim", false);
    g_cfg.disableHeadBob = CfgBool("DisableHeadBob", false);
    g_cfg.headAimMode = CfgIntRange("HeadAimMode", 1, 0, 2);
    g_cfg.pairLock = CfgBool("PairLockCamera", true);
    g_cfg.heightOffset = CfgFloat("CameraHeightOffset", g_cfg.heightOffset, -100.f, 100.f);
    g_cfg.cutsceneTheater = CfgBool("CutsceneTheater", false);
    g_cfg.scriptedQol = CfgBool("ScriptedEventQol", false);
    g_cfg.deltaClamp = CfgInt("DeltaClamp", 0);
    g_cfg.hudAlphaFix = CfgBool("HudAlphaFix", true);
    g_cfg.hudDsvMode = CfgIntRange("HudDsvMode", g_cfg.hudDsvMode, 0, 2);

    // weapon & arm rendering
    g_cfg.fgFovOffset = CfgHex("ForegroundFovOffset", g_cfg.fgFovOffset);
    g_cfg.fgFovValue = CfgFloat("ForegroundFovValue", g_cfg.fgFovValue, 0.f, 360.f);
    g_cfg.fgFovAuto = CfgBool("ForegroundFovAuto", false);
    g_cfg.fgFovSrc = CfgHex("ForegroundFovSrcOffset", g_cfg.fgFovSrc);
    g_cfg.worldFovOff = CfgHex("WorldFovOffset", g_cfg.worldFovOff);
    g_cfg.worldFovOff2 = CfgHex("WorldFovOffset2", g_cfg.worldFovOff2);
    g_cfg.worldFovMax = CfgFloat("WorldFovMax", g_cfg.worldFovMax, 0.f, 170.f);
    g_cfg.worldFovMin = CfgFloat("WorldFovMin", g_cfg.worldFovMin, 0.f, 170.f);
    g_cfg.worldFovVal = CfgFloat("WorldFovValue", g_cfg.worldFovVal, 5.f, 170.f);
    g_cfg.handsScale = CfgFloat("HandsScale", g_cfg.handsScale, 0.05f, 5.f);
    g_cfg.gunScale = CfgFloat("GunScale", g_cfg.gunScale, 0.05f, 5.f);
    g_cfg.gunPtrOff = CfgHex("GunPtrOffset", g_cfg.gunPtrOff);
    g_cfg.gunPtrBase = CfgInt("GunPtrBase", 1);
    g_cfg.gunChildren = CfgInt("GunChildren", 0);

    // 6-DOF hands
    g_cfg.sixDofHands = CfgBool("Enable6DofHands", false);
    g_cfg.hideArmSleeves = CfgBool("HideArmSleeves", false);
    g_cfg.armHideHandsVt = CfgHex("ArmHideHandsVt", g_cfg.armHideHandsVt);
    g_cfg.armHideSkelVt = CfgHex("ArmHideSkelVt", g_cfg.armHideSkelVt);
    g_cfg.handRigProbe = CfgBool("HandRigProbe", false);
    g_cfg.plasmidProbe = CfgBool("PlasmidProbe", true);
    // LeftHandTracked first, as the DEFAULT for OffHandTracked -- so an ini
    // written before the right hand joined keeps working, and a file carrying
    // both still lets the current name win.
    g_cfg.offHandTracked = CfgIntRange("LeftHandTracked", 0, 0, 3);
    g_cfg.offHandTracked = CfgIntRange("OffHandTracked", g_cfg.offHandTracked, 0, 3);
    g_cfg.weaponHandDrive = CfgIntRange("WeaponHandDrive", 0, 0, 1);
    g_cfg.leftHanded = CfgIntRange("LeftHanded", 0, 0, 1);
    g_cfg.leftHandedSwapSticks = CfgIntRange("LeftHandedSwapSticks", 1, 0, 1);
    g_cfg.handAnim = CfgIntRange("HandAnim", 0, 0, 1);
    g_cfg.handAnimMinDeg = CfgIntRange("HandAnimMinDeg", 5, 0, 180);
    g_cfg.handAnimHoldMs = CfgIntRange("HandAnimHoldMs", 1200, 0, 10000);
    for (int s3 = 0; s3 < 9; ++s3)
    {
        char key[32];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "HandAnimSlot%d", s3);
        // Slot 0 is the wrench and defaults OFF even when HandAnim is on: its
        // swing animation fights the motion-controlled swing, and that is a
        // recorded decision rather than a taste call.
        const int def = (s3 == 0) ? 0 : g_cfg.handAnim;
        g_cfg.handAnimSlot[s3] = CfgIntRange(key, def, 0, 1);
    }
    g_cfg.bone43Rot = CfgIntRange("WeaponHandBone43Rot", 1, 0, 1);
    g_cfg.weaponSwitchSettleMs = CfgIntRange("WeaponSwitchSettleMs", 600, 0, 5000);
    g_cfg.twoHandGrip = CfgIntRange("TwoHandGrip", 0, 0, 1);
    g_cfg.twoHandGrab = CfgIntRange("TwoHandGrabRadius", 12, 1, 200);
    g_cfg.twoHandRelease = CfgIntRange("TwoHandReleaseRadius", 20, 1, 300);
    g_cfg.twoHandBlockRadial = CfgIntRange("TwoHandBlockRadial", 0, 0, 1);
    g_cfg.hapticGrabZone = CfgIntRange("HapticGrabZone", 1, 0, 1);
    g_cfg.hapticGrabMs = CfgIntRange("HapticGrabMs", 60, 1, 500);
    g_cfg.hapticGrabEveryMs = CfgIntRange("HapticGrabEveryMs", 50, 10, 1000);
    g_cfg.hapticGrabAmp = CfgFloat("HapticGrabAmp", 0.35f, 0.f, 1.f);
    g_cfg.hapticRecoil = CfgIntRange("HapticRecoil", 1, 0, 1);
    g_cfg.hapticRecoilMinDeg = CfgIntRange("HapticRecoilMinDeg", 6, 1, 180);
    g_cfg.hapticRecoilMs = CfgIntRange("HapticRecoilMs", 45, 1, 500);
    g_cfg.hapticRecoilAmp = CfgFloat("HapticRecoilAmp", 1.0f, 0.f, 1.f);
    g_cfg.twoHandToggle = CfgIntRange("TwoHandToggle", 0, 0, 1);
    g_cfg.twoHandProbe = CfgIntRange("TwoHandProbe", 1, 0, 1);
    for (int s2 = 0; s2 < 9; ++s2)
    {
        char key[32];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "TwoHandable%d", s2);
        // Slots 2 and 5 are the two-handed weapons -- the only ones the game
        // itself poses the off hand onto -- so they are the default yes.
        g_cfg.twoHandable[s2] = CfgIntRange(key, (s2 == 2 || s2 == 5) ? 1 : 0, 0, 1);
    }
    CfgAxisMap("LeftHandAxisMap", g_cfg.leftHandAxis);
    CfgVec3("LeftHandOffset", g_cfg.leftHandOffset);
    CfgVec3("LeftHandRot", g_cfg.leftHandRot);
    CfgVec3("RightHandOffset", g_cfg.rightHandOffset);
    CfgVec3("RightHandRot", g_cfg.rightHandRot);
    g_cfg.handsProbe = CfgBool("EnableHandsProbe", false);
    g_cfg.handsPtrOff = CfgHex("HandsPtrOffset", g_cfg.handsPtrOff);
    g_cfg.handsPosOff = CfgHex("HandsPosOffset", g_cfg.handsPosOff);
    CfgVec3("HandsGripOffset", g_cfg.handsGrip);
    g_cfg.gripTunedFgFov = CfgFloat("GripTunedFgFov", g_cfg.gripTunedFgFov, 0.f, 360.f);
    CfgVec3("HandsRotOffset", g_cfg.handsRot);
    g_cfg.handsArmCalls = CfgIntRange("HandsArmCalls", 600, 1, 5000);
    g_cfg.handsRetryCalls = CfgIntRange("HandsRetryCalls", 600, 1, 5000);
    g_cfg.idleAnimMode = CfgIntRange("IdleAnimMode", 0, 0, 3);

    // Per-weapon tables. Slot order is AllPossibleWeaponClasses -- 0 Wrench,
    // 1 Pistol, 2 Shotgun, 3 Crossbow, 4 GrenadeLauncher, 5 MachineGun,
    // 6 ChemicalThrower, 7 ResearchCamera -- plus 8 = plasmid. Each slot is
    // seeded from the two globals first, so a slot with no ini key inherits them
    // instead of snapping to zero.
    for (int s = 0; s < 9; ++s)
    {
        for (int a = 0; a < 3; ++a)
        {
            g_cfg.gripSlot[s][a] = g_cfg.handsGrip[a];
            g_cfg.rotSlot[s][a] = g_cfg.handsRot[a];
            g_cfg.cursorSlot[s][a] = 0.0f;
        }
        char key[32];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "GripOffset%d", s);
        CfgVec3(key, g_cfg.gripSlot[s]);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "RotOffset%d", s);
        CfgVec3(key, g_cfg.rotSlot[s]);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "CursorOffset%d", s);
        CfgVec3(key, g_cfg.cursorSlot[s]);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "IdleAnimMode%d", s);
        g_cfg.idleModeSlot[s] = CfgIntRange(key, g_cfg.idleAnimMode, 0, 3);

        // HideArmsN: auto-arm the SuppressIndexCounts list while this weapon is
        // held. Guns hide the arms; the wrench and plasmids keep them.
        _snprintf_s(key, sizeof(key), _TRUNCATE, "HideArms%d", s);
        g_cfg.hideArmsSlot[s] = CfgIntRange(key, 0, 0, 1);

        // Per-weapon inactive-hand override. Defaults to the global setting, so
        // a slot with no key in the ini behaves exactly as it does today.
        _snprintf_s(key, sizeof(key), _TRUNCATE, "HideInactiveHand%d", s);
        g_cfg.hideHandSlot[s] = CfgIntRange(key, g_cfg.hideInactiveHand, 0, 1);
    }
    g_cfg.handsNudgeZ = CfgFloat("HandsNudgeZ", g_cfg.handsNudgeZ, -500.f, 500.f);
    g_cfg.handsNudgeYaw = CfgFloat("HandsNudgeYaw", g_cfg.handsNudgeYaw, -180.f, 180.f);
    g_cfg.handsNudgePitch = CfgFloat("HandsNudgePitch", g_cfg.handsNudgePitch, -180.f, 180.f);

    // aiming / crosshair
    g_cfg.aimSource = CfgInt("AimSource", 0);
    g_cfg.headAimUnarmed = CfgIntRange("HeadAimWhenUnarmed", 1, 0, 1);
    g_cfg.aimClampDeg = CfgFloat("AimClampDeg", g_cfg.aimClampDeg, 1.f, 80.f);
    g_cfg.plasmidAimPitch = CfgFloat("PlasmidAimPitch", g_cfg.plasmidAimPitch, -90.f, 90.f);
    g_cfg.aimSmooth = CfgFloat("AimSmoothing", g_cfg.aimSmooth, 0.f, 0.95f);
    g_cfg.hudRedirect = CfgBool("HudRedirect", true);
    g_cfg.hudWidthDeg = CfgFloat("HudWidthDeg", g_cfg.hudWidthDeg, 10.f, 140.f);
    g_cfg.hudDist = CfgFloat("HudDistance", g_cfg.hudDist, 0.4f, 8.f);
    g_cfg.hudPitchDeg = CfgFloat("HudPitchDeg", g_cfg.hudPitchDeg, -60.f, 60.f);
    g_cfg.hudYawDeg = CfgFloat("HudYawDeg", g_cfg.hudYawDeg, -90.f, 90.f);
    g_cfg.crosshair = CfgBool("EnableCrosshair", true);
    g_cfg.xhSize = CfgFloat("CrosshairSize", g_cfg.xhSize, 0.0005f, 0.5f);
    g_cfg.xhDist = CfgFloat("CrosshairDistance", g_cfg.xhDist, 0.2f, 50.f);
    g_cfg.disableReticle = CfgBool("DisableReticle", true);
    g_cfg.engPtrRva = CfgHex("EnginePtrRva", g_cfg.engPtrRva);
    g_cfg.engVtRva = CfgHex("EngineVtableRva", g_cfg.engVtRva);
    g_cfg.engExecRva = CfgHex("EngineExecRva", g_cfg.engExecRva);
    g_cfg.engExecThis = CfgHex("EngineExecThis", g_cfg.engExecThis);

    // controller
    g_cfg.controller = CfgBool("EnableController", true);
    g_cfg.controllerMode = CfgIntRange("ControllerMode", 1, 0, 1);
    // Default 1, matching the struct. It read 0 here while the global said 1 --
    // two different answers to "what is the default", which only ever showed up
    // for a user whose ini lacked the key, because the shipped ini sets it.
    // Consolidating the two into one declaration is what made the mismatch
    // visible; this is the only intentional behaviour change in the refactor.
    g_cfg.controllerLayout = CfgIntRange("ControllerLayout", 1, 0, 1);
    g_cfg.controllerPitch = CfgBool("ControllerPitch", false);
    g_cfg.stickDeadzone = CfgFloat("ControllerDeadzone", g_cfg.stickDeadzone, 0.f, 0.9f);
    g_cfg.dpadModifier = CfgIntRange("ControllerDpadModifier", 1, 0, 4);
    g_cfg.hideInactiveHand = CfgIntRange("HideInactiveHand", 1, 0, 1);
    g_cfg.hideCutsceneBars = CfgIntRange("HideCutsceneBars", 1, 0, 1);
    g_cfg.cutsceneBarVerts = CfgIntRange("CutsceneBarVertices", 29, 1, 4096);
    g_cfg.swingEnabled = CfgIntRange("SwingEnabled", 1, 0, 1);
    g_cfg.swingThreshold = CfgFloat("SwingThreshold", 2.0f, 0.5f, 20.0f);
    g_cfg.swingRearm = CfgFloat("SwingRearm", 1.5f, 0.1f, 10.0f);
    g_cfg.swingCooldownMs = CfgIntRange("SwingCooldownMs", 180, 0, 5000);
    g_cfg.swingPulseMs = CfgIntRange("SwingPulseMs", 120, 20, 1000);
    g_cfg.swingDelayMs = CfgIntRange("SwingDelayMs", 0, 0, 1000);
    g_cfg.swingLog = CfgIntRange("SwingLog", 0, 0, 1);
    g_cfg.swingOutFrac = CfgFloat("SwingOutwardFraction", 0.60f, 0.0f, 1.0f);
    g_cfg.swingTravel = CfgFloat("SwingTravelMetres", 0.15f, 0.0f, 1.0f);
    g_cfg.gripThreshold = CfgFloat("GripThreshold", 0.80f, 0.30f, 0.99f);
    g_cfg.gripHysteresis = CfgFloat("GripHysteresis", 0.15f, 0.00f, 0.50f);
    g_cfg.headRelativeMove = CfgIntRange("HeadRelativeMove", 1, 0, 1);

    // MUST come after AimSource and HeadRelativeMove: those two seed it, so an
    // ini written before MovementMode existed keeps behaving exactly as it did.
    //
    // THE NUMBERS CHANGED MEANING on 2026-08-11, when the modes went from three
    // to four and stopped deciding who aims. Old 0/1/2 (head, both, controller)
    // are new 2/3/1. Both shipped ini files were rewritten in the same commit
    // rather than reinterpreted silently -- and the echo below prints the mode
    // NAME, so a stale number is visible in the log rather than felt in a
    // headset. The seed below is remapped to match.
    {
        const int seed = (g_cfg.aimSource == 1)
            ? (g_cfg.headRelativeMove ? 3 : 1)
            : 2;
        g_cfg.movementMode = CfgIntRange("MovementMode", seed, 0, 3);
    }

    // Head aim used to BE movement mode 0. An ini written before the split says
    // AimSource=0 to mean "no controller aim", which is the same request.
    g_cfg.headAimAlways = CfgIntRange("HeadAimAlways",
        (g_cfg.aimSource == 1) ? 0 : 1, 0, 1);
    g_cfg.flashGuiProbe = CfgIntRange("FlashGuiProbe", 1, 0, 1);
    g_cfg.turnRateProbe = CfgIntRange("TurnRateProbe", 1, 0, 1);
    g_cfg.scriptedRotProbe = CfgIntRange("ScriptedRotProbe", 1, 0, 1);
    g_cfg.scriptedCameraFollow = CfgIntRange("ScriptedCameraFollow", 1, 0, 1);
    g_cfg.scriptedRecentre = CfgIntRange("ScriptedRecentre", 0, 0, 2);
    g_cfg.walkDriftProbe = CfgIntRange("WalkDriftProbe", 1, 0, 1);
    g_cfg.gameTurnSpeed = CfgIntRange("GameTurnSpeed", 70, -1, 100);

    // ExecCommand1..8. ONE-BASED, matching the ini's own GripOffset/HideArms
    // convention and what a non-programmer would write first.
    for (int i = 0; i < kExecCommands; ++i)
    {
        char key[32];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "ExecCommand%d", i + 1);
        CfgStr(key, "", g_cfg.execCommand[i], sizeof(g_cfg.execCommand[i]));
    }

    g_cfg.stickPrecomp = CfgIntRange("StickPrecomp", 1, 0, 1);
    g_cfg.gameStickDeadzone =
        CfgFloat("GameStickDeadzone", g_cfg.gameStickDeadzone, 0.0f, 0.90f);

    g_cfg.turnAxisMax = CfgFloat("TurnAxisMax", g_cfg.turnAxisMax, 0.20f, 1.0f);
    g_cfg.turnAxisExp = CfgFloat("TurnAxisExp", g_cfg.turnAxisExp, 0.50f, 4.0f);

    g_cfg.snapTurn = CfgIntRange("SnapTurn", 0, 0, 1);
    g_cfg.snapTurnDeg = CfgFloat("SnapTurnDegrees", 45.0f, 5.0f, 180.0f);
    g_cfg.freezeGameRot = CfgIntRange("FreezeGameRotation", 0, 0, 1);
    g_cfg.freezeGameplayRot = CfgIntRange("FreezeGameplayRotation", 1, 0, 1);
    g_cfg.scriptedRotFollow = CfgIntRange("ScriptedRotationFollow", 1, 0, 1);
    g_cfg.controllableScripted = CfgIntRange("ControllableScriptedFix", 1, 0, 1);
    g_cfg.scriptedProbe = CfgBool("ScriptedWindowProbe", true);
    g_cfg.scriptedHandsMotion =
        CfgFloat("ScriptedHandsMotionThreshold", 0.02f, 0.0001f, 10.0f);
    g_cfg.scriptedHandsHoldMs =
        CfgIntRange("ScriptedHandsHoldMs", 300, 0, 10000);
    g_cfg.scriptedWindowHoldMs =
        CfgIntRange("ScriptedWindowHoldMs", 250, 0, 5000);
    g_cfg.modYaw = CfgIntRange("ModYaw", 0, 0, 1);
    g_cfg.modYawSpeed = CfgFloat("ModYawSpeed", 90.0f, 15.0f, 360.0f);
    g_cfg.forceFocus = CfgIntRange("ForceWindowFocus", 1, 0, 1);
    g_cfg.pitchServo = CfgIntRange("PitchServo", 0, 0, 1);
    g_cfg.pitchServoGain = CfgFloat("PitchServoGain", 0.030f, 0.001f, 0.500f);
    g_cfg.pitchServoDead = CfgFloat("PitchServoDeadzoneDeg", 2.0f, 0.0f, 30.0f);
    g_cfg.pitchServoMax = CfgFloat("PitchServoMax", 0.80f, 0.05f, 1.00f);
    g_cfg.xhFromShot = CfgIntRange("CrosshairFromShot", 1, 0, 1);
    g_cfg.dpadFlip = CfgIntRange("ControllerDpadFlip", 0, 0, 1);
    g_cfg.stickYToDpad = CfgBool("ControllerStickYToDpad", false);
    g_cfg.controllerLog = CfgBool("ControllerLog", true);
    g_cfg.pauseChord = CfgIntRange("ControllerPauseChord", 1, 0, 1);
    g_cfg.jumpOnR3 = CfgBool("JumpOnR3", false);

    // R3 cannot both jump and be the D-pad modifier. Quest 1 and Quest 2
    // controllers have no thumbrest sensor, so mode 2 is their only D-pad
    // option and it has to win.
    if (g_cfg.dpadModifier == 2 && g_cfg.jumpOnR3)
    {
        g_cfg.jumpOnR3 = false;
        Log("!!! JumpOnR3 forced OFF -- ControllerDpadModifier=2 needs R3.");
    }

    // menus
    g_cfg.menuScreen = CfgBool("EnableMenuScreen", true);
    g_cfg.menuSize = CfgFloat("MenuScreenSize", g_cfg.menuSize, 0.2f, 10.f);
    g_cfg.menuDist = CfgFloat("MenuScreenDistance", g_cfg.menuDist, 0.3f, 20.f);
    g_cfg.menuHeight = CfgFloat("MenuScreenHeight", g_cfg.menuHeight, -3.f, 3.f);
    g_cfg.menuMaxIndexed = CfgIntRange("MenuMaxIndexed", 8, 0, 100000);
    g_cfg.menuMaxDraw = CfgIntRange("MenuMaxDraw", 0, 0, 100000);
    CfgStr("MenuIndexCounts", "1769,63,49,95,21,87", g_cfg.menuList, sizeof(g_cfg.menuList));
    CfgStr("AnchorIndexCounts", "", g_cfg.anchorList, sizeof(g_cfg.anchorList));
    CfgStr("AnchorMovies", "", g_cfg.anchorMovies, sizeof(g_cfg.anchorMovies));
    CfgStr("SceneMovies", "", g_cfg.sceneMovies, sizeof(g_cfg.sceneMovies));
    CfgStr("FollowMovies", "", g_cfg.followMovies, sizeof(g_cfg.followMovies));
    CfgStr("PanelMovies", "", g_cfg.panelMovies, sizeof(g_cfg.panelMovies));
    g_cfg.waterProbe = CfgIntRange("WaterProbe", 1, 0, 1);

    // debug / probe
    g_cfg.drawHook = CfgBool("EnableDrawHook", true);
    g_cfg.gameState = CfgBool("EnableGameState", true);
    // DEFAULT ON, and that is a deliberate exception to "new behaviour ships
    // default-off". The scan is read-only, hooks nothing, writes nothing and
    // gates nothing -- its entire product IS the log line, so shipping it off
    // would waste the headset cycle it exists to spend. Same reasoning as the
    // MyHudTick/CineTick probes. The switch is here to kill it without a
    // rebuild, not to hide it.
    g_cfg.nativeScan = CfgBool("EnableNativeScan", true);
    // DEFAULT OFF, unlike EnableNativeScan above, and the difference is the
    // point: that one is a one-shot locate, this one samples two windows four
    // times a second for the whole session. Diagnostic, read-only, gates
    // nothing -- but it is the only periodic diff in the mod, so it is opt-in.
    g_cfg.forcedMoveProbe = CfgBool("EnableForcedMoveProbe", false);
    g_cfg.forcedMoveProbeAll = CfgBool("ForcedMoveProbeAll", false);
    g_cfg.hookInstanced = CfgBool("HookInstanced", false);
    CfgStr("SuppressIndexCounts", "", g_cfg.suppressList, sizeof(g_cfg.suppressList));
    CfgStr("IsolateCounts",
        "7425,3360,600,381,297,174,153,144,129,33,63021,9,105,139,83,7,2,17,23",
        g_cfg.isolateList, sizeof(g_cfg.isolateList));
    CfgStr("WeaponCounts", "", g_cfg.weaponList, sizeof(g_cfg.weaponList));
    g_cfg.weaponScale = CfgFloat("WeaponScale", g_cfg.weaponScale, 0.f, 4.f);
    CfgStr("HudCounts", "", g_cfg.hudList, sizeof(g_cfg.hudList));
    g_cfg.hudScale = CfgFloat("HudScale", g_cfg.hudScale, 0.f, 4.f);
    CfgStr("ArrowCounts", "", g_cfg.arrowList, sizeof(g_cfg.arrowList));
    g_cfg.arrowScale = CfgFloat("ArrowScale", g_cfg.arrowScale, 0.f, 4.f);
    g_cfg.arrowX = CfgFloat("ArrowOffsetX", g_cfg.arrowX, -2.f, 2.f);
    g_cfg.arrowY = CfgFloat("ArrowOffsetY", g_cfg.arrowY, -2.f, 2.f);
    g_cfg.arrowPtrOff = CfgHex("ArrowPtrOffset", g_cfg.arrowPtrOff);
    CfgVec3("ArrowWorldOffset", g_cfg.arrowWorld);
    g_cfg.arrowUnparentRot = CfgIntRange("ArrowUnparentRot", 0, 0, 1);
    g_cfg.arrowProbe = CfgIntRange("ArrowProbe", 1, 0, 1);
    g_cfg.arrowDrawScale = CfgFloat("ArrowDrawScale", 0.0f, 0.0f, 20.0f);
    g_cfg.mirrorOneEye = CfgIntRange("MirrorOneEye", 1, 0, 1);
    g_cfg.arrowHideScripted = CfgIntRange("ArrowHideScripted", 1, 0, 1);
    g_cfg.arrowLevel = CfgIntRange("ArrowLevel", 1, 0, 2);

    // ---- echo ----
    Log("=== BioshockVR config ===");

    Log("[core]");
    CfgEcho("GameFovDegrees", "%.1f", g_cfg.fovDeg);
    CfgEcho("Resolution", "%d x %d  (%s)", g_cfg.resX, g_cfg.resY,
        (g_cfg.resX && g_cfg.resY) ? "written to Bioshock.ini" : "not managed");
    CfgEcho("EyeSeparation", "%.2f cm  (%.0f mm IPD)", g_cfg.eyeSep, g_cfg.eyeSep * 20.f);
    CfgEcho("SwapEyes", "%d", (int)g_cfg.swapEyes);
    CfgEcho("DisableVSync", "%d", (int)g_cfg.disableVSync);
    CfgEcho("ForceFlipModel", "%d  %s", (int)g_cfg.forceFlip,
        g_cfg.forceFlip ? "(rewriting swapchain for tearing)" : "(stock swapchain)");
    CfgEcho("MirrorPresentEvery", "%d  %s", g_cfg.mirrorEvery,
        g_cfg.mirrorEvery <= 0 ? "(time-throttled to ~58/s -- auto-tunes to any monitor)"
        : g_cfg.mirrorEvery == 1 ? "(every frame == stock, compositor will cap you)"
        : "(fixed divisor)");
    CfgEcho("SyncGameIni", "%d", (int)g_cfg.syncGameIni);
    CfgEcho("Fullscreen", "%d  %s", g_cfg.fullscreen,
        g_cfg.fullscreen < 0 ? "(leaving the game's own choice alone)" :
        g_cfg.fullscreen ? "(exclusive -- ignores the refresh cap)" : "(windowed)");

    Log("[camera]");
    CfgEcho("EnableCameraHook", "%d", (int)g_cfg.cameraHook);
    CfgEcho("EnableCameraWrite", "%d  %s", (int)g_cfg.cameraWrite,
        g_cfg.cameraWrite ? "(camera WILL be modified)" : "(read-only, no stereo)");
    CfgEcho("EnableHeadTracking", "%d", (int)g_cfg.headTracking);
    CfgEcho("EnableHeadPosition", "%d", (int)g_cfg.headPosition);
    CfgEcho("EnableHeadRoll", "%d", (int)g_cfg.headRoll);
    CfgEcho("EnableHeadAim", "%d", (int)g_cfg.headAim);
    CfgEcho("DisableHeadBob", "%d", (int)g_cfg.disableHeadBob);
    CfgEcho("HeadAimMode", "%d  %s", g_cfg.headAimMode,
        g_cfg.headAimMode == 0 ? "(legacy additive -- turn artifact)" :
        g_cfg.headAimMode == 1 ? "(local compose, mouse pitch kept)" :
        "(local compose, PITCH DECOUPLED)");
    CfgEcho("PairLockCamera", "%d", (int)g_cfg.pairLock);
    CfgEcho("CameraHeightOffset", "%.1f cm", g_cfg.heightOffset);
    CfgEcho("CutsceneTheater", "%d  %s", (int)g_cfg.cutsceneTheater,
        g_cfg.cutsceneTheater ? "(cutscenes on the flat quad)" : "(cutscenes in 3D)");
    CfgEcho("ScriptedEventQol", "%d  %s", (int)g_cfg.scriptedQol,
        g_cfg.scriptedQol ? "(arms shown, hands and aim released)" : "(off)");
    CfgEcho("FreezeGameplayRotation", "%d  %s", g_cfg.freezeGameplayRot,
        g_cfg.freezeGameplayRot
        ? "(no shake/kick/auto-pan; scripted, forced-move and bathysphere free)"
        : "(off)");
    CfgEcho("ScriptedRotationFollow", "%d  %s", g_cfg.scriptedRotFollow,
        g_cfg.scriptedRotFollow ? "(cutscenes turn you)"
        : "(view holds still; turn yourself)");
    CfgEcho("ScriptedHandsMotionThreshold", "%.4f", g_cfg.scriptedHandsMotion);
    CfgEcho("ScriptedHandsHoldMs", "%d   (hands stay up this long after the rig "
        "stops)", g_cfg.scriptedHandsHoldMs);
    CfgEcho("ScriptedWindowHoldMs", "%d   %s", g_cfg.scriptedWindowHoldMs,
        g_cfg.scriptedWindowHoldMs
        ? "(one window per scene -- bridges the gap between a forced move and "
          "the animation that follows it)"
        : "(OFF -- the window closes the instant both signals drop)");
    CfgEcho("ControllableScriptedFix", "%d  %s", g_cfg.controllableScripted,
        g_cfg.controllableScripted ? "(a sequence with the HUD up keeps your aim)"
        : "(off)");
    CfgEcho("ScriptedWindowProbe", "%d", (int)g_cfg.scriptedProbe);
    CfgEcho("DeltaClamp", "%d  %s", g_cfg.deltaClamp,
        (g_cfg.deltaClamp == 2) ? "(BOTH worlds, one advance per eye pair)"
        : (g_cfg.deltaClamp == 1) ? "(player world only)" : "(off, one per eye)");

    Log("[weapon]");
    CfgEcho("ForegroundFov", "offset 0x%X  value %.1f  %s",
        g_cfg.fgFovOffset, g_cfg.fgFovValue,
        g_cfg.fgFovAuto ? "(AUTO -- recomputed from the real backbuffer)" : "(fixed)");
    CfgEcho("WorldFov", "offset 0x%X / 0x%X   max %.1f  min %.1f  -> %.1f",
        g_cfg.worldFovOff, g_cfg.worldFovOff2, g_cfg.worldFovMax,
        g_cfg.worldFovMin, g_cfg.worldFovVal);
    CfgEcho("HandsScale", "%.2f", g_cfg.handsScale);
    CfgEcho("GunScale", "%.2f  ptr %s+0x%03X", g_cfg.gunScale,
        g_cfg.gunPtrBase ? "hands" : "pawn", (unsigned)g_cfg.gunPtrOff);
    CfgEcho("GunChildren", "%d", g_cfg.gunChildren);

    Log("[hands 6dof]");
    CfgEcho("Enable6DofHands", "%d", (int)g_cfg.sixDofHands);
    CfgEcho("HideArmSleeves", "%d   hands vt 0x%X  skel vt 0x%X",
        (int)g_cfg.hideArmSleeves, g_cfg.armHideHandsVt, g_cfg.armHideSkelVt);
    CfgEcho("HandRigProbe", "%d", (int)g_cfg.handRigProbe);
    CfgEcho("PlasmidProbe", "%d", (int)g_cfg.plasmidProbe);
    CfgEcho("OffHandTracked", "%d  %s", g_cfg.offHandTracked,
        g_cfg.offHandTracked == 0 ? "(off)" :
        g_cfg.offHandTracked == 1 ? "(position)" :
        g_cfg.offHandTracked == 2 ? "(position + rotation)" : "(AXIS SWEEP)");
    CfgEcho("LeftHanded", "%d  %s", g_cfg.leftHanded,
        g_cfg.leftHanded ? "(gun in the LEFT hand -- RESTART REQUIRED to change)"
        : "(right-handed)");
    CfgEcho("LeftHandedSwapSticks", "%d  %s", g_cfg.leftHandedSwapSticks,
        g_cfg.leftHandedSwapSticks ? "(move and turn follow the flip)"
        : "(movement stays on the physical left stick)");
    CfgEcho("WeaponHandDrive", "%d  %s", g_cfg.weaponHandDrive,
        g_cfg.weaponHandDrive ? "(the weapon hand is FROZEN -- no gun sway)"
        : "(off; the game animates the weapon hand)");
    CfgEcho("HandAnim", "%d  %s", g_cfg.handAnim,
        g_cfg.handAnim ? "(adopt the engine's animation when it restamps)"
        : "(rigid; reference frozen at capture)");
    if (g_cfg.handAnim)
    {
        CfgEcho("HandAnim tuning", "min %d deg, hold %d ms  (below the threshold "
            "is breathing and stays rigid)",
            g_cfg.handAnimMinDeg, g_cfg.handAnimHoldMs);
        char slots[64] = {};
        int n = 0;
        for (int i = 0; i < 9 && n < 60; ++i)
        {
            if (i) slots[n++] = ',';
            slots[n++] = g_cfg.handAnimSlot[i] ? '1' : '0';
        }
        slots[n] = 0;
        CfgEcho("HandAnimSlot", "%s   (slot 0 is the wrench and is OFF by design)",
            slots);
    }
    CfgEcho("TwoHandGrip", "%d  %s", g_cfg.twoHandGrip,
        g_cfg.twoHandGrip ? "(off-hand grip near the fore-end two-hands the weapon)"
        : "(off)");
    if (g_cfg.twoHandGrip || g_cfg.twoHandProbe)
        CfgEcho("TwoHand tuning", "grab %d cm, release %d cm, %s, probe %d",
            g_cfg.twoHandGrab, g_cfg.twoHandRelease,
            g_cfg.twoHandToggle ? "TOGGLE" : "hold", g_cfg.twoHandProbe);
    if (g_cfg.twoHandGrip)
        CfgEcho("TwoHandBlockRadial", "%d  %s", g_cfg.twoHandBlockRadial,
            g_cfg.twoHandBlockRadial
            ? "(the grip NEVER opens the plasmid wheel -- and so cannot switch "
              "plasmids either, on a grabbable weapon)"
            : "(the wheel opens normally outside the grab zone)");
    CfgEcho("HapticGrabZone", "%d  %s", g_cfg.hapticGrabZone,
        g_cfg.hapticGrabZone ? "(steady buzz while the off hand is at the fore-end)"
        : "(off)");
    CfgEcho("HapticRecoil", "%d  %s", g_cfg.hapticRecoil,
        g_cfg.hapticRecoil ? "(kick on the weapon hand when the gun fires -- from "
        "the recoil animation, so it needs WeaponHandDrive=1)" : "(off)");
    CfgEcho("WeaponHandBone43Rot", "%d  %s", g_cfg.bone43Rot,
        g_cfg.bone43Rot ? "(the GUN is frozen to the hand -- attach bone rotation "
        "written; set 0 if the gun breaks)"
        : "(attach bone rotation left to the engine -- the gun will sway)");
    CfgEcho("WeaponSwitchSettleMs", "%d ms  (equip animation plays, then the pose "
        "is captured)", g_cfg.weaponSwitchSettleMs);
    CfgEcho("LeftHandAxisMap", "%d,%d,%d   (1 fwd, 2 right, 3 up; signed)",
        g_cfg.leftHandAxis[0], g_cfg.leftHandAxis[1], g_cfg.leftHandAxis[2]);
    CfgEcho("LeftHandOffset", "%.1f fwd, %.1f right, %.1f up (cm)",
        g_cfg.leftHandOffset[0], g_cfg.leftHandOffset[1], g_cfg.leftHandOffset[2]);
    CfgEcho("LeftHandRot", "%.1f, %.1f, %.1f  (pitch,yaw,roll deg)",
        g_cfg.leftHandRot[0], g_cfg.leftHandRot[1], g_cfg.leftHandRot[2]);
    CfgEcho("RightHandOffset", "%.1f fwd, %.1f right, %.1f up (cm)",
        g_cfg.rightHandOffset[0], g_cfg.rightHandOffset[1], g_cfg.rightHandOffset[2]);
    CfgEcho("RightHandRot", "%.1f, %.1f, %.1f  (pitch,yaw,roll deg)",
        g_cfg.rightHandRot[0], g_cfg.rightHandRot[1], g_cfg.rightHandRot[2]);
    CfgEcho("EnableHandsProbe", "%d", (int)g_cfg.handsProbe);
    CfgEcho("HandsPtrOffset", "0x%X", g_cfg.handsPtrOff);
    CfgEcho("HandsPosOffset", "0x%X", g_cfg.handsPosOff);
    CfgEcho("HandsArm / Retry", "%d / %d calls  (~%.1f / %.1f s at 220/s)",
        g_cfg.handsArmCalls, g_cfg.handsRetryCalls,
        g_cfg.handsArmCalls / 220.0, g_cfg.handsRetryCalls / 220.0);
    CfgEcho("IdleAnimMode", "%d  %s", g_cfg.idleAnimMode,
        g_cfg.idleAnimMode == 0 ? "(off)" :
        g_cfg.idleAnimMode == 1 ? "(all entries -> entry[0], kills the wrench slap)" :
        g_cfg.idleAnimMode == 2 ? "(-> HandsOffscreenAnimationName, arms off screen)"
        : "(-> EquippingHandsAnim, no idle motion, arms visible)");
    CfgEcho("HandsGripOffset", "%.0f fwd, %.0f right, %.0f up (cm)",
        g_cfg.handsGrip[0], g_cfg.handsGrip[1], g_cfg.handsGrip[2]);
    CfgEcho("GripTunedFgFov", "%.1f  %s", g_cfg.gripTunedFgFov,
        g_cfg.gripTunedFgFov > 5.0f ? "(right/up auto-scaled to the live fg FOV)"
        : "(off -- offsets used exactly as written)");

    Log("[aim]");
    CfgEcho("AimSource", "%d  %s", g_cfg.aimSource,
        g_cfg.aimSource == 1 ? "(right controller)" : "(head)");
    CfgEcho("HeadAimWhenUnarmed", "%d", g_cfg.headAimUnarmed);
    CfgEcho("AimClampDeg", "%.0f", g_cfg.aimClampDeg);
    CfgEcho("PlasmidAimPitch", "%.0f deg", g_cfg.plasmidAimPitch);
    for (int s = 0; s < 9; ++s)
        Log("  slot %d  pos %6.1f,%6.1f,%6.1f   rot %5.1f,%5.1f,%5.1f   cur %5.1f,%5.1f,%5.1f   idle %d  hide %d", s,
            g_cfg.gripSlot[s][0], g_cfg.gripSlot[s][1], g_cfg.gripSlot[s][2],
            g_cfg.rotSlot[s][0], g_cfg.rotSlot[s][1], g_cfg.rotSlot[s][2],
            g_cfg.cursorSlot[s][0], g_cfg.cursorSlot[s][1], g_cfg.cursorSlot[s][2],
            g_cfg.idleModeSlot[s], g_cfg.hideArmsSlot[s]);

    CfgEcho("AimSmoothing", "%.2f", g_cfg.aimSmooth);
    CfgEcho("HudQuad", "%d  %.1f deg wide at %.2f m  pitch %.1f  yaw %.1f",
        (int)g_cfg.hudRedirect, g_cfg.hudWidthDeg, g_cfg.hudDist,
        g_cfg.hudPitchDeg, g_cfg.hudYawDeg);
    CfgEcho("DisableReticle", "%d   engine ptr 0x%X  exec 0x%X",
        (int)g_cfg.disableReticle, g_cfg.engPtrRva, g_cfg.engExecRva);
    CfgEcho("Crosshair", "%d  %.1f mm dot at %.2f m  fromShot=%d", (int)g_cfg.crosshair,
        g_cfg.xhSize * 1000.f, g_cfg.xhDist, g_cfg.xhFromShot);

    Log("[controller]");
    CfgEcho("EnableController", "%d  mode=%s", (int)g_cfg.controller,
        g_cfg.controllerMode ? "replace" : "merge");
    CfgEcho("ControllerLayout", "%d  %s", g_cfg.controllerLayout,
        g_cfg.controllerLayout ? "(jump right-A, use right-B)"
        : "(literal Xbox: jump left-Y, use right-A)");
    CfgEcho("ControllerPitch", "%d  %s", (int)g_cfg.controllerPitch,
        g_cfg.controllerPitch ? "(right-stick Y passed -- expect pitch fight)"
        : "(right-stick Y dropped -- correct for HeadAimMode=2)");
    CfgEcho("ControllerDeadzone", "%.2f", g_cfg.stickDeadzone);
    CfgEcho("DpadModifier", "%d  %s", g_cfg.dpadModifier,
        g_cfg.dpadModifier == 0 ? "(off)" :
        g_cfg.dpadModifier == 1 ? "(right thumbrest)" :
        g_cfg.dpadModifier == 2 ? "(right stick click)" : "(left grip)");
    CfgEcho("DpadFlip", "%d  %s", (int)g_cfg.dpadFlip,
        g_cfg.dpadFlip ? "(left thumbrest + right stick)" : "(right thumbrest + left stick)");
    CfgEcho("StickYToDpad / Log", "%d / %d", (int)g_cfg.stickYToDpad, (int)g_cfg.controllerLog);
    CfgEcho("JumpOnR3", "%d  %s", (int)g_cfg.jumpOnR3,
        g_cfg.jumpOnR3 ? "(R3 jumps, zoom unbound)" : "(R3 is zoom, stock)");
    CfgEcho("PauseChord", "%d  %s", (int)g_cfg.pauseChord,
        g_cfg.pauseChord ? "(hold X+Y to pause)" : "(off, menu button only)");
    CfgEcho("ForceWindowFocus", "%d", g_cfg.forceFocus);

    Log("[menus]");
    CfgEcho("EnableMenuScreen", "%d", (int)g_cfg.menuScreen);
    CfgEcho("MenuScreen", "%.2f m at %.2f m  (height %+.2f)",
        g_cfg.menuSize, g_cfg.menuDist, g_cfg.menuHeight);
    CfgEcho("MenuMaxIndexed", "%d", g_cfg.menuMaxIndexed);
    CfgEcho("MenuIndexCounts", "'%s'", g_cfg.menuList);
    CfgEcho("AnchorIndexCounts", "'%s'", g_cfg.anchorList);
    CfgEcho("AnchorMovies", "'%s'%s", g_cfg.anchorMovies,
        g_cfg.anchorMovies[0] ? "" : "   (none -- every screen rides the HUD "
        "panel and scales with HudWidthDeg)");
    CfgEcho("SceneMovies", "'%s'%s", g_cfg.sceneMovies,
        g_cfg.sceneMovies[0] ? "   (not captured; drawn in the world)" : "");
    CfgEcho("PanelMovies", "'%s'%s", g_cfg.panelMovies,
        g_cfg.panelMovies[0] ? "   (interface on the HUD panel, world in stereo)"
        : "   (none)");
    CfgEcho("FollowMovies", "'%s'%s", g_cfg.followMovies,
        g_cfg.followMovies[0] ? "   (whole frame, follows your head)" : "");
    CfgEcho("WaterProbe", "%d", g_cfg.waterProbe);
    CfgEcho("HudDsvMode", "%d  (0 none, 1 private, 2 game's)", g_cfg.hudDsvMode);

    Log("[debug/probe]");
    CfgEcho("EnableDrawHook", "%d", (int)g_cfg.drawHook);
    CfgEcho("EnableGameState", "%d", (int)g_cfg.gameState);
    CfgEcho("EnableNativeScan", "%d", (int)g_cfg.nativeScan);
    CfgEcho("EnableForcedMoveProbe", "%d", (int)g_cfg.forcedMoveProbe);
    CfgEcho("HookInstanced", "%d", (int)g_cfg.hookInstanced);
    CfgEcho("HideInactiveHand", "%d", g_cfg.hideInactiveHand);
    CfgEcho("HideCutsceneBars", "%d  verts %d", g_cfg.hideCutsceneBars, g_cfg.cutsceneBarVerts);
    CfgEcho("Swing", "%d  thr %.2f  rearm %.2f  cd %d  pulse %d  out %.2f  travel %.2f",
        g_cfg.swingEnabled, g_cfg.swingThreshold, g_cfg.swingRearm,
        g_cfg.swingCooldownMs, g_cfg.swingPulseMs,
        g_cfg.swingOutFrac, g_cfg.swingTravel);
    CfgEcho("Grip", "on %.2f  off %.2f", g_cfg.gripThreshold,
        g_cfg.gripThreshold - g_cfg.gripHysteresis);
    CfgEcho("HeadRelativeMove", "%d   (legacy -- MovementMode is the authority)",
        g_cfg.headRelativeMove);
    CfgEcho("MovementMode", "%d  %s", g_cfg.movementMode,
        g_cfg.movementMode == 0 ? "(neither: right stick only)"
        : g_cfg.movementMode == 1 ? "(controller: walk where you point)"
        : g_cfg.movementMode == 2 ? "(head: walk where you look)"
        : "(both: point and look each steer)");
    CfgEcho("HeadAimAlways", "%d  %s", g_cfg.headAimAlways,
        g_cfg.headAimAlways ? "(the aim field carries your HEAD)"
        : "(the aim field carries your CONTROLLER)");

    CfgEcho("FlashGuiProbe", "%d", g_cfg.flashGuiProbe);
    CfgEcho("StickPrecomp", "%d  %s", g_cfg.stickPrecomp,
        g_cfg.stickPrecomp ? "(undo the game's SQUARE movement deadzone)"
        : "(off -- rotated walking will drift)");
    CfgEcho("GameStickDeadzone", "%.3f   (User.ini XENON_LTHUMB_*AXIS DeadZone)",
        g_cfg.gameStickDeadzone);
    CfgEcho("TurnAxis", "max %.2f  exp %.2f   (1.00 max == the game's own steep "
        "top end)", g_cfg.turnAxisMax, g_cfg.turnAxisExp);
    CfgEcho("TurnRateProbe", "%d", g_cfg.turnRateProbe);
    CfgEcho("ScriptedRotProbe", "%d", g_cfg.scriptedRotProbe);
    CfgEcho("ScriptedCameraFollow", "%d  %s", g_cfg.scriptedCameraFollow,
        g_cfg.scriptedCameraFollow ? "(scripted scenes turn you from the "
        "game's camera)" : "(off -- aim field only)");
    CfgEcho("ScriptedRecentre", "%d  %s", g_cfg.scriptedRecentre,
        g_cfg.scriptedRecentre == 0 ? "(off -- scene rotation lands on top of "
        "your own turning)"
        : g_cfg.scriptedRecentre == 1 ? "(your own turning washes out as the "
        "scene turns)"
        : "(your own turning is dropped the moment the scene turns)");
    CfgEcho("WalkDriftProbe", "%d", g_cfg.walkDriftProbe);
    CfgEcho("GameTurnSpeed", "%d  %s", g_cfg.gameTurnSpeed,
        g_cfg.gameTurnSpeed < 0 ? "(leave the game's own value alone)"
        : "(written into Bioshock.ini Sensitivity)");

    // Echoed individually, not as a count. The whole value of the experiment
    // channel is being able to read back the EXACT string the engine will get --
    // a mistyped property name is invisible in "3 commands set".
    for (int i = 0; i < kExecCommands; ++i)
    {
        if (!g_cfg.execCommand[i][0]) continue;
        char key[32];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "ExecCommand%d", i + 1);
        CfgEcho(key, "%s", g_cfg.execCommand[i]);
    }
    CfgEcho("PitchServo", "%d  gain %.3f  dead %.1f deg  max %.2f",
        g_cfg.pitchServo, g_cfg.pitchServoGain, g_cfg.pitchServoDead, g_cfg.pitchServoMax);
    CfgEcho("SuppressIndexCounts", "'%s'", g_cfg.suppressList);
    CfgEcho("WeaponCounts", "'%s'  scale %.2f", g_cfg.weaponList, g_cfg.weaponScale);
    CfgEcho("HudCounts", "'%s'  scale %.2f", g_cfg.hudList, g_cfg.hudScale);
    CfgEcho("ArrowCounts", "'%s'  scale %.2f  offset %+.2f,%+.2f",
        g_cfg.arrowList, g_cfg.arrowScale, g_cfg.arrowX, g_cfg.arrowY);
    CfgEcho("ArrowPtrOffset", "0x%X   world %+.0f fwd %+.0f right %+.0f up (cm)",
        g_cfg.arrowPtrOff, g_cfg.arrowWorld[0], g_cfg.arrowWorld[1], g_cfg.arrowWorld[2]);
    CfgEcho("ArrowUnparentRot", "%d  %s", g_cfg.arrowUnparentRot,
        g_cfg.arrowUnparentRot ? "(cancel the weapon's rotation -- RUNS AWAY, "
        "see the QUEST ARROW banner)" : "(leave the arrow's rotation alone)");
    CfgEcho("MirrorOneEye", "%d  %s", g_cfg.mirrorOneEye,
        g_cfg.mirrorOneEye ? "(one eye -- no flicker on the monitor)"
        : "(both eyes, as before -- the monitor will flicker)");
    CfgEcho("ArrowDrawScale", "%.2f  %s", g_cfg.arrowDrawScale,
        g_cfg.arrowDrawScale > 0.f ? "" : "(size left alone)");
    CfgEcho("ArrowLevel", "%d  %s", g_cfg.arrowLevel,
        (g_cfg.arrowLevel >= 2) ? "(roll and pitch zeroed -- horizontal)"
        : g_cfg.arrowLevel ? "(roll zeroed -- it was the roll tracking the gun)"
        : "(rotation untouched)");
    CfgEcho("ArrowHideScripted", "%d  %s", g_cfg.arrowHideScripted,
        g_cfg.arrowHideScripted ? "(parked out of the world during scenes)"
        : "(visible during scenes)");

    Log("=========================");
}
