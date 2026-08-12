// BioshockVR/Input/InputHook.cpp
//
// Touch controllers -> synthetic Xbox pad. See InputHook.h for the shape.
//
// WHY AN XINPUT DETOUR AND NOT A VIRTUAL PAD DRIVER: ViGEm works and is what
// most mods use, but it is a signed driver the user has to install. We already
// have MinHook and a live OpenXR session in this process, so the whole thing
// fits in one file with no install step.
//
// THE THING THAT WILL BITE FIRST, so it is measured from line one: a game that
// believes no controller exists may never call XInputGetState at all, in which
// case a perfect detour changes nothing. XInputGetCapabilities is therefore
// hooked too (a game that asks "is there a pad?" gets YES), and the heartbeat
// prints the raw call counts. If POLL shows getState=0 forever, the problem is
// upstream of this file and the Bioshock.ini UseJoystick/UseController keys are
// back on the table.
// 
// ============================================================================
//  EVERY xr* FUNCTION CALLED HERE MUST BE EXPORTED BY THE SHIM.
//
//  BioshockVR.dll statically imports openxr_loader.dll. On the SteamVR path
//  that file IS the shim, so a call to any function the shim does not export
//  makes WINDOWS refuse to load BioshockVR.dll entirely -- and the loader
//  reports it as "BioshockVR.dll not found beside dxgi.dll", which sends you
//  looking for a missing file that is sitting right there.
//
//  MEASURED: xrGetCurrentInteractionProfile cost an evening exactly this way.
//
//  The shim's export list is the ProcEntry table at the bottom of
//  shim_main.cpp. Check there before adding a call. If you need something
//  outside that list, resolve it at RUNTIME via xrGetInstanceProcAddr -- a
//  null return is harmless, a missing static import is fatal.
// ============================================================================

#include "Input/InputHook.h"
#include "Camera/CameraHook.h"

#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include "Input/Swing.h"

#include <MinHook.h>
#include "Core/Config.h"

extern void LogFile(const char* msg);

bool CameraHook_GetPitchError(float* outDeg);
bool CameraHook_GetHeadYawOffset(float* outDeg);
bool CameraHook_AimUsesHead();   // is the head already in the aim field?
// How far to rotate the movement stick for the current MovementMode. Computed
// at the aim write site from the rotators actually written -- see the banner
// above PublishWalkRotation in CameraHook.cpp.
bool CameraHook_GetWalkRotation(float* outDeg);


bool DrawHook_MenuUp();              // DrawHook.cpp
bool GameState_Paused();             // GameState.cpp
bool GameState_Theater();            // GameState.cpp
bool GameState_ScriptedAnim();       // GameState.cpp
bool GameState_ForcedMove();         // GameState.cpp
bool GameState_ScriptedWindow();     // GameState.cpp -- the two above, HELD

// M7-S2/S6. The turn path must be handed back for the whole window in which the
// mod is not writing the aim field, which includes the forced move BEFORE a
// sequence starts.
//
// ⚠ THIS IS NOT A MIRROR OF CameraHook's ScriptedAimReleased(), and the comment
// here used to claim it was. That one now also releases for a "walk through"
// scene (ControllableScriptedFix), where the mod deliberately KEEPS writing the
// aim field so head look can steer. This one stays wider on purpose: releasing
// the turn axis there is what the tester signed off on -- "the big daddy splicer
// fight felt mostly normal". Do not weld them together without a headset.
//
// THE PAIR IS NOW HELD, and the width above is unchanged by that. The union of
// the two signals goes momentarily false BETWEEN the two phases of one scene --
// measured at 5 ms on the Little Sister crawl -- which handed the turn axis back
// mid-scene for a frame. GameState_ScriptedWindow() is the same union with a
// hold on its falling edge; every policy layered on top stays exactly as it was.
static bool ScriptedQol()
{
    return g_cfg.scriptedQol && GameState_ScriptedWindow();
}

static void Log(const char* fmt, ...)
{
    char b[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

// ---------------------------------------------------------------- XInput ABI
//
// Declared locally rather than including <Xinput.h>. The layouts are identical
// across 9.1.0 / 1.3 / 1.4 and this way the build cannot accidentally pick up a
// #pragma comment(lib) and add a load-time dependency on an XInput DLL that the
// game might not otherwise have pulled in.

typedef struct _XI_GAMEPAD
{
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XI_GAMEPAD;

typedef struct _XI_STATE
{
    DWORD      dwPacketNumber;
    XI_GAMEPAD Gamepad;
} XI_STATE;

typedef struct _XI_VIBRATION { WORD wLeftMotorSpeed, wRightMotorSpeed; } XI_VIBRATION;

typedef struct _XI_CAPS
{
    BYTE         Type;
    BYTE         SubType;
    WORD         Flags;
    XI_GAMEPAD   Gamepad;
    XI_VIBRATION Vibration;
} XI_CAPS;

#define XI_DPAD_UP        0x0001
#define XI_DPAD_DOWN      0x0002
#define XI_DPAD_LEFT      0x0004
#define XI_DPAD_RIGHT     0x0008
#define XI_START          0x0010
#define XI_BACK           0x0020
#define XI_LTHUMB         0x0040
#define XI_RTHUMB         0x0080
#define XI_LSHOULDER      0x0100
#define XI_RSHOULDER      0x0200
#define XI_A              0x1000
#define XI_B              0x2000
#define XI_X              0x4000
#define XI_Y              0x8000

#define XI_ERR_SUCCESS              0
#define XI_ERR_DEVICE_NOT_CONNECTED 1167

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XI_STATE*);
typedef DWORD(WINAPI* XInputGetCapsFn)(DWORD, DWORD, XI_CAPS*);

static XInputGetStateFn g_origGetState = nullptr;
static XInputGetStateFn g_origGetStateEx = nullptr;   // ordinal 100, UE3 uses it
static XInputGetCapsFn  g_origGetCaps = nullptr;

static void* g_addrGetState = nullptr;
static void* g_addrGetStateEx = nullptr;
static void* g_addrGetCaps = nullptr;

static bool  g_installed = false;
static DWORD g_installDeadline = 0;
static bool  g_gaveUp = false;

// Bring-up visibility. If getState stays 0 the game is not polling and nothing
// in this file can help; that is the single most useful number here.
static volatile long g_nGetState = 0;
static volatile long g_nGetCaps = 0;
static volatile long g_nSynth = 0;
static volatile long g_nRealPad = 0;

// ---------------------------------------------------------------- the channel
//
// Same seqlock discipline as the head-pose channel in XRSession/CameraHook:
// odd sequence == a write is in flight, retry. One producer, one consumer,
// no locks, no priority inversion into the render thread.

struct PadState
{
    float moveX, moveY;      // left stick,  -1..1
    float turnX, turnY;      // right stick, -1..1
    float trigL, trigR;      // 0..1
    float gripL, gripR;      // 0..1
    bool  a, b, x, y;
    bool  menu, thumbL, thumbR;
    bool  restR;             // right thumbrest capacitive touch
    bool  restL;             // left thumbrest capacitive touch
    bool  active;            // false == session not focused, publish neutral
};

static volatile long g_padSeq = 0;
static PadState      g_pad = {};

// Second, independent seqlock for the hand poses. Separate from the pad on
// purpose: the pad is read by the game's input thread every XInput poll, the
// poses will be read by the camera hook on the game thread at CalcView rate.
// One channel per consumer means neither can be starved by the other's retries.
static volatile long g_handSeq = 0;
static HandPose      g_hands[2] = {};

static void PublishPad(const PadState& s)
{
    _InterlockedIncrement(&g_padSeq);        // odd == writing
    MemoryBarrier();
    g_pad = s;
    MemoryBarrier();
    _InterlockedIncrement(&g_padSeq);        // even == done
}

static void PublishHands(const HandPose h[2])
{
    _InterlockedIncrement(&g_handSeq);
    MemoryBarrier();
    g_hands[0] = h[0];
    g_hands[1] = h[1];
    MemoryBarrier();
    _InterlockedIncrement(&g_handSeq);
}

bool Input_GetHandPose(int hand, HandPose* out)
{
    if (!out || hand < 0 || hand > 1) return false;
    for (int tries = 0; tries < 8; ++tries)
    {
        const long s0 = g_handSeq;
        if (s0 & 1) continue;
        MemoryBarrier();
        *out = g_hands[hand];
        MemoryBarrier();
        if (g_handSeq == s0) return true;
    }
    return false;
}

static bool ReadPad(PadState* out)
{
    for (int tries = 0; tries < 8; ++tries)
    {
        const long s0 = g_padSeq;
        if (s0 & 1) continue;
        MemoryBarrier();
        *out = g_pad;
        MemoryBarrier();
        if (g_padSeq == s0) return true;
    }
    return false;
}

bool Input_GetTurnX(float* out)
{
    if (!out) return false;
    PadState s;
    if (!ReadPad(&s) || !s.active) return false;
    *out = s.turnX;
    return true;
}

// The weapon/plasmid radial owns both sticks while a grip is held.
bool Input_WeaponWheelHeld()
{
    PadState s;
    if (!ReadPad(&s) || !s.active) return false;
    return (s.gripL > g_cfg.gripThreshold) || (s.gripR > g_cfg.gripThreshold);
}

// ---------------------------------------------------------------- OpenXR side

static XrInstance g_inst = XR_NULL_HANDLE;
static XrSession  g_sess = XR_NULL_HANDLE;
static bool       g_xrReady = false;

static XrActionSet g_set = XR_NULL_HANDLE;

static XrAction g_aMove = XR_NULL_HANDLE, g_aTurn = XR_NULL_HANDLE;
static XrAction g_aTrigL = XR_NULL_HANDLE, g_aTrigR = XR_NULL_HANDLE;
static XrAction g_aGripL = XR_NULL_HANDLE, g_aGripR = XR_NULL_HANDLE;
static XrAction g_aA = XR_NULL_HANDLE, g_aB = XR_NULL_HANDLE;
static XrAction g_aX = XR_NULL_HANDLE, g_aY = XR_NULL_HANDLE;
static XrAction g_aMenu = XR_NULL_HANDLE;
static XrAction g_aThumbL = XR_NULL_HANDLE, g_aThumbR = XR_NULL_HANDLE;
static XrAction g_aRestR = XR_NULL_HANDLE;
static XrAction g_aRestL = XR_NULL_HANDLE;

// Created now, deliberately unused. Attaching is one-shot -- adding these later
// costs a session restart, and motion controls will want every one of them.
static XrAction g_aAimPoseL = XR_NULL_HANDLE, g_aAimPoseR = XR_NULL_HANDLE;
static XrAction g_aGripPoseL = XR_NULL_HANDLE, g_aGripPoseR = XR_NULL_HANDLE;
static XrAction g_aHapticL = XR_NULL_HANDLE, g_aHapticR = XR_NULL_HANDLE;
static XrSpace  g_spAimL = XR_NULL_HANDLE, g_spAimR = XR_NULL_HANDLE;
static XrSpace  g_spGripL = XR_NULL_HANDLE, g_spGripR = XR_NULL_HANDLE;

static XrPath P(const char* s)
{
    XrPath p = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(g_inst, s, &p)))
        Log(">>> INPUT: !!! bad path '%s'", s);
    return p;
}

static bool MakeAction(XrActionType t, const char* name, const char* pretty, XrAction* out)
{
    XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
    ai.actionType = t;
    strncpy_s(ai.actionName, name, _TRUNCATE);
    strncpy_s(ai.localizedActionName, pretty, _TRUNCATE);

    const XrResult r = xrCreateAction(g_set, &ai, out);
    if (XR_FAILED(r))
    {
        Log(">>> INPUT: !!! xrCreateAction('%s') failed (%d)", name, (int)r);
        *out = XR_NULL_HANDLE;
        return false;
    }
    return true;
}

static XrSpace MakeActionSpace(XrAction act)
{
    if (act == XR_NULL_HANDLE) return XR_NULL_HANDLE;
    XrActionSpaceCreateInfo si = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
    si.action = act;
    si.poseInActionSpace.orientation.w = 1.0f;
    XrSpace sp = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateActionSpace(g_sess, &si, &sp))) return XR_NULL_HANDLE;
    return sp;
}

bool Input_XrCreate(XrInstance inst, XrSession sess)
{
    if (!g_cfg.controller) { Log(">>> INPUT: DISABLED by ini (EnableController=0)"); return false; }
    if (g_xrReady) return true;

    g_inst = inst;
    g_sess = sess;

    XrActionSetCreateInfo sci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy_s(sci.actionSetName, "gamepad", _TRUNCATE);
    strncpy_s(sci.localizedActionSetName, "Gamepad", _TRUNCATE);
    sci.priority = 0;

    XrResult r = xrCreateActionSet(g_inst, &sci, &g_set);
    if (XR_FAILED(r)) { Log(">>> INPUT: !!! xrCreateActionSet failed (%d)", (int)r); return false; }

    MakeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move", &g_aMove);
    MakeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Turn", &g_aTurn);
    MakeAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_l", "Left Trigger", &g_aTrigL);
    MakeAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_r", "Right Trigger", &g_aTrigR);
    MakeAction(XR_ACTION_TYPE_FLOAT_INPUT, "grip_l", "Left Grip", &g_aGripL);
    MakeAction(XR_ACTION_TYPE_FLOAT_INPUT, "grip_r", "Right Grip", &g_aGripR);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_a", "A", &g_aA);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_b", "B", &g_aB);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_x", "X", &g_aX);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_y", "Y", &g_aY);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", &g_aMenu);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_l", "Left Stick Click", &g_aThumbL);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_r", "Right Stick Click", &g_aThumbR);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "rest_r", "Right Thumbrest", &g_aRestR);
    MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "rest_l", "Left Thumbrest", &g_aRestL);

    MakeAction(XR_ACTION_TYPE_POSE_INPUT, "aim_l", "Left Aim", &g_aAimPoseL);
    MakeAction(XR_ACTION_TYPE_POSE_INPUT, "aim_r", "Right Aim", &g_aAimPoseR);
    MakeAction(XR_ACTION_TYPE_POSE_INPUT, "gpose_l", "Left Grip Pose", &g_aGripPoseL);
    MakeAction(XR_ACTION_TYPE_POSE_INPUT, "gpose_r", "Right Grip Pose", &g_aGripPoseR);
    MakeAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_l", "Left Haptic", &g_aHapticL);
    MakeAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_r", "Right Haptic", &g_aHapticR);

    // ---- Touch bindings -------------------------------------------------
    // Note the asymmetry, it is not a mistake: Touch has ONE application menu
    // button and it is on the LEFT controller. The right controller's system
    // button belongs to the runtime and cannot be bound. So START comes from
    // the left menu button and BACK has no home -- see the mapping table in
    // FillFromPad().
    {
        XrActionSuggestedBinding b[] = {
            { g_aMove,      P("/user/hand/left/input/thumbstick") },
            { g_aTurn,      P("/user/hand/right/input/thumbstick") },
            { g_aTrigL,     P("/user/hand/left/input/trigger/value") },
            { g_aTrigR,     P("/user/hand/right/input/trigger/value") },
            { g_aGripL,     P("/user/hand/left/input/squeeze/value") },
            { g_aGripR,     P("/user/hand/right/input/squeeze/value") },
            { g_aA,         P("/user/hand/right/input/a/click") },
            { g_aB,         P("/user/hand/right/input/b/click") },
            { g_aX,         P("/user/hand/left/input/x/click") },
            { g_aY,         P("/user/hand/left/input/y/click") },
            { g_aMenu,      P("/user/hand/left/input/menu/click") },
            { g_aThumbL,    P("/user/hand/left/input/thumbstick/click") },
            { g_aThumbR,    P("/user/hand/right/input/thumbstick/click") },
            { g_aRestR,     P("/user/hand/right/input/thumbrest/touch") },
            { g_aRestL,     P("/user/hand/left/input/thumbrest/touch") },
            { g_aAimPoseL,  P("/user/hand/left/input/aim/pose") },
            { g_aAimPoseR,  P("/user/hand/right/input/aim/pose") },
            { g_aGripPoseL, P("/user/hand/left/input/grip/pose") },
            { g_aGripPoseR, P("/user/hand/right/input/grip/pose") },
            { g_aHapticL,   P("/user/hand/left/output/haptic") },
            { g_aHapticR,   P("/user/hand/right/output/haptic") },
        };

        XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sb.interactionProfile = P("/interaction_profiles/oculus/touch_controller");
        sb.countSuggestedBindings = (uint32_t)(sizeof(b) / sizeof(b[0]));
        sb.suggestedBindings = b;

        r = xrSuggestInteractionProfileBindings(g_inst, &sb);
        if (XR_FAILED(r)) Log(">>> INPUT: !!! suggest(touch) failed (%d)", (int)r);
        else              Log(">>> INPUT: touch_controller bindings suggested");
    }

    // Fallback so a runtime that does not advertise the Touch profile still
    // gives us something. Simple controller has no sticks, so this is a
    // "menus at least work" tier, not a play tier.
    {
        XrActionSuggestedBinding b[] = {
            { g_aA,         P("/user/hand/right/input/select/click") },
            { g_aMenu,      P("/user/hand/left/input/menu/click") },
            { g_aAimPoseL,  P("/user/hand/left/input/aim/pose") },
            { g_aAimPoseR,  P("/user/hand/right/input/aim/pose") },
            { g_aGripPoseL, P("/user/hand/left/input/grip/pose") },
            { g_aGripPoseR, P("/user/hand/right/input/grip/pose") },
            { g_aHapticL,   P("/user/hand/left/output/haptic") },
            { g_aHapticR,   P("/user/hand/right/output/haptic") },
        };
        XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sb.interactionProfile = P("/interaction_profiles/khr/simple_controller");
        sb.countSuggestedBindings = (uint32_t)(sizeof(b) / sizeof(b[0]));
        sb.suggestedBindings = b;
        xrSuggestInteractionProfileBindings(g_inst, &sb);   // best effort
    }

    // ---- ATTACH. One shot for the lifetime of the session. --------------
    {
        XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        ai.countActionSets = 1;
        ai.actionSets = &g_set;
        r = xrAttachSessionActionSets(g_sess, &ai);
        if (XR_FAILED(r))
        {
            Log(">>> INPUT: !!! xrAttachSessionActionSets failed (%d). No controller input.", (int)r);
            return false;
        }
        Log(">>> INPUT: action set ATTACHED");
    }

    g_spAimL = MakeActionSpace(g_aAimPoseL);
    g_spAimR = MakeActionSpace(g_aAimPoseR);
    g_spGripL = MakeActionSpace(g_aGripPoseL);
    g_spGripR = MakeActionSpace(g_aGripPoseR);

    g_xrReady = true;
    Log(">>> INPUT: OpenXR side READY (pitch %s, deadzone %.2f, mode %s)",
        g_cfg.controllerPitch ? "PASSED THROUGH -- expect fighting" : "dropped",
        g_cfg.stickDeadzone,
        g_cfg.controllerMode ? "replace" : "merge");
    return true;
}

static float GetFloat(XrAction a)
{
    if (a == XR_NULL_HANDLE) return 0.0f;
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(g_sess, &gi, &s)) || !s.isActive) return 0.0f;
    return s.currentState;
}

static bool GetBool(XrAction a)
{
    if (a == XR_NULL_HANDLE) return false;
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(g_sess, &gi, &s)) || !s.isActive) return false;
    return s.currentState != XR_FALSE;
}

static void GetVec2(XrAction a, float* x, float* y)
{
    *x = 0.0f; *y = 0.0f;
    if (a == XR_NULL_HANDLE) return;
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(g_sess, &gi, &s)) || !s.isActive) return;
    *x = s.currentState.x;
    *y = s.currentState.y;
}

// Radial deadzone with rescale, so the usable range stays 0..1 instead of
// jumping from 0 to deadzone. Applied per stick, not per axis -- per-axis
// deadzones make diagonals feel notched.
static void Deadzone(float* x, float* y, float dz)
{
    const float m = sqrtf((*x) * (*x) + (*y) * (*y));
    if (m <= dz || m <= 1e-6f) { *x = 0.0f; *y = 0.0f; return; }
    const float scaled = (m - dz) / (1.0f - dz);
    const float k = (scaled > 1.0f ? 1.0f : scaled) / m;
    *x *= k;
    *y *= k;
}

// Rotate (0,0,-1) by q -- OpenXR forward. Used only for the log line; the
// consumers get the raw quaternion and do their own maths in game units.
static void QuatForward(const float q[4], float f[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    f[0] = -2.0f * (x * z + w * y);
    f[1] = -2.0f * (y * z - w * x);
    f[2] = -(1.0f - 2.0f * (x * x + y * y));
}

static bool LocateOne(XrSpace act, XrSpace base, XrTime t, float q[4], float p[3])
{
    if (act == XR_NULL_HANDLE) return false;

    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(act, base, t, &loc))) return false;

    const XrSpaceLocationFlags need =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return false;

    q[0] = loc.pose.orientation.x; q[1] = loc.pose.orientation.y;
    q[2] = loc.pose.orientation.z; q[3] = loc.pose.orientation.w;
    p[0] = loc.pose.position.x;    p[1] = loc.pose.position.y;
    p[2] = loc.pose.position.z;
    return true;
}

// ---- HAPTICS ---------------------------------------------------------------
// THE ACTIONS HAVE EXISTED SINCE MOTION CONTROLS SHIPPED AND NOTHING HAS EVER
// FIRED ONE. haptic_l and haptic_r are created in MakeAction and bound in every
// interaction profile below; there was simply never a caller. This is it.
//
// Holsters, the two-handed grip and the wrench all want the same thing: a short
// confirmation buzz on ONE hand at the moment a gesture is recognised. A gesture
// you cannot feel lands is one you second-guess and repeat.
//
// FAILS SILENTLY BY DESIGN, and the log line is throttled to the first failure.
// A runtime with no haptic support, a controller that has gone to sleep, or a
// session without focus all return non-success here, and none of them is worth a
// per-gesture log line -- let alone refusing the gesture that triggered it.
void Input_Pulse(int hand, float amplitude, int ms)
{
    if (!g_xrReady) return;

    const XrAction a = (hand == HAND_RIGHT) ? g_aHapticR : g_aHapticL;
    if (a == XR_NULL_HANDLE) return;

    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;
    if (ms < 1) ms = 1;
    if (ms > 2000) ms = 2000;          // a stuck buzz is worse than no buzz

    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    v.amplitude = amplitude;
    v.duration = (XrDuration)ms * 1000000LL;    // ms -> nanoseconds
    v.frequency = XR_FREQUENCY_UNSPECIFIED;

    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = a;
    hi.subactionPath = XR_NULL_PATH;

    const XrResult r =
        xrApplyHapticFeedback(g_sess, &hi, (const XrHapticBaseHeader*)&v);

    if (r != XR_SUCCESS)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            Log(">>> INPUT: xrApplyHapticFeedback -> %d. Haptics are off for "
                "this session; gestures still work.", (int)r);
        }
    }
}

void Input_XrSync(XrTime displayTime, XrSpace baseSpace)
{
    if (!g_xrReady) return;

    XrActiveActionSet aas = {};
    aas.actionSet = g_set;
    aas.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;

    const XrResult r = xrSyncActions(g_sess, &si);

    // XR_SESSION_NOT_FOCUSED is a SUCCESS code, not a failure: the runtime has
    // input focus (dashboard up, headset off the head). Publish a neutral pad.
    // Skipping the publish instead would freeze the last stick value and the
    // player would keep walking into a wall while the headset sat on the desk.
    if (r != XR_SUCCESS)
    {
        static XrResult lastR = XR_SUCCESS;
        if (r != lastR) { lastR = r; Log(">>> INPUT: xrSyncActions -> %d (pad neutral)", (int)r); }
        PadState z = {};
        z.active = false;
        PublishPad(z);

        HandPose hz[2] = {};      // untracked: consumers must fall back to head
        PublishHands(hz);
        return;
    }

    PadState s = {};
    s.active = true;

    GetVec2(g_aMove, &s.moveX, &s.moveY);
    GetVec2(g_aTurn, &s.turnX, &s.turnY);
    Deadzone(&s.moveX, &s.moveY, g_cfg.stickDeadzone);
    Deadzone(&s.turnX, &s.turnY, g_cfg.stickDeadzone);

    s.trigL = GetFloat(g_aTrigL);
    s.trigR = GetFloat(g_aTrigR);
    s.gripL = GetFloat(g_aGripL);
    s.gripR = GetFloat(g_aGripR);

    s.a = GetBool(g_aA);
    s.b = GetBool(g_aB);
    s.x = GetBool(g_aX);
    s.y = GetBool(g_aY);
    s.menu = GetBool(g_aMenu);
    // L3 DIAGNOSTIC. MEASURED: pressed repeatedly, appeared in zero PAD lines.
    // GetBool collapses three different failures into one `false` -- API error,
    // action inactive (no binding for this interaction profile), and simply not
    // pressed all look identical. Separate them.
    {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = g_aThumbL;
        XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
        const XrResult r = (g_aThumbL == XR_NULL_HANDLE)
            ? XR_ERROR_HANDLE_INVALID
            : xrGetActionStateBoolean(g_sess, &gi, &st);

        static XrResult lastR = (XrResult)0x7FFFFFFF;
        static XrBool32 lastA = 2, lastS = 2;
        if (r != lastR || st.isActive != lastA || st.currentState != lastS)
        {
            lastR = r; lastA = st.isActive; lastS = st.currentState;
            Log(">>> ACTION thumb_l: result=%d active=%d state=%d changed=%d",
                (int)r, (int)st.isActive, (int)st.currentState,
                (int)st.changedSinceLastSync);
        }
        s.thumbL = XR_SUCCEEDED(r) && st.isActive && st.currentState != XR_FALSE;
    }

    s.thumbR = GetBool(g_aThumbR);
    s.restR = GetBool(g_aRestR);
    s.restL = GetBool(g_aRestL);

    PublishPad(s);

    // ---- hand poses -----------------------------------------------------
    // Located in the SAME space and at the SAME predicted display time as the
    // views, so a hand pose and the head pose for one frame are consistent with
    // each other. Locating at "now" instead would put them tens of ms apart and
    // the aim ray would lag the world during head motion.
    HandPose hp[2] = {};
    hp[HAND_LEFT].aimValid =
        LocateOne(g_spAimL, baseSpace, displayTime, hp[HAND_LEFT].aimQuat, hp[HAND_LEFT].aimPos);
    hp[HAND_LEFT].gripValid =
        LocateOne(g_spGripL, baseSpace, displayTime, hp[HAND_LEFT].gripQuat, hp[HAND_LEFT].gripPos);
    hp[HAND_RIGHT].aimValid =
        LocateOne(g_spAimR, baseSpace, displayTime, hp[HAND_RIGHT].aimQuat, hp[HAND_RIGHT].aimPos);
    hp[HAND_RIGHT].gripValid =
        LocateOne(g_spGripR, baseSpace, displayTime, hp[HAND_RIGHT].gripQuat, hp[HAND_RIGHT].gripPos);

    PublishHands(hp);
}

// ---------------------------------------------------------------- the detour

// ===========================================================================
//  TURN RESPONSE -- WHY THE SAME PUSH GAVE A DIFFERENT SPEED
//
// REPORTED AS "sometimes it's slow and sometimes it's fast". MEASURED
// 2026-08-11, 40 samples, and the first hypothesis died cleanly: CalcView ran at
// 142-239 calls/s and the turn rate showed NO correlation with it at all -- at
// ~230 calls/s the rate ranged from 52.9 to 215.6 deg/s. FRAME-RATE DEPENDENCE
// IS FALSIFIED.
//
// What it tracks is stick deflection, and the game's curve is nearly vertical at
// the very top:
//
//     stick 0.90-0.93 ->  64-72 deg/s        stick 0.99 -> 112-144
//     stick 0.96-0.97 ->  82-101             stick 1.00 -> 187-216
//     stick 0.98      ->  94-117
//
// A 2% difference in how hard you push DOUBLES the turn rate. So holding it
// steady feels steady, and releasing and re-pushing lands you somewhere else on
// a curve that is almost a cliff at the end. Nothing was inconsistent except
// where on the stick the thumb happened to stop.
//
// THE FIX IS TO NEVER SEND THE CLIFF. Remap the deflection into [0, TurnAxisMax]
// so the steep region is unreachable and the same push always means the same
// rate. TurnAxisExp shapes the rest of the range: 1.0 is linear, above 1 gives
// finer control near centre.
//
// BOTH ARE INI-TUNABLE, deliberately -- this trades top speed for repeatability
// and that is a matter of taste, so raising the cap must not need a rebuild.
//
// NOT APPLIED TO SNAP TURN OR ModYaw: both bypass this axis entirely and rotate
// g_aimBase themselves at a rate we already control.
// ===========================================================================
//  THE GAME'S MOVEMENT DEADZONE IS SQUARE, AND THAT IS THE WALK DRIFT
//
// MEASURED 2026-08-11, and it is in the game's own binding file:
//
//   XENON_LTHUMB_XAXIS=Axis xStrafe   Speedbase=1.0 DeadZone=0.225 | ...
//   XENON_LTHUMB_YAXIS=Axis xForward  Speedbase=1.0 DeadZone=0.225 | ...
//
// PER AXIS. Rotating the stick by R to redirect walking MOVES MAGNITUDE BETWEEN
// THE TWO AXES, and the game then shrinks each axis independently -- so the
// direction that comes out is not the direction we sent. Modelling the game as
// out = (|a| - d) / (1 - d) reproduces the logged sent-vs-received pairs exactly,
// seven for seven:
//
//     sent  -72.0 -> predicted -83.4, logged -83.4
//     sent  -74.7 -> predicted -87.0, logged -87.0
//     sent  +78.6 -> predicted +90.0, logged +90.0     <- forward lane ZEROED
//     sent +162.1 -> predicted +173.5, logged +173.6
//     sent  -13.6 -> predicted  -0.8, logged  -0.8
//
// The +-90.0 saturation is the signature: once the forward component falls under
// 0.225 it is zeroed outright and the walk collapses to pure strafe. The residual
// clusters at +-11 degrees, inverts with direction, and is worst near 90 where
// the forward component is smallest -- exactly the reported
// "turning the controller 90 degrees causes me to walk 10-20 diagonal".
//
// ⚠ THIS WAS NEVER A TERM WE FAILED TO CANCEL. R is algebraically exact and
// always was; the distortion happens AFTER the value leaves us. Three builds of
// refining the cancellation could not have touched it. When a correction is
// provably exact and the symptom survives, stop refining the correction and go
// and measure what the other side actually received.
//
// THE INVERSE. For a desired direction u (unit) and magnitude m:
//
//     send_i = sign(u_i) * ( |u_i| * m * (1 - d) + d )
//
// after which the game recovers exactly u_i * m -- direction exact, magnitude
// exact. A zero component stays zero, so a pure-forward push is untouched.
//
// WHY NOT JUST EDIT User.ini. Those are binding lines carrying several bindings
// each -- XENON_LTHUMB_XAXIS also holds `Axis xLean DeadZone=0.4` -- the file has
// multiple binding sections, and the game rewrites it at exit. String surgery
// there risks breaking the controls outright, for a value we can simply invert.
static void PrecompStickDeadzone(float* px, float* py)
{
    if (!g_cfg.stickPrecomp) return;

    const float d = g_cfg.gameStickDeadzone;
    if (d <= 0.0f || d >= 0.95f) return;

    float x = *px, y = *py;
    const float mag = sqrtf(x * x + y * y);
    if (mag < 1e-4f) return;                    // centred; leave it alone

    // Direction and magnitude are treated separately, because only the direction
    // is being corrupted -- the magnitude is what the player asked for and must
    // survive. Magnitude above 1 was already handled by the clamp above.
    const float ux = x / mag, uy = y / mag;
    const float m = (mag > 1.0f) ? 1.0f : mag;

    const float sx = (fabsf(ux) < 1e-4f) ? 0.0f
        : (ux < 0.0f ? -1.0f : 1.0f) * (fabsf(ux) * m * (1.0f - d) + d);
    const float sy = (fabsf(uy) < 1e-4f) ? 0.0f
        : (uy < 0.0f ? -1.0f : 1.0f) * (fabsf(uy) * m * (1.0f - d) + d);

    *px = sx; *py = sy;
}

// The movement-stick angle this detour last handed the game, in degrees, in the
// game's own (forward, strafe) convention. Read from CalcView by WalkDriftProbe.
//
// GAME THREAD both ends in practice -- the detour is called from the game's own
// input poll -- but seq-locked anyway, because "in practice" is how the last
// cross-thread bug in this file got in.
static volatile long g_sentSeq = 0;
static float         g_sentDeg = 0.0f;

static void PublishSentStickAngle(float mx, float my)
{
    // mx is the STRAFE lane (sThumbLX) and my the FORWARD lane (sThumbLY), so the
    // angle is atan2(strafe, forward) -- the same convention ReceivedStickAngle
    // reads out of aStrafe/aForward, or the two would not be comparable. A pure
    // forward push rotated by R comes out of this as exactly R.
    float d = 0.0f;
    if (fabsf(mx) + fabsf(my) > 0.001f)
        d = atan2f(mx, my) * (180.0f / 3.14159265f);

    _InterlockedIncrement(&g_sentSeq);
    MemoryBarrier();
    g_sentDeg = d;
    MemoryBarrier();
    _InterlockedIncrement(&g_sentSeq);
}

bool Input_GetSentStickAngle(float* outDeg)
{
    if (!outDeg) return false;
    for (int t = 0; t < 8; ++t)
    {
        const long s0 = g_sentSeq;
        if (s0 & 1) continue;
        MemoryBarrier();
        const float v = g_sentDeg;
        MemoryBarrier();
        if (g_sentSeq == s0) { *outDeg = v; return true; }
    }
    return false;
}

static inline float TurnResponse(float v)
{
    const float dead = g_cfg.stickDeadzone;
    const float a = fabsf(v);
    if (a <= dead) return 0.0f;

    // Renormalise so the curve spans the USABLE range. Without this the exponent
    // would be applied to a value that never reaches 0 at the bottom.
    float t = (a - dead) / (1.0f - dead);
    if (t > 1.0f) t = 1.0f;

    if (g_cfg.turnAxisExp != 1.0f) t = powf(t, g_cfg.turnAxisExp);

    const float out = t * g_cfg.turnAxisMax;
    return (v < 0.0f) ? -out : out;
}

static inline SHORT ToAxis(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    const float f = v * 32767.0f;
    return (SHORT)(f < 0.0f ? f - 0.5f : f + 0.5f);
}

static inline BYTE ToTrigger(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (BYTE)(v * 255.0f + 0.5f);
}

// MAPPING TABLE (change it here, not in three places):
//
//   left stick            -> LX / LY                 move + strafe
//   right stick X         -> RX                      yaw, through the game's
//                                                    own turn path
//   right stick Y         -> RY only if ControllerPitch=1, or while a menu is
//                            up (menu lists scroll on it and would otherwise
//                            be unusable). Default: DROPPED. HeadAimMode=2
//                            erases injected pitch ~8ms later and it reads as
//                            a fight -- we own this source, so we just don't
//                            send it.
//   right stick Y         -> D-pad up/down if ControllerStickYToDpad=1. The
//                            axis is dead anyway and BioShock cycles weapons
//                            and plasmids on the d-pad. Off by default because
//                            it will occasionally fire mid-turn.
//   triggers              -> triggers                fire / alt-fire
//   grips                 -> LB / RB
//   A B X Y               -> A B X Y                 same letters, same places
//   left menu button      -> START                   pause
//   stick clicks          -> L3 / R3
//   BACK                  -> unmapped. Touch has one menu button and it is
//                            already START. If BioShock turns out to need BACK
//                            for something daily, take it from thumb_l.
//   D-PAD MODIFIER (ControllerDpadModifier): while held, the LEFT stick stops
//                            producing movement and produces d-pad instead.
//                            1 = right thumbrest touch, 2 = right stick click,
//                            3 = left grip. The thumbrest is capacitive and
//                            reads touched whenever the thumb is merely
//                            RESTING -- which is most of the time you are
//                            walking -- so the d-pad fires as a 120ms PULSE on
//                            direction change, never as a held press.
//                            Suppressed entirely while a grip is held: the
//                            plasmid radial is driven by the LEFT stick, and a
//                            resting right thumb must not zero it.
//
// S35, from the game's own User.ini [Default] context:
//     XENON_A = Use          XENON_B  = Med hypo
//     XENON_X = Hack/Reload  XENON_Y  = JUMP
//     XENON_LB/RB (hold) = plasmid / weapon RADIAL, selected with the sticks
//     XENON_LT/RT = fire plasmid / fire weapon
//     L3 = Duck   R3 = Zoom   START = Pause   BACK = Context help
// Layout 0 reproduces that literally. Layout 1 rearranges so the right thumb
// (already parked next to A/B) owns JUMP and RELOAD, the two actions you hit
// mid-fight, and the left hand takes Use and Med hypo, which you press while
// standing still anyway:
//     right A -> Jump     right B -> Hack / Reload
//     left  X -> Use      left  Y -> Med hypo

// D-pad modifier telemetry. Three separate things can silently suppress it and
// none of them logged anything, so "it doesn't work" was unfalsifiable.
static bool  s_dbgRest = false, s_dbgThumb = false, s_dbgMenu = false, s_dbgMod = false;
static float s_dbgGripL = 0.0f, s_dbgGripR = 0.0f;

static void FillFromPad(const PadState& s, XI_STATE* out)
{
    ZeroMemory(out, sizeof(*out));

    // Index grips read high from a RESTING hand, so a single 0.5 threshold
    // leaves LB/RB permanently held and BioShock permanently in its radial
    // context, where face buttons get eaten. Hysteresis: a deliberate squeeze
    // to engage, a real release to let go.
    //
    // Computed HERE, at the top, because three separate things below need the
    // answer -- the d-pad modifier, the radial test, and LB/RB themselves.
    static bool gripLOn = false, gripROn = false;
    const float onT = g_cfg.gripThreshold;
    const float offT = g_cfg.gripThreshold - g_cfg.gripHysteresis;
    gripLOn = gripLOn ? (s.gripL > offT) : (s.gripL > onT);
    gripROn = gripROn ? (s.gripR > offT) : (s.gripR > onT);

    // Was DrawHook_MenuUp(), the legacy draw-signature detector. MEASURED: it
    // reads TRUE through normal gameplay -- its MenuMaxIndexed rule fires on any
    // low-geometry frame -- so it silently disabled the d-pad modifier the
    // entire time. GameState_Paused reads the game's own Level.Pauser and is
    // the signal that actually means a full-screen UI is up.
    const bool menuUp = GameState_Paused();

    // ---- is the d-pad modifier held? -----------------------------------
    bool mod = false;
    switch (g_cfg.dpadModifier)
    {
    case 1: mod = g_cfg.dpadFlip ? s.restL : s.restR;   break;
    case 4: mod = s.restL;   break;      // explicit left thumbrest
    case 2: mod = s.thumbR;  break;
    case 3: mod = gripLOn; break;
    default: mod = false;    break;
    }
    if (menuUp) mod = false;                          // menus navigate on the stick
    if (gripLOn || gripROn) mod = false;               // radial owns the sticks
    s_dbgRest = s.restR;   s_dbgThumb = s.thumbR;
    s_dbgGripL = s.gripL;  s_dbgGripR = s.gripR;
    s_dbgMenu = menuUp;    s_dbgMod = mod;

    if (mod)
    {
        // Dominant axis only -- diagonals on a thumbstick are too easy to hit
        // by accident when the intent is a single direction.
        int dir = 0;   // 1 up, 2 down, 3 left, 4 right
        // FLIPPED: read the RIGHT stick and let the left keep walking you
        // around. Unflipped keeps the original left-stick behaviour.
        const float dx = g_cfg.dpadFlip ? s.turnX : s.moveX;
        const float dy = g_cfg.dpadFlip ? s.turnY : s.moveY;
        const float ax = fabsf(dx), ay = fabsf(dy);
        if (ay >= 0.5f && ay >= ax)      dir = (dy > 0.0f) ? 1 : 2;
        else if (ax >= 0.5f && ax > ay)  dir = (dx > 0.0f) ? 4 : 3;

        // S38: HELD, not pulsed. The first version emitted a 120ms pulse to
        // avoid weapon-switch spam -- but weapons are on the RADIAL in this
        // game, and the d-pad drives HUD functions. One of those is the hint
        // button, and ShockPlayerController gates the MAP SCREEN behind
        // HintButtonHeld with HintHoldTime=0.5s. A pulse made the map
        // unreachable by construction. Holding costs nothing and buys it back.
        switch (dir)
        {
        case 1: out->Gamepad.wButtons |= XI_DPAD_UP;    break;
        case 2: out->Gamepad.wButtons |= XI_DPAD_DOWN;  break;
        case 3: out->Gamepad.wButtons |= XI_DPAD_LEFT;  break;
        case 4: out->Gamepad.wButtons |= XI_DPAD_RIGHT; break;
        default: break;
        }

        // In FLIPPED mode the left stick still walks -- only turning is eaten.
        if (g_cfg.dpadFlip)
        {
            out->Gamepad.sThumbLX = ToAxis(s.moveX);
            out->Gamepad.sThumbLY = ToAxis(s.moveY);
        }
        // Unflipped: left stick contributes NO movement while held.
    }
    else
    {
        float mx = s.moveX, my = s.moveY;

        // ---- MOVEMENT MODE: ROTATE THE STICK, NEVER THE AIM FIELD -----------
        // The game measures the walk direction from the aim field and then
        // applies the stick angle, so adding R to the stick redirects walking
        // while leaving aim, the weapon trace and forced-move sequences alone:
        //
        //     walk = aimFieldYaw + stickAngle + R
        //
        // R IS NOT COMPUTED HERE ANY MORE, and that is the fix for a residual
        // coupling the tester measured as "subtle, and inverse of where you are
        // pointing". It used to be built from the head yaw and the controller's
        // offset, on the assumption that the aim field holds their SUM. It does
        // not: the field is written by ComposeHeadLocal, a basis multiplication,
        // whose yaw depends on the pitch as well. The error was zero only when
        // the controller was perfectly level.
        //
        // CameraHook now computes R at the write site, from the rotators it
        // actually wrote, and publishes one number. It cancels the composition
        // by construction at any pitch, and the mode table lives next to the
        // values it is about. See the banner above PublishWalkRotation.
        //
        // WHY THIS IS NOT GRAVEYARD ENTRY 13. That entry says no arrangement of
        // Controller.Rotation separates view, weapon trace and walk direction.
        // True -- and this arranges nothing: the field is never written here. It
        // binds AIM. Locomotion was always separable and mode 3 has been doing it
        // by this exact mechanism since HeadRelativeMove shipped.
        //
        // GATING. This is for LOCOMOTION only. The radial wheels and menus read
        // this same stick as a SCREEN-relative direction -- rotating it there
        // means stick-up stops selecting the top entry whenever your head is
        // turned. Theater and non-gameplay contexts get the raw stick too. These
        // gates used to apply to mode 1 alone; all four need them.
        const bool stickRotOk =
            g_cfg.headTracking &&
            !menuUp &&
            !gripLOn && !gripROn &&
            !GameState_Theater();

        bool rotated = false;
        if (stickRotOk && g_cfg.movementMode != 1)
        {
            float R = 0.0f;
            if (CameraHook_GetWalkRotation(&R))
            {
                const float r = R * 0.01745329f;      // deg -> rad
                const float c = cosf(r), sn = sinf(r);
                const float nx = mx * c + my * sn;
                const float ny = -mx * sn + my * c;
                mx = nx; my = ny;

                // ---- KEEP THE SPEED THE PLAYER ASKED FOR --------------------
                // ToAxis() clamps each component to +-1 INDEPENDENTLY, so the
                // pair the game receives is a SQUARE while this rotation is
                // circular. A full diagonal push has magnitude 1.41; rotate it
                // onto an axis and that component is clipped back to 1.0, and
                // how much gets clipped depends on the angle. Reported as
                // "you speed up when the controller is turned in certain
                // directions".
                //
                // Scaling by 1/max preserves the DIRECTION and caps speed
                // uniformly, which is the honest reading of the stick. Clipping
                // would change the direction as well as the speed.
                //
                // PREDATES THE FOUR MODES: HeadRelativeMove has rotated the
                // stick this way since it shipped and had the same artifact.
                const float ax = fabsf(mx), ay = fabsf(my);
                const float peak = (ax > ay) ? ax : ay;
                if (peak > 1.0f)
                {
                    const float k = 1.0f / peak;
                    mx *= k; my *= k;
                }

                rotated = true;
            }
        }

        // ---- PUBLISH THE ANGLE WE INTEND THE GAME TO WALK AT ---------------
        // For WalkDriftProbe, which lives in CalcView and cannot see the stick.
        // Without this it had to assume the player was pushing exactly forward,
        // and that assumption was worth +-7 degrees of noise -- as large as the
        // drift being hunted.
        //
        // BEFORE the deadzone pre-compensation on purpose. This is the direction
        // we WANT, so `recv - sent` reads as the error the game introduced --
        // which is the whole point of the probe, and is what proves the
        // compensation worked rather than merely changed something.
        //
        // The game's axes are (forward, strafe) and its yaw grows from +X toward
        // +Y, so the stick angle is atan2(strafe, forward) with forward = my.
        // Published even when centred, as 0, so a stale value cannot masquerade
        // as a push.
        PublishSentStickAngle(mx, my);

        // ONLY WHEN WE ROTATED. With no rotation the game's own square deadzone
        // acts on an unrotated stick exactly as it always has, which is the
        // vanilla feel the player already knows -- compensating there would
        // change something that is not broken. The distortion only appears once
        // rotation moves magnitude between the two axes.
        if (rotated) PrecompStickDeadzone(&mx, &my);
        out->Gamepad.sThumbLX = ToAxis(mx);
        out->Gamepad.sThumbLY = ToAxis(my);
    }

    // Flipped mode owns the right stick while the modifier is held, so the
    // d-pad direction must not ALSO snap-turn you.
    if (!(mod && g_cfg.dpadFlip))
    {
        // Snap turn and mod-yaw both rotate g_aimBase directly. Sending the axis
        // as well would turn you twice, at two different rates.
        //
        // ---- M7-S2: HAND THE TURN PATH BACK DURING SCRIPTED SEQUENCES -------
        // THIS IS GRAVE 12'S MECHANISM. With ModYaw on, this line zeroes the
        // axis permanently, so the game's own turn path never runs -- and
        // CameraHook simultaneously overwrites Controller.Rotation from
        // g_aimBase every frame. A forced-move sequence steers by that field, so
        // it could not turn the player: the opening bathysphere walked into the
        // back wall. Releasing the axis here, and the aim write in CameraHook,
        // is what makes mod-side yaw safe to leave on during ordinary play.
        //
        // ⚠ THE BATHYSPHERE IS THE ONE CASE THIS IS NOT MEASURED ON. M7-S1
        // proved the signal on a scripted scene and proved it does NOT cover
        // the rescue or the EVE injection. If the bathysphere likewise does not
        // set hands+0x594 bit 2, this release never fires there and grave 12
        // comes straight back. Test the opening bathysphere FIRST.
        const bool modOwnsTurn =
            (g_cfg.snapTurn || g_cfg.modYaw) && !ScriptedQol();

        out->Gamepad.sThumbRX = modOwnsTurn ? 0 : ToAxis(TurnResponse(s.turnX));
    }

    // Right-stick Y is normally DROPPED (HeadAimMode=2 erases injected pitch).
    // But [RadialActive] rebinds this same axis to yRadialRight, so while a
    // grip is held the weapon/plasmid radial NEEDS it or its top and bottom
    // entries are unreachable.
    const bool radialOpen = gripLOn || gripROn;
    if (g_cfg.controllerPitch || menuUp || radialOpen)
    {
        out->Gamepad.sThumbRY = ToAxis(s.turnY);
    }
    else if (g_cfg.pitchServo)
    {
        // Drive the engine's hidden pitch toward where you are actually
        // looking. Melee aims from that pitch, so without this the wrench
        // lands wherever the engine last happened to leave it.
        float err = 0.0f;
        if (CameraHook_GetPitchError(&err))
        {
            if (err > g_cfg.pitchServoDead || err < -g_cfg.pitchServoDead)
            {
                float v = err * g_cfg.pitchServoGain;
                if (v > g_cfg.pitchServoMax) v = g_cfg.pitchServoMax;
                if (v < -g_cfg.pitchServoMax) v = -g_cfg.pitchServoMax;
                out->Gamepad.sThumbRY = ToAxis(v);

                if (g_cfg.swingLog)
                {
                    static ULONGLONG lastP = 0;
                    const ULONGLONG nowP = GetTickCount64();
                    if (nowP - lastP > 1000)
                    {
                        lastP = nowP;
                        Log("  PITCHSERVO: err %+.1f deg -> RY %+.2f", err, v);
                    }
                }
            }
        }
    }
    else if (g_cfg.stickYToDpad && !mod)
    {
        if (s.turnY > 0.6f) out->Gamepad.wButtons |= XI_DPAD_UP;
        else if (s.turnY < -0.6f) out->Gamepad.wButtons |= XI_DPAD_DOWN;
    }

    out->Gamepad.bLeftTrigger = ToTrigger(s.trigL);
    out->Gamepad.bRightTrigger = ToTrigger(s.trigR);
    if (Swing_RightTriggerActive()) out->Gamepad.bRightTrigger = 255;

    // Logical actions first, THEN the XInput bit the game expects for each.
    bool aUse, aHypo, aHack, aJump;
    if (g_cfg.controllerLayout == 1)
    {
        aJump = s.a;   aHack = s.b;  aUse = s.x;   aHypo = s.y;
    }
    else
    {
        aUse = s.a;    aHypo = s.b;  aHack = s.x;   aJump = s.y;
    }

    WORD btn = out->Gamepad.wButtons;
    if (aUse)            btn |= XI_A;             // XENON_A = Use
    if (aHypo)           btn |= XI_B;             // XENON_B = Med hypo
    if (aHack)           btn |= XI_X;             // XENON_X = Hack / Reload

    // Y is suppressed ONLY while X is actually held -- not permanently. Killing
    // it outright also killed the hacking mini-game, which uses Y to speed up
    // the pipe. The cost is that pressing Y a frame before X can produce one
    // frame of jump on the way into a pause, which is a small hop and nothing
    // worse. R3 stays available as a second jump either way.
    if (aJump && !(g_cfg.pauseChord && s.x)) btn |= XI_Y;
    if (g_cfg.pauseChord && s.thumbR)        btn |= XI_Y;

    // R3 -> JUMP. The game binds R3 to Zoom, which is unusable in VR anyway --
    // ADS drives ZoomedForegroundFOVAngle and breaks the ForegroundFov
    // calibration. ADDITIVE: the layout's normal jump button still jumps, so
    // this cannot take anything away.
    if (g_cfg.jumpOnR3 && s.thumbR) btn |= XI_Y;

    // Touch has ONE application menu button, and the game wants both START
    // (pause) and BACK (ShowContextHelp -- the "WHAT IS THIS?" prompt). The
    // modifier disambiguates: menu alone pauses, modifier+menu is context help.
    // PAUSE. The menu button is not reliably ours: SteamVR takes the left one
    // for its dashboard, the Meta runtime eats the right one, and Quest's
    // "swap Oculus and Menu button" accessibility option sends BOTH to the
    // system. X+Y together is reserved by nobody, on any runtime.
    bool chordPause = false;
    if (g_cfg.pauseChord && s.x && s.y)
    {
        chordPause = true;
        btn &= ~(WORD)(XI_A | XI_B | XI_X | XI_Y);   // don't hack+jump mid-pause

        static bool wasChord = false;
        if (!wasChord) { wasChord = true; Log("  PAUSE: X+Y chord -> START"); }
    }

    // MODIFIER + X+Y = BACK (ShowContextHelp / "WHAT IS THIS?"). Held, because
    // ShockPlayerController gates the MAP behind HintButtonHeld at
    // HintHoldTime=0.5s -- so a tap gives the prompt and holding reaches the map.
    // X+Y alone = START (pause).
    if (s.menu)     btn |= (mod ? XI_BACK : XI_START);
    if (chordPause) btn |= (mod ? XI_BACK : XI_START);
    if (s.thumbL)   btn |= XI_LTHUMB;
    if (s.thumbL)   btn |= XI_LTHUMB;

    // A control used as the modifier must not ALSO send its normal button, or
    // every d-pad press would come with a stray R3 / LB. Same for one rebound
    // to jump: without this, every jump would also zoom.
    if (s.thumbR && g_cfg.dpadModifier != 2 && !g_cfg.jumpOnR3) btn |= XI_RTHUMB;
    if (gripLOn && g_cfg.dpadModifier != 3) btn |= XI_LSHOULDER;
    if (gripROn)                           btn |= XI_RSHOULDER;

    out->Gamepad.wButtons = btn;
}

// Games poll dwPacketNumber to detect change; a constant value makes some of
// them ignore the state entirely. Bump only on an actual change so we do not
// look like a pad that is jittering every frame.
static DWORD SynthState(DWORD idx, XI_STATE* st)
{
    if (idx != 0) return XI_ERR_DEVICE_NOT_CONNECTED;   // one virtual pad, slot 0

    PadState s = {};
    if (!ReadPad(&s) || !s.active)
    {
        ZeroMemory(st, sizeof(*st));
        static DWORD neutralPkt = 0;
        st->dwPacketNumber = neutralPkt;
        return XI_ERR_SUCCESS;
    }

    XI_STATE fresh = {};
    FillFromPad(s, &fresh);

    static XI_GAMEPAD lastPad = {};
    static DWORD      pkt = 1;
    if (memcmp(&fresh.Gamepad, &lastPad, sizeof(XI_GAMEPAD)) != 0)
    {
        lastPad = fresh.Gamepad;
        ++pkt;
    }
    fresh.dwPacketNumber = pkt;

    *st = fresh;
    _InterlockedIncrement(&g_nSynth);
    return XI_ERR_SUCCESS;
}

static DWORD WINAPI hkXInputGetState(DWORD idx, XI_STATE* st)
{
    _InterlockedIncrement(&g_nGetState);
    if (!st) return XI_ERR_DEVICE_NOT_CONNECTED;

    // merge mode: a REAL pad plugged into slot 0 wins outright. That keeps the
    // "verify with a real Xbox pad first" test honest -- with our hook loaded,
    // a real pad still behaves exactly as if we were not here.
    if (g_cfg.controllerMode == 0 && g_origGetState)
    {
        const DWORD r = g_origGetState(idx, st);
        if (r == XI_ERR_SUCCESS)
        {
            _InterlockedIncrement(&g_nRealPad);
            return r;
        }
    }
    return SynthState(idx, st);
}

static DWORD WINAPI hkXInputGetStateEx(DWORD idx, XI_STATE* st)
{
    _InterlockedIncrement(&g_nGetState);
    if (!st) return XI_ERR_DEVICE_NOT_CONNECTED;

    if (g_cfg.controllerMode == 0 && g_origGetStateEx)
    {
        const DWORD r = g_origGetStateEx(idx, st);
        if (r == XI_ERR_SUCCESS)
        {
            _InterlockedIncrement(&g_nRealPad);
            return r;
        }
    }
    return SynthState(idx, st);
}

// A game that asks "is there a pad?" and hears no may never call GetState at
// all. Answer yes for slot 0 so the polling loop starts.
static DWORD WINAPI hkXInputGetCapabilities(DWORD idx, DWORD flags, XI_CAPS* caps)
{
    _InterlockedIncrement(&g_nGetCaps);

    if (g_cfg.controllerMode == 0 && g_origGetCaps)
    {
        const DWORD r = g_origGetCaps(idx, flags, caps);
        if (r == XI_ERR_SUCCESS) return r;
    }
    if (idx != 0 || !caps) return XI_ERR_DEVICE_NOT_CONNECTED;

    ZeroMemory(caps, sizeof(*caps));
    caps->Type = 1;        // XINPUT_DEVTYPE_GAMEPAD
    caps->SubType = 1;        // XINPUT_DEVSUBTYPE_GAMEPAD
    caps->Flags = 0;
    // Report every field as available -- this is a capability mask, not a state.
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = (SHORT)0xFFC0;
    caps->Gamepad.sThumbLY = (SHORT)0xFFC0;
    caps->Gamepad.sThumbRX = (SHORT)0xFFC0;
    caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
    caps->Vibration.wLeftMotorSpeed = 0xFF;
    caps->Vibration.wRightMotorSpeed = 0xFF;
    return XI_ERR_SUCCESS;
}

// ---------------------------------------------------------------- install
//
// GetModuleHandle, never LoadLibrary: hook the DLL the game ALREADY chose. If
// we loaded one ourselves we would be hooking a copy nobody calls, and the
// heartbeat would show a perfectly healthy hook with zero traffic -- the most
// expensive kind of wrong.

// S33: hook the XInput DLL the GAME imports, not the first one we trip over.
//
// The original version broke out of the search loop on the first hit and armed
// on xinput1_4.dll -- which on Win10 is loaded by the STEAM OVERLAY. dumpbin
// says BioshockHD.exe statically imports XINPUT1_3.dll, so the game's calls
// went through a module we never touched and the log showed a healthy hook
// with getState 0/s forever.
//
// Fixed by reading the exe's own import directory: it is mapped in memory, it
// is authoritative, and it also keeps us from feeding a fake pad to the Steam
// overlay (which would happen if we blanket-hooked every loaded XInput DLL).
static const char* kXInputDlls[] = {
    "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll",
    "xinput1_2.dll", "xinput1_1.dll",
};
static const int kNumXInputDlls = (int)(sizeof(kXInputDlls) / sizeof(kXInputDlls[0]));

static bool g_hookedDll[kNumXInputDlls] = {};
static bool g_exeImports[kNumXInputDlls] = {};
static bool g_importsParsed = false;
static bool g_anyImported = false;

// Walk the main module's import descriptors. Logs every input-ish DLL the exe
// declares, and flags which of our XInput candidates are real imports.
static void ParseExeImports()
{
    if (g_importsParsed) return;
    g_importsParsed = true;

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return;

    __try
    {
        BYTE* base = (BYTE*)exe;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;

        const DWORD rva =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) return;

        Log(">>> INPUT: ---- exe import table (input DLLs) ----");

        IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
        for (; imp->Name; ++imp)
        {
            const char* name = (const char*)(base + imp->Name);

            char low[64] = {};
            strncpy_s(low, name, _TRUNCATE);
            _strlwr_s(low);

            if (!strstr(low, "input")) continue;
            Log(">>> INPUT:   imports %s", name);

            for (int i = 0; i < kNumXInputDlls; ++i)
            {
                if (_stricmp(low, kXInputDlls[i]) == 0)
                {
                    g_exeImports[i] = true;
                    g_anyImported = true;
                }
            }
        }

        if (!g_anyImported)
            Log(">>> INPUT:   (no XInput DLL statically imported -- will hook whatever is loaded)");
        Log(">>> INPUT: --------------------------------------");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(">>> INPUT: !!! import table walk faulted. Falling back to loaded-module scan.");
        g_anyImported = false;
    }
}

// Modules worth knowing about. dinput8 alongside xinput is normal for this
// engine generation: DirectInput for keyboard/mouse, XInput for the pad.
static void SurveyInputModules()
{
    static bool done = false;
    if (done) return;
    done = true;

    Log(">>> INPUT: ---- loaded input modules ----");

    static const char* kProbe[] = {
        "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll",
        "xinput1_2.dll", "xinput1_1.dll", "xinput.dll",
        "dinput8.dll",   "dinput.dll",
        "gameinput.dll", "hid.dll",       "winmm.dll",
    };

    int found = 0;
    for (int i = 0; i < (int)(sizeof(kProbe) / sizeof(kProbe[0])); ++i)
    {
        HMODULE m = GetModuleHandleA(kProbe[i]);
        if (!m) continue;
        ++found;

        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, MAX_PATH);
        Log(">>> INPUT:   %-16s base 0x%08X  %s",
            kProbe[i], (unsigned)(uintptr_t)m, path);
    }
    if (!found) Log(">>> INPUT:   (none -- the game has not loaded ANY input DLL)");
    Log(">>> INPUT: --------------------------------");
}

// The exe imports XInput BY ORDINAL, so the dump cannot tell us which function
// ordinals 2 and 3 are. Resolve both ways and compare pointers: whichever named
// export shares an address with an ordinal IS that ordinal. Prints once per DLL.
static void LogOrdinalMap(HMODULE mod, const char* which)
{
    static const char* kNames[] = {
        "XInputEnable", "XInputGetState", "XInputSetState",
        "XInputGetCapabilities", "XInputGetKeystroke",
        "XInputGetBatteryInformation", "XInputGetDSoundAudioDeviceGuids",
        "XInputGetAudioDeviceIds",
    };
    const int nNames = (int)(sizeof(kNames) / sizeof(kNames[0]));

    void* nameAddr[8] = {};
    for (int i = 0; i < nNames; ++i)
        nameAddr[i] = (void*)GetProcAddress(mod, kNames[i]);

    Log(">>> INPUT:   %s ordinal map:", which);
    static const int kOrds[] = { 1, 2, 3, 4, 5, 6, 7, 100, 101, 102, 103 };
    for (int i = 0; i < (int)(sizeof(kOrds) / sizeof(kOrds[0])); ++i)
    {
        void* a = (void*)GetProcAddress(mod, MAKEINTRESOURCEA(kOrds[i]));
        if (!a) continue;

        const char* match = "(unnamed)";
        for (int j = 0; j < nNames; ++j)
            if (nameAddr[j] && nameAddr[j] == a) { match = kNames[j]; break; }

        if (kOrds[i] == 100 && strcmp(match, "(unnamed)") == 0) match = "XInputGetStateEx (assumed)";

        Log(">>> INPUT:     ord %-3d -> 0x%08X  %s", kOrds[i], (unsigned)(uintptr_t)a, match);
    }
}

static bool HookOneDll(int idx)
{
    HMODULE mod = GetModuleHandleA(kXInputDlls[idx]);
    if (!mod) return false;

    const char* which = kXInputDlls[idx];

    void* pState = (void*)GetProcAddress(mod, "XInputGetState");
    void* pCaps = (void*)GetProcAddress(mod, "XInputGetCapabilities");
    void* pStateEx = (void*)GetProcAddress(mod, MAKEINTRESOURCEA(100));   // XInputGetStateEx

    Log(">>> INPUT: %s -- GetState=0x%08X GetStateEx(ord100)=0x%08X GetCaps=0x%08X",
        which, (unsigned)(uintptr_t)pState, (unsigned)(uintptr_t)pStateEx,
        (unsigned)(uintptr_t)pCaps);

    LogOrdinalMap(mod, which);

    if (!pState && !pStateEx)
    {
        Log(">>> INPUT: !!! %s resolved no entry points. Nothing to hook.", which);
        g_hookedDll[idx] = true;      // don't retry this one forever
        return false;
    }

    bool any = false;

    // Passthrough trampolines: keep the FIRST one we get. Every XInput DLL
    // proxies the same physical devices, so which trampoline merge mode calls
    // to check for a real pad is immaterial -- it is the same answer.
    if (pState)
    {
        XInputGetStateFn tramp = nullptr;
        if (MH_CreateHook(pState, &hkXInputGetState, (LPVOID*)&tramp) == MH_OK &&
            MH_EnableHook(pState) == MH_OK)
        {
            if (!g_origGetState) g_origGetState = tramp;
            if (!g_addrGetState) g_addrGetState = pState;
            any = true;
            Log(">>> INPUT:   hooked %s!XInputGetState", which);
        }
        else Log(">>> INPUT: !!! hook %s!XInputGetState FAILED", which);
    }

    // Ordinal 100 is usually a distinct function; if the export table aliases
    // it to XInputGetState, MinHook rejects the duplicate and we skip it.
    if (pStateEx && pStateEx != pState)
    {
        XInputGetStateFn tramp = nullptr;
        if (MH_CreateHook(pStateEx, &hkXInputGetStateEx, (LPVOID*)&tramp) == MH_OK &&
            MH_EnableHook(pStateEx) == MH_OK)
        {
            if (!g_origGetStateEx) g_origGetStateEx = tramp;
            if (!g_addrGetStateEx) g_addrGetStateEx = pStateEx;
            any = true;
            Log(">>> INPUT:   hooked %s!XInputGetStateEx (ord 100)", which);
        }
        else Log(">>> INPUT:   (%s ordinal 100 not hooked -- usually harmless)", which);
    }

    if (pCaps)
    {
        XInputGetCapsFn tramp = nullptr;
        if (MH_CreateHook(pCaps, &hkXInputGetCapabilities, (LPVOID*)&tramp) == MH_OK &&
            MH_EnableHook(pCaps) == MH_OK)
        {
            if (!g_origGetCaps) g_origGetCaps = tramp;
            if (!g_addrGetCaps) g_addrGetCaps = pCaps;
            Log(">>> INPUT:   hooked %s!XInputGetCapabilities", which);
        }
        else Log(">>> INPUT: !!! hook %s!XInputGetCapabilities FAILED", which);
    }

    g_hookedDll[idx] = true;
    if (any) Log(">>> INPUT: XINPUT HOOK ARMED on %s", which);
    return any;
}

static bool TryInstall()
{
    SurveyInputModules();
    ParseExeImports();

    bool any = false;
    for (int i = 0; i < kNumXInputDlls; ++i)
    {
        if (g_hookedDll[i]) continue;             // already handled this module
        if (g_anyImported && !g_exeImports[i])    // the exe does not use this one
        {
            g_hookedDll[i] = true;                // leave Steam's copy alone
            continue;
        }
        if (HookOneDll(i)) any = true;
    }

    if (any) g_installed = true;
    return g_installed;
}

void Input_Tick()
{
    if (!g_cfg.controller) return;

    // The game may load its XInput DLL lazily, well after our first Present, and
    // may load a SECOND one later still -- so keep scanning even once armed.
    // Retry for 60s, then say so once and stop burning the check.
    if (!g_gaveUp)
    {
        const DWORD now = GetTickCount();
        if (!g_installDeadline) g_installDeadline = now + 60000;

        static DWORD nextTry = 0;
        if (now >= nextTry)
        {
            nextTry = now + 500;
            TryInstall();
            if (!g_installed && now > g_installDeadline)
            {
                g_gaveUp = true;
                Log(">>> INPUT: !!! no XInput DLL loaded after 60s.");
                Log(">>> INPUT: !!! The game never asked for controller support, so there is");
                Log(">>> INPUT: !!! nothing to hook. This is the case where the Bioshock.ini");
                Log(">>> INPUT: !!! UseJoystick / UseController keys actually do matter.");
            }
            else if (g_installed && now > g_installDeadline)
            {
                g_gaveUp = true;      // armed and settled; stop rescanning
            }
        }
    }

    if (!g_cfg.controllerLog) return;

    static DWORD lastTick = 0;
    const DWORD now = GetTickCount();
    if (now - lastTick < 1000) return;
    lastTick = now;

    static long lastGet = 0, lastCaps = 0, lastSynth = 0, lastReal = 0;
    const long gs = g_nGetState, cp = g_nGetCaps, sy = g_nSynth, rp = g_nRealPad;

    // THE diagnostic line. getState/s == 0 means the game is not polling and no
    // amount of work in this file will change anything.
    Log("  POLL: getState %ld/s  getCaps %ld/s  synth %ld/s  realpad %ld/s  hook=%s xr=%s",
        gs - lastGet, cp - lastCaps, sy - lastSynth, rp - lastReal,
        g_installed ? "ON" : "off", g_xrReady ? "ON" : "off");
    Log("  DPAD: restR=%d thumbR=%d gripL=%.2f gripR=%.2f menuUp=%d -> modifier %s",
        (int)s_dbgRest, (int)s_dbgThumb, s_dbgGripL, s_dbgGripR,
        (int)s_dbgMenu, s_dbgMod ? "HELD" : "off");

    lastGet = gs; lastCaps = cp; lastSynth = sy; lastReal = rp;

    // Hand poses. Yaw/pitch here are for EYEBALLING only -- point straight
    // ahead and yaw should read near 0; point right, yaw goes positive.
    for (int h = 0; h < 2; ++h)
    {
        HandPose hp = {};
        if (!Input_GetHandPose(h, &hp)) continue;
        if (!hp.aimValid) { Log("  HAND%s: aim NOT TRACKED", h ? "R" : "L"); continue; }

        float f[3];
        QuatForward(hp.aimQuat, f);
        const double yaw = atan2((double)f[0], -(double)f[2]) * 57.2957795;
        double fy = f[1]; if (fy > 1.0) fy = 1.0; if (fy < -1.0) fy = -1.0;
        const double pitch = asin(fy) * 57.2957795;

        Log("  HAND%s: aim yaw %+6.1f  pitch %+6.1f deg   pos %+5.2f %+5.2f %+5.2f m   grip %s",
            h ? "R" : "L", yaw, pitch,
            hp.aimPos[0], hp.aimPos[1], hp.aimPos[2],
            hp.gripValid ? "ok" : "--");
    }

    PadState s = {};
    if (ReadPad(&s) && s.active)
    {
        const bool anything =
            fabsf(s.moveX) + fabsf(s.moveY) + fabsf(s.turnX) + fabsf(s.turnY) +
            s.trigL + s.trigR + s.gripL + s.gripR > 0.01f ||
            s.a || s.b || s.x || s.y || s.menu || s.thumbL || s.thumbR || s.restR;

        if (anything)
            Log("  PAD : L %+.2f %+.2f  R %+.2f %+.2f  T %.2f %.2f  G %.2f %.2f  %s%s%s%s%s%s%s%s",
                s.moveX, s.moveY, s.turnX, s.turnY, s.trigL, s.trigR, s.gripL, s.gripR,
                s.a ? "A" : "", s.b ? "B" : "", s.x ? "X" : "", s.y ? "Y" : "",
                s.menu ? " MENU" : "", s.thumbL ? " L3" : "", s.thumbR ? " R3" : "",
                s.restR ? " REST" : "");
    }
}

void Input_Remove()
{
    if (g_addrGetState)   MH_DisableHook(g_addrGetState);
    if (g_addrGetStateEx) MH_DisableHook(g_addrGetStateEx);
    if (g_addrGetCaps)    MH_DisableHook(g_addrGetCaps);
    g_installed = false;
}