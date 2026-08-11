// BioshockVR/Input/Zone.h
#pragma once

// ARM / FIRE / RE-ARM, THE PATTERN THIS CODEBASE KEEPS REDISCOVERING.
//
// A threshold that fires once when a measurement crosses it and CANNOT fire
// again until the measurement has left and come back. Swing.cpp implements this
// by hand (grep `g_armed`); holsters and the two-handed grip both need it, and
// all three get it subtly different if each writes its own.
//
// WHY A SEPARATE RE-ARM LEVEL, which is the part that is always got wrong:
// one gesture is a fast out and a fast back. A single threshold fires on BOTH
// halves, so the second half of every draw registers as a stow. The re-arm level
// must be crossed in the other direction before the next fire is allowed, and it
// must be meaningfully below the fire level or the two chatter at the boundary.
//
// WHY dt IS A PARAMETER AND NOT MEASURED HERE: tracking gaps. A hitch, an
// alt-tab or a lost controller produces a huge dt, and any rate computed across
// it is garbage that will sail past any threshold. The caller already knows its
// frame time, so it passes it, and this DROPS THE SAMPLE rather than measuring
// through the gap -- the same discipline as the `if (dt > 0.10f) dt = 0.0f;`
// guard in CameraHook's turn accumulator.
//
// HEADER-ONLY AND STATELESS ACROSS INSTANCES. Each consumer owns a Zone by
// value; nothing here is global, so two features cannot desynchronise each
// other's arming the way two copies of the pattern would.

struct Zone
{
    float fire = 1.0f;      // cross this (upward) to fire
    float rearm = 0.5f;     // fall back below this to be allowed to fire again
    float maxDt = 0.10f;    // above this, the sample is a gap, not a measurement

    bool  armed = true;     // may fire now
    bool  inside = false;   // currently above `fire` -- for callers that want it

    // Returns true on the frame the value crosses `fire` while armed. `dt` is
    // seconds since the last call; pass 0 or a value above maxDt and the sample
    // is DISCARDED -- not treated as zero, discarded, leaving arming untouched.
    bool Update(float value, float dt)
    {
        if (dt <= 0.0f || dt > maxDt) return false;

        if (value <= rearm)
        {
            armed = true;
            inside = false;
            return false;
        }

        if (value < fire) return false;   // between the two levels: hysteresis

        inside = true;
        if (!armed) return false;

        armed = false;
        return true;
    }

    // Level change, actor swap, controller lost. Arms without firing, so the
    // next real crossing is treated as a first crossing rather than as the
    // second half of a gesture that happened before the world changed.
    void Reset()
    {
        armed = true;
        inside = false;
    }
};
