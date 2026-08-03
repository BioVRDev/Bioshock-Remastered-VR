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

extern void LogFile(const char* msg);

extern void XR_GetHeadPos(float out[3]);
extern int  HandsProbe_WeaponSlot();
extern bool GameState_Paused();
extern bool GameState_Theater();
extern bool Input_WeaponWheelHeld();

// ini, all wired in dllmain.cpp
extern int   g_cfgSwingEnabled;      // SwingEnabled
extern float g_cfgSwingThreshold;    // SwingThreshold      m/s
extern float g_cfgSwingRearm;        // SwingRearm          m/s
extern int   g_cfgSwingCooldownMs;   // SwingCooldownMs
extern int   g_cfgSwingPulseMs;      // SwingPulseMs
extern int   g_cfgSwingDelayMs;      // SwingDelayMs
extern int   g_cfgSwingLog;          // SwingLog

static bool  g_armed = true;
static bool  g_havePrev = false;
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
    if (!g_cfgSwingEnabled) { Swing_Reset(); return; }

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

    if (speed <= g_cfgSwingRearm) g_armed = true;

    // Peak speed once a second, so "it never fires" becomes a number.
    if (g_cfgSwingLog)
    {
        static float peak = 0.0f;
        static ULONGLONG lastPeak = 0;
        if (speed > peak) peak = speed;
        if (now - lastPeak > 1000)
        {
            lastPeak = now;
            Log("  SWING: peak %.2f m/s (need %.2f)  armed=%d slot=%d",
                peak, g_cfgSwingThreshold, (int)g_armed, HandsProbe_WeaponSlot());
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

    if (blocked || !g_armed || speed < g_cfgSwingThreshold || now < g_cooldownUntil)
    {
        if (g_cfgSwingLog && speed >= g_cfgSwingThreshold && blocked)
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
    g_pulseBegin = now + (ULONGLONG)(g_cfgSwingDelayMs < 0 ? 0 : g_cfgSwingDelayMs);
    g_pulseEnd = g_pulseBegin + (ULONGLONG)(g_cfgSwingPulseMs < 20 ? 20 : g_cfgSwingPulseMs);
    g_cooldownUntil = now + (ULONGLONG)(g_cfgSwingCooldownMs < 0 ? 0 : g_cfgSwingCooldownMs);

    if (!g_loggedFirst)
    {
        g_loggedFirst = true;
        Log(">>> SWING: first swing detected at %.2f m/s -- synthetic RT pulse for %d ms.",
            speed, g_cfgSwingPulseMs);
        Log(">>> SWING: too sensitive? raise SwingThreshold. Missing swings? lower it.");
    }
    else if (g_cfgSwingLog)
    {
        Log("  SWING: %.2f m/s -> RT pulse", speed);
    }
}

bool Swing_RightTriggerActive()
{
    if (!g_cfgSwingEnabled || !g_pulseEnd) return false;
    const ULONGLONG now = GetTickCount64();
    return now >= g_pulseBegin && now < g_pulseEnd;
}
