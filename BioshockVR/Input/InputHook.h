// BioshockVR/Input/InputHook.h
#pragma once

// QUEST TOUCH CONTROLLERS -> VIRTUAL XBOX PAD (Phase B)
//
// Two halves, deliberately independent so either can be diagnosed alone:
//
//   PRODUCER (render thread, OpenXR)  Input_XrCreate / Input_XrSync
//       One action set, synced once per XR frame, published through a seqlock.
//
//   CONSUMER (game input thread)      the XInputGetState detour
//       Reads the seqlock, fills an XINPUT_STATE, hands it to the game.
//
// The two halves never share a lock and never block each other. If the game
// stops polling XInput the producer keeps running harmlessly, and the log says
// exactly which half is dead -- see the once-a-second POLL line.
//
// CRITICAL INTERACTION WITH HeadAimMode=2: the camera hook overwrites pitch
// every CalcView, so any pitch injected through the pad is erased ~8ms later
// and reads as a fight. Right-stick Y is therefore DROPPED by default. Unlike
// the mouse we own this source outright, so this costs nothing.
// ControllerPitch=1 re-enables it for A/B demos of the artifact.

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>

// ---- producer: call from XRSession.cpp ----------------------------------
// ONCE, at the end of XR_Init, BEFORE the session is begun. Action sets can be
// attached to a session exactly once and never again, so every action we will
// ever want -- including the aim/grip poses and haptics that motion controls
// will need -- is created here even though nothing reads them yet. Adding one
// later means restarting the whole session.
bool Input_XrCreate(XrInstance inst, XrSession sess);

// ONCE per XR frame, from BOTH SubmitPair and SubmitMenuMono (menus need input
// too). Safe to call when the session is not focused: it publishes a neutral
// pad instead, so taking the headset off cannot leave the player walking.
//
// baseSpace is the app's LOCAL reference space (XRSession's g_space) -- the SAME
// space the head pose is published in, so hand and head poses are directly
// comparable without a conversion step.
void Input_XrSync(XrTime displayTime, XrSpace baseSpace);

// ---- HAND POSES (motion controls) ---------------------------------------
// Published every XR frame in the app's LOCAL space, through a seqlock, exactly
// like the pad state. FULL 6-DOF for both hands and both pose types, even
// though the first consumer (clamped hybrid aim) uses orientation only:
//
//   aim  -- the runtime's own "pointing" ray. Use this for AIMING. It already
//           accounts for how a Touch controller is held; the grip pose does not.
//   grip -- the physical hold point. Use this for PLACING a weapon model in the
//           hand, if we ever get control of the weapon transform.
//
// Publishing both from day one means the aim consumer and a future 6-DOF weapon
// consumer share one channel and one producer; adding the second one later
// touches nothing that already works.
enum { HAND_LEFT = 0, HAND_RIGHT = 1 };

struct HandPose
{
    float aimQuat[4];    // x,y,z,w  OpenXR LOCAL
    float aimPos[3];     // metres   OpenXR LOCAL
    float gripQuat[4];
    float gripPos[3];
    bool  aimValid;      // false == not tracked this frame; do NOT use the values
    bool  gripValid;
};

// Returns false if the seqlock could not be read cleanly. Check aimValid /
// gripValid separately: a clean read of an untracked hand is still a clean read.
bool Input_GetHandPose(int hand, HandPose* out);

// Right-stick X, -1..1. False when the XR session is not focused. For
// CameraHook's render-side cutscene turn: during input context NullInput the
// game DISCARDS stick input (ShockPlayerController::Use pushes NullInput), so
// reading the stick here is the ONLY way to turn during a scripted sequence.
//
// The original consumer -- CameraHook's render-side cutscene turn (S75/S78/S79)
// -- is RETIRED; it made scripted sequences worse. The live consumer is ModYaw,
// which rotates g_aimBase directly and gives turning the same authority for the
// same reason. See docs/modules/camera.md.
bool Input_GetTurnX(float* out);

// A short confirmation buzz on one hand, at the moment a gesture is recognised.
// hand is HAND_LEFT or HAND_RIGHT, amplitude 0..1, ms clamped to 1..2000.
//
// The haptic actions have been created and bound since motion controls shipped
// and had ZERO callers until this existed. Fails silently -- no haptic support,
// a sleeping controller and an unfocused session all return non-success, and
// none of them should refuse the gesture that asked for the buzz.
void Input_Pulse(int hand, float amplitude, int ms);

// The movement-stick angle last handed to the game, in degrees, in the game's own
// (forward, strafe) convention -- atan2(strafe, forward), so a pure forward push
// rotated by R reads as exactly R. For WalkDriftProbe, which lives in CalcView
// and cannot otherwise see the stick.
bool Input_GetSentStickAngle(float* outDeg);

// ---- consumer: call from Hooks.cpp --------------------------------------
// Once per Present. Handles deferred hook installation (the game may load its
// XInput DLL long after our first frame) and the once-a-second log line.
void Input_Tick();

void Input_Remove();
bool Input_WeaponWheelHeld();