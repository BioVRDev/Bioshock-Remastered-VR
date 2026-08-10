# Research: roomscale, handedness, detached hands, and QOL

**Dated 2026-08-10**, immediately after M3-S1 passed. Desk research only — the
corpus, the shipped executables, and the game's own config. **Nothing here has
been tested in a headset.** Every claim is marked *measured*, *derived* or
*inferred*, and the inferred ones are the ones that will cost a cycle if taken
on trust.

> **Copyright note.** Same rule as `docs/ARCHITECTURE.md`: `research/uscript/` is
> 2K's content and gitignored. This file cites declarations, names and
> file provenance only.

---

## The headline: four asks, two enablers

The four things asked for do not each need their own machinery. They collapse
onto **two** enablers, and one of them is already the next card in flight.

| Ask | Enabler | Status |
|---|---|---|
| Roomscale | **A native call that moves the pawn with collision** | M3-S2 is exactly this problem |
| Left-handed mode | **Rigid bone-cluster transform** | Not built. One new mechanism. |
| Detached hands | Rigid bone-cluster transform | Same mechanism |
| Two-handed grip | Rigid bone-cluster transform | Same mechanism |

So the honest plan is **two projects, not four**. Build the cluster transform
once and handedness, detached hands and two-handed grip become configuration on
top of it. `docs/modules/hands.md` § *Future direction* already predicted this
mechanism, in almost these words, before any of these features were asked for.

---

# 1 — Roomscale

## What exists today is not roomscale

`EnableHeadPosition=1` applies your head's translation to `CameraLocation`. The
**collision capsule does not move** — `Camera/CameraHook.cpp` says so directly
(grep `The collision capsule does NOT move`). So today you can lean and step and
the *view* follows, but the body, the weapon trace origin and the pawn stay
where the stick left them. Walk far enough and you are looking through a wall.

Real roomscale needs the pawn to follow your feet, **with collision**. That is a
single missing primitive.

## Three candidate primitives, ranked

**1. `AActor::Move(Vector Delta)` — the right answer.**
*Measured:* `Engine/Classes/Actor.uc` declares `function bool Move(Vector Delta)`
and the symbol **`intAActorexecMove` is present in the shipped exe** — confirmed
by the same scan M3-S1 shipped. `MoveSmooth` is present too and is the variant
that slides along a wall rather than stopping dead, which is what you want when
someone walks diagonally into geometry.

It is a swept, collision-checked move that returns whether it succeeded, which is
exactly the contract roomscale needs: move the body if you can, and if you cannot,
leave it and let the head go through the wall alone.

*Cost:* it is an `exec` native, so it needs the `FFrame` machinery M3-S2 is
already going to build, plus a vector parameter instead of a name. **Roomscale is
downstream of M3-S2 and shares its risk.** That is the single most useful thing
in this document: it raises the value of M3-S2 well above "cutscene detection",
and it means M3-S2 should be built with a second caller in mind.

**2. `StartForcePlayerMove` — cheap, probably wrong shape.**
*Measured:* `ShockPlayerController.uc` declares `bIsForcingPlayerMove`,
`ForcePlayerMoveTargetLocation/Rotation` and four delta-velocity floats, and
`StartForcePlayerMove()` is **plain script that only writes those fields** — no
native call. So it is reachable by memory writes alone, at offsets computable by
declaration order (remember the `+0xC` `FMatrix` shift from M1-S1).

*Why it is probably wrong:* it is the game's scripted "walk the player to the
vending machine" mover. It forces rotation as well as location, and it is what
pushes `NullInput` — it is designed to take control away, not to track you 1:1.
Worth **one cheap probe** to see what it actually does, not worth designing on.

**3. Velocity injection — no native call, but not 1:1.**
Write the pawn's velocity so the engine's own physics step sweeps it. Collision
comes free and correct. But velocity is re-derived from input every tick, and
roomscale wants an exact displacement rather than a push. Expect it to feel like
walking nudges you rather than moves you. **Fallback only.**

**Not an option: writing `Location` (`+0x1D8`) directly.** That is
`SetLocation(bNoTest=true)` semantics — no collision at all. You would walk
through walls. The offset is known and it is tempting and it is wrong.

## The design, once the primitive exists

Each frame, on the game thread:

1. Take the head's horizontal offset from the room origin — already computed as
   `g_posRight`/`g_posFwd` against `g_posOrigin` in `CameraHook.cpp`.
2. Rotate into world by the same room yaw the camera write already uses. The
   maths is written and working; grep `roomYaw`.
3. `Move()` the pawn by that delta.
4. **Advance the room origin by the amount actually moved.** This is the step
   that is easy to miss and that makes it feel right: the pawn catches up to your
   head, the residual head offset shrinks to zero, and the movement does not
   double-count. If `Move()` reports blocked, do **not** advance the origin — the
   body stays, the head goes through, and the offset persists as a visual cue.

Vertical is deliberately excluded: physical crouching should not fight the
engine's own `EyeHeight`/`CrouchHeight`.

**Ship it default-off**, with a "comfort radius" cap so a tracking glitch cannot
fling the pawn across the level.

---

# 2, 3, 4 — the hand features share one mechanism

## The rig is fully mapped already

*Measured*, in `Hands/ArmHide.cpp`:

```
AHands           +0x3FC -> SkeletonInstance*
SkeletonInstance +0x48  -> hkQsTransform* render bone array
SkeletonInstance +0x4C  -> bone count, exactly 47
SkeletonInstance +0x88  -> evaluate-if-dirty byte
```

`hkQsTransform` is 48 bytes: `position[4]`, **`rotation[4]` (quaternion)**,
`scale[4]`. ArmHide writes position and scale today. **The rotation lane is right
there, untouched, and it is the whole of what these three features need.**

| Cluster | Bones | Wrist | Sleeve |
|---|---|---|---|
| Left | 6–21 | 6 | 3, 4, 5, 22, 23 |
| Right | 27–44 | 27 | 24, 25, 26, 45, 46 |

Bone **43** is the weapon attachment, inside the right cluster, and is the one
bone that must never be scaled.

**The array is in a common (model) space, not parent-relative.** *Inferred, but
strongly:* `CollapseBone(idx, wrist.position)` pins sleeve bones **at the wrist's
position**, passing one bone's coordinates to another — which is meaningless in a
parent-relative array. The `kFarBelow = {0,0,-5000}` push works the same way.
**Verify this explicitly before building on it**, because the whole design rests
on it: read three bones of a cluster at rest and check their positions differ by
plausible anatomical distances in a shared frame.

## The one mechanism to build

> **Rigid cluster transform.** Capture a reference pose for a bone cluster once.
> Each frame, choose a target transform for the cluster's wrist, and rewrite every
> bone in the cluster as `target × (reference_wrist⁻¹ × reference_bone)`. Clear the
> dirty byte. Apply it **late** — after the game's animation evaluation.

Two things make it work, and both are already-paid lessons in this codebase:

- **The dirty byte is not optional.** ArmHide's banner says the array is lazily
  rebuilt and the render path will re-evaluate straight over your write.
- **Late write.** S59/S60 measured the game tick erasing hand *roll* every single
  frame, which is why `CameraHook_LateHandsWrite` re-applies from Present. A
  cluster write from `CalcView` will be erased the same way. Reuse that seam.

## 2 — Left-handed mode

Three routes, cheapest first.

**A. Swap which controller drives the Hands actor.** Nearly free — the 6-DOF
hands write and `AimSource` currently assume the right controller
(`aimSource: 0 head, 1 right controller`; there is no left option). Add one, swap
`hideInactiveHand`'s cluster choice, and swap the trigger mapping. The weapon
then lives in your left hand. **The model still renders as a right hand holding
it** — most VR mods accept exactly this, and it delivers most of the value for a
fraction of the work.

**B. Mirror with `DrawScale3D` negative Y** (`+0x2B4`, *measured*). One write.
Almost certainly renders inside-out — a negative scale flips triangle winding and
backface culling eats it — and it mirrors the weapon too, so text and ejection
ports end up wrong. **Try it as a five-minute experiment, expect it to fail.**

**C. Swap the clusters properly**, with the mechanism above: right cluster (and
bone 43, and the weapon with it) driven to the left controller, left cluster to
the right. This is the correct answer and it composes with the other two features.

**One cheap experiment could shortcut all of this.** *Measured:*
`Holdable.AttachBone` is a **`config name`**, default `"R_Grip"`, overridden
per weapon (`Pistol`, `Wrench`, `TommyGun`, `Crossbow`, `Launcher`, `Chem`). The
`R_` prefix implies an `L_` counterpart may exist in the rig. Because it is
`config`, the mod's **existing** `EngineExec` `set` channel can change it with no
new code at all:

```
set Pistol AttachBone L_Grip
```

If the rig has that bone, the weapon moves to the left hand using the game's own
attachment system. *Caveat, and it is a real one:* `R_Grip` is the **only** such
name anywhere in the corpus, so there is no positive evidence `L_Grip` exists.
This is a lottery ticket that costs one console command — buy it before building
route C.

## 3 — Detached hands

"Hands not connected to arms" is mostly already true: `ArmHide` collapses the ten
sleeve bones, leaving 34 hand/finger bones. What is missing is that **both hands
still ride one actor transform**, so the free hand cannot go anywhere on its own.

With the cluster mechanism: keep driving the actor from the weapon hand as now,
then apply an independent cluster transform to the **off-hand** cluster targeted
at the other controller. That is one cluster, sixteen bones, one target transform
per frame.

Order matters — actor transform first (it moves everything), cluster transform
second and late (it overrides the off-hand back to where your real hand is).

## 4 — Two-handed grip

The same write with a different target. Instead of the off-hand cluster tracking
the off-controller, it tracks **a point on the weapon**: a per-slot grip offset in
the weapon's frame, tuned exactly the way `gripSlot[9][3]` and `rotSlot[9][3]`
already are, with the same live-numpad-and-save-to-INI workflow.

Engage it when the off-hand controller is physically near that point and the grip
is held; release on either condition failing. The game has **no concept of a
second hand on a weapon** — nothing in the corpus suggests one — so this is
purely a mod-side visual. That is fine; it is also why it cannot affect recoil or
accuracy without further work.

`HideInactiveHand2=0` (shotgun) and `HideInactiveHand5=0` (machine gun) already
exist precisely so two-handed weapons keep both hands. **Those two slots are the
natural first test cases.**

---

# 5 — QOL, ranked by what it costs

## Free today — no code, no new mechanism

**The game binds exec functions straight to gamepad buttons.** *Measured*, from
the live `User.ini`:

```
F8=QuickSave          XENON_RB=OpenWeaponMenu | onRelease CloseWeaponMenu | MODIFIER
F9=QuickLoad          H=HarvestGathererExec
                      L=SaveGathererExec
```

The binding grammar supports **`onHold`, `onRelease` and `MODIFIER` chords**, and
bindings are **per input context**. The mod already synthesizes XInput and already
writes the game's ini (`Game/GameIni.cpp` writes five keys into `Bioshock.ini`).
Extending that to bind actions is a known, safe pattern.

So these become reachable without touching the engine:

| Want | How |
|---|---|
| **Quick save / quick load in VR** | Bind `QuickSave`/`QuickLoad` to a controller chord. The single highest-value QOL item here. |
| Reliable pause | `ForcePause` — a real function, not the current X+Y chord heuristic |
| Weapon/plasmid radial | `OpenWeaponMenu` / `CloseWeaponMenu` / `CloseAbilityMenu` |
| Rescue / harvest by gesture | `SaveGathererExec` / `HarvestGathererExec` |
| Actions with no pad button left | `Use`, `Fire`, `AltFire`, `Jump`, `ThrowWeapon` are all `exec` |

*Caveat:* the mod's `EngineExec` runs commands through `UGameEngine::Exec`, and
whether that reaches `PlayerController` exec functions on this build is
**inferred, not measured**. It is one console command to find out.

**Head bob at the source.** `Pawn.Bob` is `var config float` — so
`set Pawn Bob 0` writes the class default and survives respawn, level change and
save reload, exactly like the reticle kill already does.

**Stature, done properly.** `Pawn.BaseEyeHeight` is `config`. The mod currently
raises the view with `CameraHeightOffset`, which is a camera-only lie — the
capsule does not move, and `Hands.UpdateLocation()` anchors the arms to
`PawnOwner.Location + EyeHeight`, which is why the shoulders needed the same
knob. Setting `BaseEyeHeight` moves the game's own notion and fixes both
honestly. *Inferred* that it takes effect on the live pawn; test it.

**Camera shake.** `PlayerController` declares `ShakeRollRate`, `ShakeRollTime`,
`ShakeOffset`, `ShakeOffsetRate`, `ShakeOffsetTime`. Zeroing them via `set` kills
explosion and damage shake. Partly covered already by `FreezeGameRotation`.

## Already fixed this session

**`ExcorcisingGatherer` was missing from `kContexts`.** Diffing the mod's table
against the game's own 30-entry `Contexts` list in `User.ini` found exactly one
gap — and it is the **Little Sister rescue**, the sequence M3-S3 exists to
detect. Added, with the classification marked as inferred.

> **The game misspells it: `Excorcising`, not `Exorcising`.** The script corpus
> spells the *animation* name correctly (`ExorcisingGathererAnimationName`), so
> anyone adding this from memory would spell it right and it would silently never
> match. The `User.ini` `Contexts=` list is the authority.

This was inert on arrival (the context scan has never locked) and stays inert
until M3-S3 works — which is precisely why it was worth fixing before then rather
than discovering it as a failed cycle.

## Worth a probe, cheap

- **`ToggleBehindView()`** is an `exec`. Third person during cutscenes is a
  legitimate answer to the whole cutscene-comfort problem, and it is one command.
  Whether the VR camera path survives it is unknown.
- `DisableContinuousRagdoll` / `EnableContinuousRagdoll` — performance.

---

# Proposed sequencing

Nothing here should displace the current arc; M3-S2 is the gate for the most
valuable item on the list anyway.

1. **Finish M3.** S2 unblocks roomscale as a side effect. Build its `FFrame`
   machinery so a second caller with a vector parameter is easy.
2. **The free QOL pass.** Quick save/load on a chord, `Pawn Bob`,
   `BaseEyeHeight`. No new mechanism, real user impact, and it can run in the
   gaps while headset cycles are scarce. Confirm `Exec` reaches `PlayerController`
   first — one command, gates the rest.
3. **`set Pistol AttachBone L_Grip`.** One command. If it works, left-handed mode
   collapses to almost nothing.
4. **Left-handed route A.** Add a left option to `AimSource` and swap the driving
   controller. Cheap, independent, ships value immediately.
5. **Verify the bone array's space.** One diagnostic read. Gates everything below.
6. **Build the rigid cluster transform**, late-write, default-off, on the
   **off-hand only**. Test on shotgun and machine gun, the two slots that already
   keep both hands.
7. **Detached hands**, then **two-handed grip**, then **left-handed route C** —
   all configuration on top of step 6.
8. **Roomscale**, after M3-S2 proves the call. Default-off, comfort-capped.

---

# What was deliberately not researched

- **Comparative VR-mod survey.** Skipped for the same reason
  `docs/ARCHITECTURE.md` skipped it: the corpus and the binary answered these
  questions specifically, and a survey would cost far more context to land
  somewhere less applicable to a 32-bit UE2.5 fork.
- **Bone names.** The mod indexes bones numerically and the rig's names live in
  Havok data, not the script corpus. Resolving `R_Grip` to bone 43 by name was
  not attempted; it would make the `L_Grip` question answerable offline and is
  the obvious next desk-research step if step 3 is ambiguous.
- **Recoil/accuracy effects of a two-handed grip.** Out of scope until the visual
  works.
