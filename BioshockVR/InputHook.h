// BioshockVR/InputHook.h
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
void Input_XrSync(XrTime displayTime);

// ---- consumer: call from Hooks.cpp --------------------------------------
// Once per Present. Handles deferred hook installation (the game may load its
// XInput DLL long after our first frame) and the once-a-second log line.
void Input_Tick();

void Input_Remove();
