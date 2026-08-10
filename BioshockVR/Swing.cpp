// BioshockVR/Swing.cpp
//
// See Swing.h.
//
// WHY HEAD-RELATIVE VELOCITY
// The controller's velocity in world space includes everything the whole player
// is doing: walking, an elevator, a bathysphere, room-scale movement. All of
// that is shared with the head. Subtracting the head position first leaves only
// motion of the hand RELATIVE TO THE BODY, which is what a swing actually is.
// Absolute velocity fires the wrench every time you sprint.
//
// WHY A RE-ARM THRESHOLD
// One swing is a fast out and a fast back. A single threshold fires on both
// halves and double-hits. The detector disarms on fire and only re-arms once
// the hand has genuinely slowed, so the return stroke cannot trigger.

#include "Swing.h"

#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "InputHook.h"      // HandPose, Input_GetHandPose, HAND_LEFT/RIGHT
#include "Config.h"

extern void LogFile(const char* msg);

extern void XR_GetHeadPos(float out[3]);
extern int  HandsProbe_WeaponSlot();
extern bool GameState_Paused();
extern bool GameState_Theater();
extern bool Input_WeaponWheelHeld();

// ini, all wired in dllmain.cpp

static bool  g_armed = true;
static bool  g_havePrev = false;
static float g_armPos[3] = {};      // where the hand was when it re-armed
static bool  g_haveArmPos = false;
static float g_prevRel[3] = {};
static ULONGLONG g_prevTime = 0;
static ULONGLONG g_cooldownUntil = 0;
static ULONGLONG g_pulseBegin = 0;
static ULONGLONG g_pulseEnd = 0;
static bool  g_loggedFirst = false;

static void Log(const char* fmt, ...)
{
    char b[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

void Swing_Reset()
{
    g_havePrev = false;
    g_armed = true;
    g_pulseBegin = 0;
    g_pulseEnd = 0;
}

void Swing_Update()
{
    if (!g_cfg.swingEnabled) { Swing_Reset(); return; }

    HandPose right = {};
    if (!Input_GetHandPose(HAND_RIGHT, &right) || !right.gripValid)
    {
        // No pose this frame. Drop the history rather than measuring across the
        // gap -- the distance covered while tracking was lost would read as one
        // enormous swing the moment it comes back.
        g_havePrev = false;
        return;
    }

    float head[3] = {};
    XR_GetHeadPos(head);

    const float rel[3] = {
        right.gripPos[0] - head[0],
        right.gripPos[1] - head[1],
        right.gripPos[2] - head[2],
    };

    const ULONGLONG now = GetTickCount64();

    if (!g_havePrev)
    {
        memcpy(g_prevRel, rel, sizeof(rel));
        g_prevTime = now;
        g_havePrev = true;
        return;
    }

    const float dt = (float)(now - g_prevTime) * 0.001f;

    // dt == 0 divides by zero. dt > 0.25 means a hitch, a load, or an alt-tab,
    // and the hand may legitimately be somewhere completely different. Neither
    // is a measurement.
    if (dt <= 0.0f || dt > 0.25f)
    {
        memcpy(g_prevRel, rel, sizeof(rel));
        g_prevTime = now;
        return;
    }

    const float dx = rel[0] - g_prevRel[0];
    const float dy = rel[1] - g_prevRel[1];
    const float dz = rel[2] - g_prevRel[2];
    const float speed = sqrtf(dx * dx + dy * dy + dz * dz) / dt;

    memcpy(g_prevRel, rel, sizeof(rel));
    g_prevTime = now;

    if (speed <= g_cfg.swingRearm)
    {
        g_armed = true;
        // Remember where the hand settled. Travel is measured from HERE, so a
        // wrist flick cannot accumulate enough distance to count as a swing.
        memcpy(g_armPos, rel, sizeof(g_armPos));
        g_haveArmPos = true;
    }

    // ---- DIRECTION -------------------------------------------------------
    // Scalar speed has no direction, so a fast wind-up BACKWARD looked
    // identical to the forward strike. The game then started its attack early
    // and evaluated collision after the hand had swung down -- which is why it
    // hit the floor about half the time while the trigger was accurate.
    //
    // rel is hand-minus-head, so normalising it gives the outward direction
    // from your body. A real strike has a large positive component along it.
    float outward = 0.0f;
    {
        const float rl = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        if (rl > 1e-4f)
            outward = (dx * rel[0] + dy * rel[1] + dz * rel[2]) / (rl * dt);
    }

    // ---- TRAVEL ----------------------------------------------------------
    float travel = 0.0f;
    if (g_haveArmPos)
    {
        const float tx = rel[0] - g_armPos[0];
        const float ty = rel[1] - g_armPos[1];
        const float tz = rel[2] - g_armPos[2];
        travel = sqrtf(tx * tx + ty * ty + tz * tz);
    }

    // Peak speed once a second, so "it never fires" becomes a number.
    if (g_cfg.swingLog)
    {
        static float peak = 0.0f;
        static ULONGLONG lastPeak = 0;
        if (speed > peak) peak = speed;
        if (now - lastPeak > 1000)
        {
            lastPeak = now;
            Log("  SWING: peak %.2f m/s (need %.2f)  out %+.2f  travel %.2f m  armed=%d slot=%d",
                peak, g_cfg.swingThreshold, outward, travel,
                (int)g_armed, HandsProbe_WeaponSlot());
            peak = 0.0f;
        }
    }

    // Slot 0 is the wrench in the existing weapon table.
    const bool wrench = (HandsProbe_WeaponSlot() == 0);

    // MEASURED: DrawHook_MenuUp() reads true during ordinary gameplay in this
    // build, so it is NOT usable as a swing gate -- it blocked every swing,
    // including one at 8.20 m/s. Pause and theater are the real gates, plus the
    // weapon wheel, which owns both sticks while a grip is held.
    const bool blocked =
        !wrench ||
        GameState_Paused() ||
        GameState_Theater() ||
        Input_WeaponWheelHeld();

    const bool goodDir = (outward >= g_cfg.swingThreshold * g_cfg.swingOutFrac);
    const bool goodTravel = (travel >= g_cfg.swingTravel);

    if (blocked || !g_armed || speed < g_cfg.swingThreshold ||
        !goodDir || !goodTravel || now < g_cooldownUntil)
    {
        if (g_cfg.swingLog && speed >= g_cfg.swingThreshold && blocked)
        {
            static ULONGLONG lastBlock = 0;
            if (now - lastBlock > 1000)
            {
                lastBlock = now;
                Log("  SWING: %.2f m/s but blocked (wrench=%d paused=%d theater=%d wheel=%d)",
                    speed, (int)wrench, (int)GameState_Paused(), (int)GameState_Theater(),
                    (int)Input_WeaponWheelHeld());
            }
        }
        return;
    }

    g_armed = false;
    g_pulseBegin = now + (ULONGLONG)(g_cfg.swingDelayMs < 0 ? 0 : g_cfg.swingDelayMs);
    g_pulseEnd = g_pulseBegin + (ULONGLONG)(g_cfg.swingPulseMs < 20 ? 20 : g_cfg.swingPulseMs);
    g_cooldownUntil = now + (ULONGLONG)(g_cfg.swingCooldownMs < 0 ? 0 : g_cfg.swingCooldownMs);

    if (!g_loggedFirst)
    {
        g_loggedFirst = true;
        Log(">>> SWING: first swing detected at %.2f m/s -- synthetic RT pulse for %d ms.",
            speed, g_cfg.swingPulseMs);
        Log(">>> SWING: too sensitive? raise SwingThreshold. Missing swings? lower it.");
    }
    else if (g_cfg.swingLog)
    {
        Log("  SWING: %.2f m/s -> RT pulse", speed);
    }
}

bool Swing_RightTriggerActive()
{
    if (!g_cfg.swingEnabled || !g_pulseEnd) return false;
    const ULONGLONG now = GetTickCount64();
    return now >= g_pulseBegin && now < g_pulseEnd;
}
