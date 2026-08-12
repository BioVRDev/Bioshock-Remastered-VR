// BioshockVR/Hands/HandsProbe.h
#pragma once

// FINDING THE `Hands` ACTOR -- the prerequisite for 6-DOF weapon holding, and
// for decoupling weapon distance from weapon size.
//
// The chain we need:
//
//     ShockPlayerController*      <- we already have this, every CalcView
//         -> Pawn                 (AController::Pawn)
//             -> Hands            (ShockPlayer::Hands)
//
// THE FREE ANCHOR: we already write the aim field at +0x1E4, and script says
// GetViewRotation() returns `Rotation + ViewRotationOffset + CameraAnimOffset`
// where Rotation is the ACTOR member. So AActor::Rotation == +0x1E4 for every
// actor in this game, and UE2 puts Location (12 bytes) immediately before
// Rotation -- so AActor::Location should be +0x1D8. That is a prediction we
// verify in one comparison instead of a field we have to hunt.
//
// With those two offsets, both pointer hops get a NUMERIC test:
//
//   Pawn   -- an object whose Location is within a couple of metres of the
//             camera. Camera coordinates are large and distinctive
//             (e.g. -6280, 4497, 2627), so a three-axis match is effectively
//             unique; nothing lands there by chance.
//
//   Hands  -- Hands.UpdateLocation() runs every frame and does
//             SetLocation(~camera) and SetRotation(view rotation). So the Hands
//             object matches the camera position AND the view rotator at the
//             same time. That pair constraint is much stronger than either
//             alone.
//
// Everything here is READ-ONLY except the deliberate nudge test below.

void HandsProbe_Observe(void* playerController,
    const float camLoc[3], const int camRot[3]);

// Non-zero once the Hands actor has been positively identified.
void* HandsProbe_Get();

// The Pawn (ShockPlayer). Found in STAGE A, and useful well beyond this file:
// LastPlayerInputContext lives on ShockPlayer, not on the controller, which is
// why GameState's 16KB scan of the controller came back with nothing.
void* HandsProbe_GetPawn();

// Active weapon slot, or -1 before the first switch. DrawHook reads this to
// decide whether the arms should be suppressed for the weapon in hand.
int HandsProbe_WeaponSlot();

// ---- 6-DOF (S54) ---------------------------------------------------------
// MEASURED, all four confirmed by writing and watching:
//   Hands object   pawn+0x724   (tracks the view rotator, err 0.0 deg)
//   Rotation       +0x1E4       (zero bias against the view)
//   Location       +0x1D8       (0.0 cm from the camera at rest)
// Location and Rotation are adjacent, which is the standard UE2 AActor layout.
//
// Returns false until the probe has locked. CameraHook drives the writes: it
// already owns HeadQuatToDeg, ComposeHeadLocal, g_aimBase and the room-yaw
// rotation, and doing it there keeps the hands in the same frame as the view.
bool HandsProbe_GetTargets(void** obj, unsigned* locOff, unsigned* rotOff);

// TRUE when a plasmid is active rather than a weapon. Drives which controller
// aims, and which one the hands actor is posed from.
bool HandsProbe_AbilityMode();

// TRUE when you are holding ANYTHING -- a weapon or a plasmid. False only when
// both hands are genuinely empty, which is the opening of the game and wherever
// the game takes your weapon off you.
//
// Costs nothing: the same two pointers that decide AbilityMode already say it.
// Read from the render thread by the crosshair, and it defaults TRUE so a probe
// that never locks leaves the crosshair exactly as it was.
bool HandsProbe_Armed();

// WHICH object is in your hand right now -- the weapon (CurrentHoldable,
// hands+0x45C) or the plasmid (CurrentAbility, +0x454), weapon first when both
// are set. Both pointers are already read every frame, so this costs nothing.
//
// AN IDENTITY, NEVER A TARGET. It is compared for CHANGE and never dereferenced,
// which is what makes it safe to hand out: a freed and reused address is still a
// correct answer to "is this a different thing from last frame".
//
// Preferred over HandsProbe_WeaponSlot() as a "the authored pose changed" signal,
// because the slot does not move when one plasmid is swapped for another -- and
// in ability mode the driven cluster is posed around the plasmid.
const void* HandsProbe_ActiveHeld();

void HandsProbe_Reset();