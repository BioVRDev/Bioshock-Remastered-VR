// BioshockVR/Hands/ArmHide.h
#pragma once

// HIDING THE FIRST-PERSON FOREARMS WITHOUT HIDING THE HANDS.
//
// The arms are a problem in VR because they are ANIMATED BY THE GAME while the
// hands are being driven by the controllers -- so they stretch from a shoulder
// that isn't where your shoulder is, to a hand that is. Suppressing the whole
// arm draw takes the weapon with it. The fix works at the skeleton instead:
// collapse the ten sleeve bones to zero scale and pin them at the wrist, and
// leave the 34 hand/finger bones and the weapon attachment completely alone.
//
// FAIL-CLOSED BY CONSTRUCTION. Both the AHands and SkeletonInstance vtables are
// verified against expected values, the SkeletonInstance is checked to actually
// belong to this actor, and the rig must report exactly 47 bones. Any mismatch
// and this writes NOTHING -- because a wrong offset here does not produce a
// glitch, it produces a bone matrix full of garbage.
//
// Like EngineExec, this uses absolute addresses we did not derive ourselves, so
// both are ini-overridable (ArmHideHandsVt, ArmHideSkelVt).

// Call once per leader CalcView, after the original has returned. Passing
// hide=false restores the last engine pose and hands the rig back.
bool ArmHide_Update(void* handsActor, bool hide);

// Drop cached pointers on a pawn/world transition. Deliberately does NOT try to
// restore first: by the time a reset is reported the old actor may already be
// destroyed and its address reused.
void ArmHide_Reset();
bool ArmHide_UpdateInactiveHand(void* handsActor, int activeHand);
void ArmHide_ReleaseInactiveHand();

// ---- M7-S4: IS THE RIG ACTUALLY ANIMATING RIGHT NOW? --------------------
// Model-space motion of the right wrist since the previous call, peak-held with
// a decay. Model space matters: the whole actor tracks the camera every frame,
// so a world-space measurement would read "moving" constantly. Only animation
// registers here.
//
// WHY MEASURE AT ALL. The script has no usable "a scripted animation is
// playing" flag. bFinishedStateAnimations was tried and FALSIFIED --
// Hands.uc's PlayingScriptedHandAnimation state has an empty body and never
// touches it -- and ScriptedHandsAnimationHandle is only ever assigned, never
// cleared. Motion answers the question the flags cannot.
//
// Returns false until the skeleton is locked. `outRaw` is the un-smoothed
// per-call value, logged for threshold calibration.
bool ArmHide_HandMotion(float* outSmoothed, float* outRaw);

// ---- M7-S4: HIDE THE WHOLE ACTOR ----------------------------------------
// Arms, hands AND weapon, via the actor's DrawScale3D.
//
// DELIBERATELY NOT A BONE WRITE, and this is the load-bearing reason: hiding by
// collapsing bone clusters would LATCH UP. This file clears the skeleton's
// dirty byte so the engine does not re-evaluate over its writes -- so the
// instant the hands were hidden, the bone ArmHide_HandMotion samples would stop
// moving BECAUSE WE FROZE IT, motion would read zero forever, and the hands
// would never come back. DrawScale3D leaves the bone array untouched, so the
// motion signal stays honest whether the hands are visible or not.
//
// Never writes exact zero -- the attachment path inverse-decomposes scale,
// which is the same division that makes bone 43 untouchable.
void ArmHide_SetActorHidden(void* handsActor, bool hidden);
