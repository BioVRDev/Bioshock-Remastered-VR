// BioshockVR/Game/FireSeam.h
#pragma once

// MAKING THE SHOT LEAVE THE BARREL.
//
// The engine starts every shot at the PAWN'S OWN Location (pawn+0x1D8) -- the
// player's body, not the gun they are holding at arm's length in VR. That is a
// constant WORLD DISPLACEMENT between the crosshair's ray and the bullet's ray,
// which is why an angular correction tuned at one distance is wrong at every
// other distance, and why a visible projectile (crossbow bolt, grenade,
// chemical stream) is seen leaving the player's chest.
//
// CursorOffsetN cannot fix this and that is not a tuning failure: it rotates the
// aim RAY and cannot move the shot's ORIGIN. AWeapon::GetPerfectFireStart hands
// back the origin AND the rotation as out-parameters, so both can be
// substituted without caring what the trace downstream reads.
//
// THIS IS THE ONLY PLACE IN THE MOD THAT DETOURS A FUNCTION ON THE FIRING PATH.
// Everything here is shaped by that:
//
//   - Nothing is hooked at all unless FireSeam is non-zero. Default 0.
//   - The target comes from the WEAPON'S VTABLE SLOT, never a hardcoded RVA, so
//     it survives a storefront change. (The rva it resolves to on Steam is
//     0x226840, which is exactly the figure an independently developed mod
//     recorded for the same function -- docs/ENGINE-MAP.md.)
//   - The install REFUSES unless the target's own `ret imm` matches the argument
//     count we are about to call it with. Getting that wrong does not crash: in
//     a Release build it corrupts the stack silently.
//   - The detour calls the original FIRST. The engine's own numbers are what we
//     log and what we fall back to, so a refusal is exactly today's behaviour.
//   - AI weapons inherit AWeapon and hit this same seam every time a splicer
//     shoots. [weapon+WeaponOwnerOffset] must equal the pawn we already located
//     or we touch nothing.
//
// Technique and measurements adapted from mohamad-balouza/bioshock-vr (MIT),
// which live-confirmed the signature and the substitution; the code is ours.
// docs/ENGINE-MAP.md carries the derivation.

// Try to hook, once, from the held weapon's vtable. GAME THREAD ONLY -- called
// from HandsProbe's ResolveWeaponSlot, which runs inside CalcView. Safe to call
// every frame: it latches after the first success and after a hard refusal.
//
// Also keeps the DISTINCT-TARGET CENSUS. Each weapon class has its own vtable,
// so a subclass that overrides the implementation would slip past a single hook
// -- and "some of them werent even firing from the gun barrel" is exactly what
// that would look like. Every distinct slot value is recorded and logged.
void FireSeam_TryInstall(const void* heldWeapon);

// ---- the snapshot, and why it is one -------------------------------------
// The detour runs on the game thread inside an engine call. It must not walk
// the hands actor, the bone array or the XR runtime while it is there. So
// CalcView publishes what the seam needs and the seam reads nothing else.
//
// FRESHNESS IS THE GAMEPLAY GATE, and it is free. DriveHands already returns
// early for a cutscene, a UI panel, a scripted window and sixDofHands=0, so a
// stale snapshot means precisely "the hands are not ours right now" -- and the
// seam then substitutes nothing. There is no second predicate to keep in sync.
//
// `originWorld` is the muzzle in game world units (cm). `aimRot` is the
// controller aim as an FRotator, published separately because it is decided
// earlier in CalcView than the hand position is.
void FireSeam_PublishOrigin(const double originWorld[3]);
void FireSeam_PublishAim(int pitch, int yaw, int roll);

// Drop the snapshot and the census on a pawn/world transition. Deliberately
// does NOT unhook: the function address belongs to the module, not the world.
void FireSeam_Reset();

// One line for the shutdown/periodic summary: how many calls, how many
// substitutions, how many refused and why. Read by the log so a run can be
// judged without the tester describing anything.
void FireSeam_LogCensus();
