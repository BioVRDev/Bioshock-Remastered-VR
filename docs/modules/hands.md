# Hands, weapons, and arms

`Hands/HandsProbe.cpp` (1707), `Hands/ArmHide.cpp` (504).

## Finding the Hands actor

The chain is `ShockPlayerController → Pawn → Hands`. There is no reflection
system, so both hops are identified **positionally, with numeric tests**:

- **Pawn** — an object whose `Location` (`+0x1D8`) is within a couple of metres
  of the camera. Camera coordinates are large and distinctive (e.g.
  `-6280, 4497, 2627`), so a three-axis match is effectively unique.
- **Hands** — `Hands.UpdateLocation()` runs every frame doing
  `SetLocation(~camera)` and `SetRotation(view rotation)`. So the Hands object
  matches the camera position **and** the view rotator simultaneously. That pair
  constraint is far stronger than either alone.

The offsets were a *prediction*, not a hunt: script says `GetViewRotation()`
returns `Rotation + ViewRotationOffset + CameraAnimOffset`, and the mod already
writes the aim field at `+0x1E4` — so `AActor::Rotation == +0x1E4`, and UE2 puts
`Location` (12 bytes) immediately before it, giving `+0x1D8`. Confirmed by
writing and watching: `Hands` at `pawn+0x724`, rotation error 0.0°, 0.0 cm from
the camera at rest.

Three stages: A (pawn), B, C. Everything is read-only except the deliberate nudge
test. Fast probe lock removed multi-second delays after level loads — keep the
arming/retry values and the lifecycle reset.

## Rotation must compose as quaternions

Adding trim angles to Euler angles produces gimbal and shear behaviour, badly so
near vertical wrist orientations. The stable design composes a configured offset
quaternion with the controller/aim quaternion and converts to an engine rotator
**once, at the end**. Diagnosed independently more than once; do not "simplify"
it back to Euler arithmetic.

The rotation is re-applied from the **render thread** after the game tick
(`CameraHook_LateHandsWrite`), which is what made wrist roll hold.

## Per-slot calibration

Grip position, rotation, and cursor/shot offsets are per weapon slot, tunable
live with the numpad, and saved back to the INI. `CursorOffset` should be the
single source of truth for both the crosshair and the shot direction.

Slot map and layout offsets are in `docs/ENGINE-MAP.md`. **All plasmids share
slot 8**, so Electrobolt and Telekinesis overwrite each other's calibration —
fixing that needs the equipped plasmid identified and the slot tables widened.

Historical calibrated values (2750×2850, world FOV 100, foreground ~117.5) — a
reference point, not current defaults:

| Slot | Item | Grip fwd/right/up | Rotation | Cursor |
|---:|---|---|---|---|
| 0 | Wrench | `58, 18.3, -16.7` | `-12, -16, -18` | `-17, 2, -11` |
| 1 | Pistol | `44, 16.7, -15.4` | `0, -8, 0` | `2, -9, -4` |
| 2 | Shotgun | `16, 11.8, -11.8` | `0, -8, 0` | `0, -6, 0` |
| 5 | MachineGun | `52, 17, -14.7` | `3, 0, -1` | `5, -2, -1` |
| 8 | Plasmid | `48, -11.4, -13.8` | `-23, 24, 0` | `-12, 34, 0` |

Slots 3/4/6/7 inherit a generic offset and were never tuned.

## Scale

Real actor controls replaced the old viewport `WeaponScale` projection hack:
`DrawScale` at `+0x2AC`, `DrawScale3D` at `+0x2B0..+0x2B8`, weapon actor via
`hands+0x45C`. At least one model has a sub-mesh child (historically `gun+0x528`)
needing separate scaling.

## Arm hiding

The arms are animated by the game while the hands follow the controllers, so they
stretch from a shoulder that isn't where your shoulder is to a hand that is.
Suppressing the whole arm draw takes the weapon with it, so `ArmHide` works at
the **skeleton**: collapse ten sleeve bones to zero scale and pin them at the
wrist, leaving the 34 hand/finger bones and the weapon attachment alone.

**Fail-closed by construction**, and it must stay that way: both the `AHands` and
`SkeletonInstance` vtables are verified against expected values, the
`SkeletonInstance` is checked to actually belong to this actor, and the rig must
report **exactly 47 bones**. Any mismatch writes nothing — a wrong offset here
does not produce a glitch, it produces a bone matrix full of garbage.

Both vtable RVAs are Steam/Epic-specific and INI-overridable
(`ArmHideHandsVt`, `ArmHideSkelVt`); the shift between stores is not constant, so
no single value serves both.

> ### ⚠ Bone 43 is untouchable
> Telekinesis release walks the attachment path through it. Moving or scaling it
> crashes the game. Per-weapon inactive-hand overrides exist so two-handed
> weapons keep both hands: `HideInactiveHand2=0` (shotgun),
> `HideInactiveHand5=0` (machine gun).

`ArmHide_Reset()` deliberately does **not** restore first — by the time a reset is
reported the old actor may already be destroyed and its address reused.

## Animation

`IdleAnimMode=1` suppresses the wrench slap while keeping a normal fidget. More
aggressive parking (`IdleAnimMode=2` and per-animation suppression) was
structurally fragile and abandoned. `AdditiveHandBobAnim` was already `None` and
`WeaponBobDamping` (observed 0.5) had no observable effect — neither is a sway fix.

## Future direction

Top-level actor transforms are not the final authority for every rendered weapon
piece; attachments follow skeletal matrices. The stronger architecture is to
identify the active hand bone cluster, preserve a reference pose, and apply a
rigid cluster transform around a reference anchor after skeleton evaluation.
Larger and riskier than the current slot system.

> **This is now the critical path for three requested features.**
> `docs/proposals/vr-features-research.md` (2026-08-10) found that left-handed
> mode, detached hands and two-handed grip are all the *same* mechanism — this
> one — and that the `hkQsTransform` rotation lane needed to build it is already
> sitting untouched in the bone array `ArmHide` writes to.
>
> Two things it must inherit rather than rediscover: the **dirty byte is not
> optional**, and the write must be **late** (the S59/S60 measurement — the game
> tick erases hand roll every frame, which is why `CameraHook_LateHandsWrite`
> re-applies from Present). One thing it must verify first: that the render bone
> array is in a **common model space** and not parent-relative. That is inferred
> from `CollapseBone` pinning sleeve bones at the wrist's position, which is
> strong but is not a measurement.

## A split was considered and rejected

See `.planning/DECISIONS.md`. `g_hands`, `g_pawn` and `g_gun` are used across the
whole file by both the discovery stages and the per-weapon code.

Sections, by banner text: `state` · `live tuning` · `stage A` · `stage B` ·
`stage C` · `HAND MODE PROBE` · `ACTIVE HAND MODE` · `WHICH WEAPON IS EQUIPPED` ·
`PER-WEAPON GRIP OFFSET` · `IDLE HANDS ANIMATION` · `QUEST ARROW HUNT`.
