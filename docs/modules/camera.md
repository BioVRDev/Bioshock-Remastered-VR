# Camera, aim, and turning

`CameraHook.cpp` (2615) — the largest file and the engine seam the whole mod
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

- **The eye-tag FIFO** (`:142`, API at `:2340`). Tags each view with the eye it
  applied and pushes; `Present` pops in order. Because the tag travels with the
  frame, pipeline depth is irrelevant and a one-frame slip cannot accumulate.
  `CameraHook_EyeQueueStats` reports depth — `min == max` means lockstep.
- **The latched-pose channel** (`:179`). Publishes the pose the camera was
  *actually rendered from* — the quat latched at eye-0 time plus the applied,
  clamped head-centre position. The projection layer must carry this, not the
  freshest pose at submit time. Removing it reintroduces flicker.
- **Motion aim** (`:476`), head aim (`:821`), applied-shot direction (`:717`),
  snap turn, `ModYaw` (`:1903`), the pitch servo (`:550`, must stay off).
- **Delta clamp** (`:947`) — one world advance per eye pair, so both eyes show
  one instant.
- **6-DOF hand writes** (`:882`) and the late rotation write (`:911`), re-applied
  from the render thread after the game tick has had its say.

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
walk direction. `AimSource=1` writes the controller's direction into it, so
motion aim necessarily drags locomotion along. `AimSource=0` puts everything on
the head, which is coherent but is not motion aim.

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

## Suggested split

- `CameraHook.cpp` — detour and install
- `CameraScan.cpp` — module scan and the six stages (`:256–441`), delta scan (`:2439`)
- `EyeQueue.cpp` — FIFO (`:142`), latched pose (`:179`), FIFO API (`:2340`)
- `AimControl.cpp` — motion aim (`:476`), pitch servo (`:550`), shot direction
  (`:717`), head aim (`:821`), ModYaw (`:1903`)
