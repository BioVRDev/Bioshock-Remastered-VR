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

## Aim and movement are the same field

`Controller.Rotation` (`+0x1E4`) drives the view, the weapon trace **and** the
walk direction. Writing the controller's direction into it means motion aim
necessarily drags locomotion along; writing the head's puts everything on the
head, which is coherent but is not motion aim.

**`MovementMode` is the switch, and `AimSource`/`HeadRelativeMove` are legacy**
seeds read only when it is absent. Mode 0 head, 1 combined (the default and the
old behaviour), 2 controller. The coupling above is exactly why one key owns
both halves now: mode 0 was *unreachable* before, because the aim carried the
head while `HeadRelativeMove` still rotated the movement stick by the head
offset — **the head applied twice**, measured as "turning 90 degrees left almost
moves you backwards". 90 twice is 180.

Two predicates keep that honest, and must not be merged: `AimUsesHeadNow()`
drives the aim field *and* the stick gate (they must agree or the duplicate comes
back); `ModeUsesHead()` drives the view composition and ignores the empty-handed
case, so picking a weapon up never recomposes the view.

`AimSource=2` ("write nothing, let the game keep its heading") **cannot work** —
with `ModYaw` on, the game has no input from which to update that heading, so it
freezes permanently. See `docs/INVARIANTS.md`.

Decoupling requires knowing what the firing trace reads. If it comes off the
weapon socket rather than `Controller.Rotation`, the seam exists; if not, it
doesn't. That is unmeasured and is the first thing to settle.

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
