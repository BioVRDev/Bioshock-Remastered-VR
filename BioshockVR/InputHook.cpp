// BioshockVR/InputHook.cpp
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

#include "InputHook.h"

#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>

#include <MinHook.h>

extern void LogFile(const char* msg);

// dllmain.cpp -- see the block at the bottom of this file for the lines to add.
extern bool  g_cfgController;        // EnableController        default 1
extern int   g_cfgControllerMode;    // 0 = merge, 1 = replace  default 0
extern bool  g_cfgControllerPitch;   // emit right-stick Y      default 0
extern bool  g_cfgStickYToDpad;      // dead Y axis -> d-pad    default 0
extern float g_cfgStickDeadzone;     // radial, 0..0.9          default 0.15
extern bool  g_cfgControllerLog;     // heartbeat               default 1
extern int   g_cfgDpadModifier;      // 0 off / 1 thumbrest / 2 R3 / 3 Lgrip  default 1
extern int   g_cfgControllerLayout;  // 0 literal / 1 jump-on-A       default 0
extern bool  g_cfgJumpOnR3;          // R3 -> jump instead of zoom    default 0

bool DrawHook_MenuUp();              // DrawHook.cpp

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
    if (!g_cfgController) { Log(">>> INPUT: DISABLED by ini (EnableController=0)"); return false; }
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
        g_cfgControllerPitch ? "PASSED THROUGH -- expect fighting" : "dropped",
        g_cfgStickDeadzone,
        g_cfgControllerMode ? "replace" : "merge");
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
    Deadzone(&s.moveX, &s.moveY, g_cfgStickDeadzone);
    Deadzone(&s.turnX, &s.turnY, g_cfgStickDeadzone);

    s.trigL = GetFloat(g_aTrigL);
    s.trigR = GetFloat(g_aTrigR);
    s.gripL = GetFloat(g_aGripL);
    s.gripR = GetFloat(g_aGripR);

    s.a = GetBool(g_aA);
    s.b = GetBool(g_aB);
    s.x = GetBool(g_aX);
    s.y = GetBool(g_aY);
    s.menu = GetBool(g_aMenu);
    s.thumbL = GetBool(g_aThumbL);
    s.thumbR = GetBool(g_aThumbR);
    s.restR = GetBool(g_aRestR);

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
static void FillFromPad(const PadState& s, XI_STATE* out)
{
    ZeroMemory(out, sizeof(*out));

    const bool menuUp = DrawHook_MenuUp();

    // ---- is the d-pad modifier held? -----------------------------------
    bool mod = false;
    switch (g_cfgDpadModifier)
    {
    case 1: mod = s.restR;   break;
    case 2: mod = s.thumbR;  break;
    case 3: mod = (s.gripL > 0.5f); break;
    default: mod = false;    break;
    }
    if (menuUp) mod = false;                          // menus navigate on the stick
    if (s.gripL > 0.5f || s.gripR > 0.5f) mod = false; // radial owns the sticks

    if (mod)
    {
        // Dominant axis only -- diagonals on a thumbstick are too easy to hit
        // by accident when the intent is a single direction.
        int dir = 0;   // 1 up, 2 down, 3 left, 4 right
        const float ax = fabsf(s.moveX), ay = fabsf(s.moveY);
        if (ay >= 0.5f && ay >= ax)      dir = (s.moveY > 0.0f) ? 1 : 2;
        else if (ax >= 0.5f && ax > ay)  dir = (s.moveX > 0.0f) ? 4 : 3;

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

        // left stick contributes NO movement while the modifier is held
    }
    else
    {
        out->Gamepad.sThumbLX = ToAxis(s.moveX);
        out->Gamepad.sThumbLY = ToAxis(s.moveY);
    }

    out->Gamepad.sThumbRX = ToAxis(s.turnX);

    // Right-stick Y is normally DROPPED (HeadAimMode=2 erases injected pitch).
    // But [RadialActive] rebinds this same axis to yRadialRight, so while a
    // grip is held the weapon/plasmid radial NEEDS it or its top and bottom
    // entries are unreachable.
    const bool radialOpen = (s.gripL > 0.5f) || (s.gripR > 0.5f);
    if (g_cfgControllerPitch || menuUp || radialOpen)
        out->Gamepad.sThumbRY = ToAxis(s.turnY);
    else if (g_cfgStickYToDpad && !mod)
    {
        if (s.turnY > 0.6f) out->Gamepad.wButtons |= XI_DPAD_UP;
        else if (s.turnY < -0.6f) out->Gamepad.wButtons |= XI_DPAD_DOWN;
    }

    out->Gamepad.bLeftTrigger = ToTrigger(s.trigL);
    out->Gamepad.bRightTrigger = ToTrigger(s.trigR);

    // Logical actions first, THEN the XInput bit the game expects for each.
    bool aUse, aHypo, aHack, aJump;
    if (g_cfgControllerLayout == 1)
    {
        aJump = s.a;   aHack = s.b;  aUse = s.x;   aHypo = s.y;
    }
    else
    {
        aUse = s.a;    aHypo = s.b;  aHack = s.x;   aJump = s.y;
    }

    WORD btn = out->Gamepad.wButtons;
    if (aUse)            btn |= XI_A;      // XENON_A = Use
    if (aHypo)           btn |= XI_B;      // XENON_B = Med hypo
    if (aHack)           btn |= XI_X;      // XENON_X = Hack / Reload
    if (aJump)           btn |= XI_Y;      // XENON_Y = Jump

    // R3 -> JUMP. The game binds R3 to Zoom, which is unusable in VR anyway --
    // ADS drives ZoomedForegroundFOVAngle and breaks the ForegroundFov
    // calibration. ADDITIVE: the layout's normal jump button still jumps, so
    // this cannot take anything away.
    if (g_cfgJumpOnR3 && s.thumbR) btn |= XI_Y;

    // Touch has ONE application menu button, and the game wants both START
    // (pause) and BACK (ShowContextHelp -- the "WHAT IS THIS?" prompt). The
    // modifier disambiguates: menu alone pauses, modifier+menu is context help.
    if (s.menu)          btn |= (mod ? XI_BACK : XI_START);
    if (s.thumbL)        btn |= XI_LTHUMB;

    // A control used as the modifier must not ALSO send its normal button, or
    // every d-pad press would come with a stray R3 / LB. Same for one rebound
    // to jump: without this, every jump would also zoom.
    if (s.thumbR && g_cfgDpadModifier != 2 && !g_cfgJumpOnR3) btn |= XI_RTHUMB;
    if (s.gripL > 0.5f && g_cfgDpadModifier != 3)  btn |= XI_LSHOULDER;
    if (s.gripR > 0.5f)                            btn |= XI_RSHOULDER;

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
    if (g_cfgControllerMode == 0 && g_origGetState)
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

    if (g_cfgControllerMode == 0 && g_origGetStateEx)
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

    if (g_cfgControllerMode == 0 && g_origGetCaps)
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
    if (!g_cfgController) return;

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

    if (!g_cfgControllerLog) return;

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