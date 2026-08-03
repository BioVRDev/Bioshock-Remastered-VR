// BioshockVR/ArmHide.h
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
