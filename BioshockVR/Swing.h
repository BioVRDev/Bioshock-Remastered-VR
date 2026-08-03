// BioshockVR/Swing.h
//
// Physical wrench swinging.
//
// This does NOT simulate collision. A fast hand motion emits a short synthetic
// right-trigger pulse and BioShock's own wrench attack runs normally -- its
// animation, sound, damage, timing and Havok collision all unchanged. Trying to
// drive the collision directly would mean reimplementing the melee system; this
// borrows it whole.
#pragma once

// Once per OpenXR frame, from XR_SubmitPair after PublishHead(). NOT from
// CalcView -- the stereo camera path runs more than once per presented frame,
// which would sample the same pose twice and compute nonsense velocities.
void Swing_Update();

// True while a synthetic pulse is live. Composed with the physical trigger in
// FillFromPad, never replacing it, so ordinary firing still works.
bool Swing_RightTriggerActive();

// Drop the velocity history. Call on level load, theater entry, or anything
// else that teleports the hand -- otherwise the jump reads as a swing.
void Swing_Reset();
