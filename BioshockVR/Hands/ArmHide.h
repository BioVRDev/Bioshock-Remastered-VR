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
// WHICH BONE IT SAMPLES IS NOT FIXED, and must not be. It is the wrist of the
// cluster the free-hand drive is NOT writing, because a driven cluster reports
// our own rigid transform -- bit-for-bit identical every frame while the
// controller is still. Measured 2026-08-11 as 189 and 223 consecutive
// `raw 0.0000` samples with the arms hidden for a whole scene. See the banner in
// ArmHide.cpp. Call ArmHide_MotionBone() to log which one is live.
//
// Returns false until the skeleton is locked. `outRaw` is the un-smoothed
// per-call value, logged for threshold calibration.
//
// ALSO RETURNS FALSE WHEN BOTH CLUSTERS ARE DRIVEN, which C1 made possible for
// the first time. There is then no engine-owned wrist and any number this could
// return would be a guaranteed zero. The caller must treat that as "cannot
// answer" and fail in the direction that SHOWS the arms -- the graveyard entry
// is arms hidden for a whole scene, and there is no matching entry for arms
// shown for one frame.
bool ArmHide_HandMotion(float* outSmoothed, float* outRaw);

// Which bone the call above is currently measuring, or -1 when both clusters
// are ours and no honest bone exists. For the log line, and for the caller's
// blind-guard test.
int ArmHide_MotionBone();

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
// THAT PREDICTION CAME TRUE THROUGH THE OTHER DOOR. Hiding never did it, but the
// M6-S1 cluster DRIVE writes bones and clears the same dirty byte, and the
// sampled bone sat inside the driven cluster during every plasmid scene. The
// hazard is the write, not the reason for it -- which is why the sampled bone is
// now chosen against the driven cluster rather than being a constant.
//
// Never writes exact zero -- the attachment path inverse-decomposes scale,
// which is the same division that makes bone 43 untouchable.
void ArmHide_SetActorHidden(void* handsActor, bool hidden);

// ---- M6-S1: WHAT SPACE IS THE BONE ARRAY IN? ----------------------------
// READ ONLY. Dumps seven bones a few times after the skeleton locks and then
// stops. Behind HandRigProbe, default off.
//
// M6's cluster transform assumes the array is in a common MODEL space rather
// than parent-relative. That is inferred from CollapseBone pinning one bone at
// ANOTHER bone's position, never measured -- and if it is wrong the transform
// maths is wrong too. The dump also says whether the array stays LIVE while the
// sleeves are hidden, because this file's own dirty-byte clear can freeze it.
void ArmHide_RigProbe(void* handsActor);

// ---- M6-S1: THE TRACKED FREE HAND ---------------------------------------
// A rigid transform on one hand's bones: the cluster keeps its own shape while
// its wrist goes wherever the caller asks. This is the ONE mechanism M6 is built
// on -- left-handed mode and a two-handed grip are the same write with a
// different target.
//
// `hand` is which cluster to drive: HAND_LEFT is bones 6-21, HAND_RIGHT 27-44.
// With a weapon the free hand is the left one; with a plasmid the left hand
// holds the plasmid and the free hand is the right.
//
// targetPos is MODEL space, the frame M6-S1 measured. targetQuat may be null,
// which slides the cluster bodily and leaves every bone at its authored angle.
//
// BONE 43 IS IN THE RIGHT CLUSTER and takes the cluster's POSITION ONLY -- see
// the banner in the .cpp. Nothing here writes scale for any bone.
bool ArmHide_DriveFreeHand(void* handsActor, int hand,
    const float targetPos[3], const float targetQuat[4]);

// Mode 3. Ignores the controller and slides the cluster along one model lane at
// a time so the axis map can be settled by observation rather than argument.
bool ArmHide_SweepFreeHand(void* handsActor, int hand);

// Restores the reference pose and re-flags the array. MUST be called whenever
// the drive stands down -- a scripted sequence needs the array evaluating again
// before ArmHide_HandMotion is trusted. Also called on a hand switch, so the
// cluster we walk away from is not left holding a pose we forced on it.
void ArmHide_ReleaseFreeHand();

// ---- C1: THE WEAPON HAND, RIGID -----------------------------------------
// The same rigid transform, pointed at the cluster that IS holding the weapon,
// with the target set to that cluster's own captured wrist. qDelta comes out
// identity, so the authored pose is replayed exactly where it already was: the
// hand stops breathing and the gun stops swaying, and nothing else moves.
//
// This is what unblocks aim-down-sight. A gun that sways cannot have a
// crosshair calibrated against it.
//
// WHY IT IS A SEPARATE ROLE RATHER THAN A SECOND CALL TO DriveFreeHand: both
// clusters can now be driven at once, so the reference buffers, the driven flags
// and the release paths are per hand, and each role tracks which hand it owns.
// The free hand wins any tie -- M6-S1 is signed off and must not change.
//
// THE CALLER OWES ONE THING: release this BEFORE ArmHide_HandMotion is consumed
// on a scripted frame. With both clusters driven there is no engine-owned wrist
// left and the motion gate has nothing honest to measure. See MotionBone().
// `poseKey` is HandsProbe_ActiveHeld() -- the identity of whatever is in your
// hand. When it changes the authored pose changed with it, so the cluster is
// released, the engine is allowed to animate the equip, and the freeze
// re-captures once WeaponSwitchSettleMs has passed. Without this the pose
// captured for the first weapon is replayed onto every later one, which is
// exactly what Build U did through seven switches.
bool ArmHide_FreezeWeaponHand(void* handsActor, int hand, const void* poseKey);
void ArmHide_ReleaseWeaponHand();

// ---- M6-S2: THE TWO-HANDED GRAB POINT -----------------------------------
// The cluster's reference wrist in MODEL space -- where the game's own animation
// puts that hand. On the shotgun and the Tommy gun that is the fore-end grip,
// which is the entire basis of the two-handed feature: the grab point is not a
// tuned constant, it is the pose the game already draws.
//
// Read-only, and safe whether the cluster is frozen or free.
bool ArmHide_FreeHandAnchor(void* handsActor, int hand, float outModel[3]);
