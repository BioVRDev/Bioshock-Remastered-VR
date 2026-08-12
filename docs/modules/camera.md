# Camera, aim, and turning

`Camera/CameraHook.cpp` (2583) — the largest file and the engine seam the whole mod
rests on.

## Finding the function

`APlayerController::eventPlayerCalcView` is located by a **six-stage FName/string
chain**, never a hardcoded RVA:

1. Find the UTF-16 `PlayerCalcView` in module memory.
2. Find an executable `PUSH imm32` referencing it.
3. Follow the init sequence to capture the FName global.
4. Find references to that global, excluding the init loop.
5. Walk backward to an MSVC function prologue.
6. Install only if **every** stage validates.

Run after the executable has unpacked (first Present), never from `DllMain`. A
failed stage installs nothing. This scan found the function on Steam, Epic *and*
GOG, which is why it is the model for every other lookup in the project.

The function has several call sites; the **leader** is the one that keeps running
while stationary (`module+0x4CCF62` on Steam). Identify it at runtime. Writing
head rotation into a movement or aim consumer site silently couples head motion
to locomotion. Full table in `docs/ENGINE-MAP.md`.

## What it owns

- **The eye-tag FIFO** (banner `the eye FIFO`, API at `the FIFO API`). Tags each view with the eye it
  applied and pushes; `Present` pops in order. Because the tag travels with the
  frame, pipeline depth is irrelevant and a one-frame slip cannot accumulate.
  `CameraHook_EyeQueueStats` reports depth — `min == max` means lockstep.
- **The latched-pose channel** (banner `latched pose`). Publishes the pose the camera was
  *actually rendered from* — the quat latched at eye-0 time plus the applied,
  clamped head-centre position. The projection layer must carry this, not the
  freshest pose at submit time. Removing it reintroduces flicker.
- **Motion aim**, head aim, applied-shot direction, snap turn, `ModYaw`, and
  the pitch servo (must stay off). Banners: `motion aim state`,
  `HIDDEN PITCH SERVO`, `APPLIED SHOT DIRECTION`, `HEAD-AIM`.
- **Delta clamp** (banner `ONE WORLD ADVANCE PER EYE PAIR`) — one world advance
  per eye pair, so both eyes show one instant.
- **6-DOF hand writes** and the late rotation write (banners `6-DOF HANDS`,
  `LATE ROTATION WRITE`), re-applied from the render thread after the game tick
  has had its say.

## Frame ordering — the rule that keeps being rediscovered

> Read and apply the game's turn delta **before** composing the final view, then
> use that same advanced base for the view, the aim field and the hands, in that
> frame.

Advancing the base afterwards leaves the gun one frame of yaw ahead of the view.
The error is proportional to turn rate and flips sign with direction, so the
weapon appears to swell turning one way and shrink the other. This is distinct
from the foreground-FOV projection bug that looked similar; changing FOV altered
the magnitude but never removed it.

The same rule bit `ModYaw` when it was first placed near the snap-turn block:
snap turn survives there because it is a single impulse, but a continuous turn
reproduces the error every frame. `ModYaw` must sit immediately before
`finalRot` is composed.

## Rotation control — current state

Two settings, both **default 0 and currently inert**:

- **`ModYaw`** — the mod rotates `g_aimBase.yaw` itself from right-stick X,
  and `InputHook` zeroes `sThumbRX` so the game never sees the axis. This gives
  turning *authority*: it works even where the game ignores the pad.
- **`FreezeGameRotation`** — discards the game's `dP`/`dY`/`dR` deltas.
  Screenshake, recoil kick, camera-anim breathing and scripted slews all arrive
  through those three lines and nowhere else. **Requires `ModYaw`**, because the
  player's own stick turn also arrives as `dY`.

**Why they are off:** zeroing `sThumbRX` freezes `Controller.Rotation`, and
forced-move sequences steer by it. The opening bathysphere walks the player into
the back wall and the projector video never plays. Freezing yaw *only* was tried
and reads as doing nothing, because `HeadAimMode=2` already discards base pitch —
yaw is the axis you actually feel.

This is unresolved, not abandoned. Keep both compiled and default-off.

## Aim and movement are the same field — for aim only

`Controller.Rotation` (`+0x1E4`) drives the view, the weapon trace **and** the
walk direction. For **aim** that coupling is structural and unbroken: no
arrangement of that one field separates the view from the trace.

**Locomotion is separable, and always was.** The game applies the stick angle on
top of the field, so rotating the *stick* redirects walking without touching the
field at all:

```
walk = aimFieldYaw + stickAngle + R
```

`HeadRelativeMove` had been doing exactly this since it shipped. The four
`MovementMode` values are just values of `R`, computed from two already-exported
terms — `H` (head yaw, `CameraHook_GetHeadYawOffset`) and `O` (the controller's
clamped offset from the head, `CameraHook_GetAimOffset`), where the controller's
absolute yaw is `C = H + O`.

| Mode | Walks where | `R` |
|---:|---|---|
| 0 neither | right stick only | `-(H+O)` |
| 1 controller | you point | `0` |
| 2 head | you look | `-O` |
| 3 both | you point, plus where you look | `H` |

### `R` is measured at the write site, never predicted

The table above is how the modes are *described*. It is **not** how `R` is
computed, and the difference cost a cycle.

`aimFieldYaw = base + H + O` is an approximation: the field is written by
`ComposeHeadLocal()`, a **basis multiplication**, so in general its yaw depends
on the pitch too.

> **It is exact in the shipping config, though**, and assuming otherwise cost a
> cycle. `HeadAimMode=2` drops the base pitch, making `M = Rz(yaw_base)` — a pure
> yaw — so `want.yaw == base.yaw + aimY` exactly. The measured and predicted
> forms are numerically identical there. Measuring is still the right form (it is
> correct for modes 0 and 1) but it fixed no reported bug.

`PublishWalkRotation()` computes `R` in `CalcView`, from the rotators actually
written, and publishes one seq-locked float that `InputHook` simply applies:

| Mode | desired walk heading | `R` |
|---|---|---|
| 0 neither | `base` | `base.yaw - aimRot.yaw` |
| 1 controller | the field as written | `0` |
| 2 head | the **view** | `viewRot.yaw - aimRot.yaw` |
| 3 both | field + the head's part | `viewRot.yaw - base.yaw` |

Exact at any pitch, by construction. Same discipline as `PublishShotDir`, which
was written for the same reason — one calculation, no second algebra to drift.

**When the game owns the field** (a scripted release, head aim off, UI up, hook
starved) `aimRot` is not written: the subtractive rows collapse to `0` and modes
2 and 3 keep `viewRot.yaw - base.yaw`, which is plain head-relative movement and
means the same thing whoever wrote the field. `CameraHook_OwnsAimField()` still
names that condition. Subtracting terms the field never contained is what steered
the plasmid balcony scene off its path.

### Scripted scenes rotate you on two different fields — and sometimes both

`ScriptedCameraFollow` (on by default) advances `g_aimBase` by the game's own
`cleanRot` yaw delta during a scripted window. Different scenes use different
fields:

| Scene | aim field | the game's own camera |
|---|---:|---:|
| Little Sister rescue | rotates | — |
| **Balcony fall**, across the window | **0.00 deg/s** | **up to 125 deg/s** |
| **Balcony fall**, the opening snap | **41.03 deg/s** | **41.03 deg/s** |

Measured 2026-08-11 with every gate open, so nothing was being discarded — the
fall's rotation had simply never been read. It is why that scene never turned the
player, for the whole life of the mod.

> **⚠ THE LAST ROW IS THE CORRECTION, and it cost three runs.** This table
> originally had only the middle row, from a deg/s average across the whole
> 67-second window — and on that evidence the mod followed **both** fields. But
> the opening snap moves both *identically*, so it landed twice and the view
> finished a whole snap past the authored heading. The error equalled the snap in
> three consecutive runs, sign included: **+41°, −4°, −77°**, against the tester's
> *"45 right"*, *"almost perfect"*, *"90 left"*.
>
> The camera is downstream of the aim field, so it already carries anything the
> scene did to `Controller.Rotation`. **It is now the single source of scripted
> yaw** (grep `ONE SOURCE FOR SCRIPTED YAW`); the aim-field path still owns pitch
> and roll, which the follow never handled. `ScriptedCameraFollow=0` reverts to
> aim-field-only.

### The window itself — one scene, one window

`ScriptedAimReleased()` decides whether the mod writes the aim field. It reads
`GameState_ScriptedWindow()`, which is the **held** union of the two signals a
scene raises in sequence: the forced move that walks you into place, then the
scripted animation. Those normally overlap by ~90 ms. When the order reverses
their union gaps, and a gap of **one frame** is enough to release the aim,
re-arm `g_aimBase` from whatever the field says at that instant, and write it
back — measured on the Little Sister crawl, which then ran 58 seconds with the
aim field 18.6° off the pawn. Full measurement in `docs/INVARIANTS.md`.

The hold lives in `GameState` rather than here **on purpose**: two consumers read
that union with deliberately different policies on top. This one narrows it with
`ScriptedButInControl()` so a walk-through scene keeps your aim; `InputHook`'s
turn-axis release stays wider, and its own note says not to weld the two together
without a headset. Holding the shared signal fixes both without touching either
policy.

Arming is treated as a write, not a read (grep `ARMING IS A WRITE`): whatever
lands in `g_aimBase` becomes the reference for the aim field *and* for the two
subtractive movement modes, so an exactly-zero reading is refused rather than
adopted. Bounded, so a genuine zero heading cannot hang the aim.

### The balcony's authored numbers

Useful because they turn *"landed wrong"* into arithmetic, and both were stable
across every correct run on two different builds:

| | value |
|---|---|
| authored heading | **−90.0°**, held for twelve seconds after the snap |
| landing position | **−417.1, −3144.8, −31.8** |

A run that ends anywhere else is a regression, and the log says so without anyone
having to judge it by eye.

### Turn response

The game's own turn rate is nearly vertical at the top of the stick — measured
`0.98` → ~105 deg/s, `0.99` → ~140, `1.00` → ~200 — so the same push landing 2%
differently doubled the speed. **Frame-rate dependence was the first hypothesis
and is falsified**: 40 samples across 142–239 CalcView calls/s show no
correlation.

`TurnAxisMax` (0.95) and `TurnAxisExp` (1.0) remap the axis so the cliff is
unreachable, trading top speed for repeatability. Both are ini-tunable because
that trade is a matter of taste. Snap turn and `ModYaw` bypass this path.

### Who aims is now a separate switch

`HeadAimAlways` decides what the aim field carries; `MovementMode` decides only
who steers. They were welded together until 2026-08-11, which made head aim
imply head steering and made three of the four modes inexpressible.

Two predicates keep it honest and **must not be merged**: `AimUsesHeadNow()`
drives the aim field and is *dynamic* (it flips with empty hands); `ModeUsesHead()`
drives view composition and is the *static config flag alone*, so picking a
weapon up never recomposes the view.

`AimSource`/`HeadRelativeMove` are legacy seeds, read only when `MovementMode` is
absent. `AimSource=2` ("write nothing, let the game keep its heading") **cannot
work** — with `ModYaw` on the game has no input from which to update that
heading, so it freezes permanently. See `docs/INVARIANTS.md`.

### Aim decoupling still needs one measurement

Whether the firing trace reads `Controller.Rotation` or the weapon socket. If the
socket, the seam exists. `AWeapon::GetPerfectFireStart` is now located at vtable
slot `+0x304` (`docs/ENGINE-MAP.md`) and **answers it by asking**.

### A known artifact, diagnosed and deferred

Walking speed varies with the rotation angle. `ToAxis()` clamps each stick
component to ±1 independently, so the pair is a **square** while `R` is a
rotation — a rotated full diagonal produces a component near 1.41 which is then
clipped. The fix, when wanted, is to scale by `1/max(|x|,|y|)` when that exceeds
1: direction preserved, speed capped uniformly. **It predates the four modes** and
applies equally to the old `HeadRelativeMove`.

## `AimOverride` — one site where a feature can take the aim

Empty today, returning false, and introduced *before* its callers on purpose.
Three planned features want the same substitution — the gun barrel (bones 43→44),
the wrench tip, and the two-handed grip. Precedence when they arrive: barrel/tip
first (weapon-slot scoped), two-handing second, none while the head owns the aim.

This codebase has already paid once for two features reaching the same place
independently; a resolver built first means the third adds a clause, not a site.

## Head aim modes

`HeadAimMode`: `0` additive · `1` local compose · `2` pitch-decoupled.

Mode 2 sets `m.pitch = 0` in `ComposeHeadLocal`, so base pitch never reaches the
view. **It also starves scripted pitch gates** — the plasmid injection scene waits
for the view to pitch down at the syringe and hangs forever under mode 2.
`HeadAimMode=1` clears it. This is a real, reproducible interaction.

Right-stick Y is dropped by default (`ControllerPitch=0`) because under mode 2 any
injected pitch is erased ~8 ms later and reads as a fight.

## A split was considered and rejected

See `.planning/DECISIONS.md` for the measurement. `g_aimBase`, `g_lpQuat` and
`g_eyeQ` are each touched across the whole file because one function --
`hkCalcView` -- drives all of it, so cutting by section would turn ~30 file-level
statics into shared cross-file globals and split seqlock writes that must stay
together.

The one genuine seam is `EnumReadableRegions` + `FindCalcView` (~230 lines),
which touch only `g_modBase`/`g_modSize`. Worth extracting as part of the
deferred layered architecture, not on its own.
