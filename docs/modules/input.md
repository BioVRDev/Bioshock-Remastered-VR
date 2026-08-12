# Input

`Input/InputHook.cpp` (1338), `Input/Swing.cpp` (222), `Core/Keybinds.cpp` (219, **unwired**).

## Two independent halves

| Half | Thread | Job |
|---|---|---|
| Producer | render (OpenXR) | `Input_XrCreate` / `Input_XrSync` — one action set, synced once per XR frame, published through a seqlock |
| Consumer | game input | the `XInputGetState` detour — reads the seqlock, fills an `XINPUT_STATE` |

They never share a lock and never block each other. If the game stops polling
XInput the producer keeps running harmlessly, and the once-a-second `POLL:` line
says which half is dead:

```
POLL: getState 90/s synth 90/s realpad 0/s     healthy replacement
POLL: getState 90/s synth  0/s realpad 90/s    a real pad is winning — see below
```

**Action sets attach to a session exactly once.** Every action the mod will ever
want — including aim/grip poses and haptics — is created in `Input_XrCreate` even
where nothing reads it yet. Adding one later means restarting the whole session.

`Input_XrSync` is called from **both** `XR_SubmitPair` and `XR_SubmitMenuMono` —
menus need input too. When the session is not focused it publishes a *neutral*
pad, so taking the headset off cannot leave the player walking.

## `ControllerMode`

**`1` is the default and it matters.** Mode 0 was described as a "merge" but was
not one: if the original XInput slot-0 call succeeded it returned immediately and
never synthesized VR input. Virtual Desktop, Steam Input, ViGEm or a plugged-in
gamepad could therefore make every VR button and stick appear dead while hand
poses and weapon aim still worked — a confusing failure that looked like a
tracking bug.

A genuine merge would need to read both sources, OR the buttons, take the larger
trigger/stick magnitudes, and arbitrate packet numbers and activity. Not built.

## Hand poses

Both hands, both pose types, full 6-DOF, published every XR frame in the app's
LOCAL space:

- **`aim`** — the runtime's own pointing ray. Use for *aiming*; it already
  accounts for how a Touch controller is held.
- **`grip`** — the physical hold point. Use for *placing* a weapon model.

`Input_GetHandPose` returns false if the seqlock could not be read cleanly.
Check `aimValid`/`gripValid` separately — a clean read of an untracked hand is
still a clean read.

## Stock controller semantics

| Control | Action | | Control | Action |
|---|---|---|---|---|
| A | Use | | LB/RB hold | plasmid / weapon radial |
| B | Med hypo | | LT/RT | plasmid fire / weapon fire |
| X | Hack / reload | | L3 | Duck |
| Y | Jump | | R3 | Zoom, or jump override |
| START | Pause | | BACK | Context help; held reaches map |

## VR mappings

- **D-pad modifier** (`ControllerDpadModifier`): `0` off · `1` right thumbrest ·
  `2` R3 · `3` left grip · `4` left thumbrest. **Rift has no thumbrest sensor —
  those users must use `2`.**
- **`ControllerDpadFlip`**: `0` = right thumbrest + left stick (walking stops
  while held). `1` = left thumbrest + right stick (walking continues, turning is
  suspended instead).
- **Pause chord** (`ControllerPauseChord=1`): X+Y → START. Necessary because on
  many setups no menu button reaches the game at all — SteamVR claims the left
  one, the Meta runtime the right. Modifier + X+Y → BACK; hold for the map.
  Y is only suppressed *while X is held*, so a lone Y still jumps and still
  speeds up hacking. R3 is a backup jump.
- **Grip**: `GripThreshold` + `GripHysteresis`. Index grips read high from a
  resting hand, which would leave LB/RB permanently held and eat the face buttons.
- **Head-relative movement**: rotate the left-stick vector by head yaw *relative
  to the body heading* — not by final absolute camera yaw, since BioShock already
  applies body yaw and using world yaw double-rotates.

  ```cpp
  outX = moveX*cos(yaw) + moveY*sin(yaw);
  outY = moveY*cos(yaw) - moveX*sin(yaw);
  ```

  Gate it out of menus, radials, d-pad modifier use, invalid tracking, and
  theater/non-gameplay states.
- **Snap turn** changes `g_aimBase.yaw` directly and suppresses continuous
  right-stick turning. The obsolete timed right-stick burst must stay removed.
- **Right-stick Y is dropped** by default (`ControllerPitch=0`): under
  `HeadAimMode=2` the camera hook erases injected pitch ~8 ms later and it reads
  as a fight.

## The game's deadzone is square, and the mod pre-compensates

`User.ini` binds both movement lanes with a **per-axis** threshold —
`XENON_LTHUMB_XAXIS`/`YAXIS`, `DeadZone=0.225`. Rotating the stick to redirect
walking moves magnitude between the two axes, and a per-axis threshold applied
after that **changes the direction**: up to ~11°, and a hard collapse to pure
strafe once the forward lane drops under the threshold. That was the walk drift
reported for four builds.

`StickPrecomp` inverts the game's transform before `ToAxis`:

```
send_i = sign(u_i) · ( |u_i| · m · (1 − d) + d ),   zero components stay zero
```

The game decodes that back to exactly `u_i · m` — direction and magnitude both
exact, verified across the full angle range. The result is bounded at 1.0, so it
never clips. **Applied only when the stick was actually rotated**; an unrotated
stick meets the game's deadzone exactly as it always has, which is the vanilla
feel and is not broken.

> **`User.ini` is deliberately NOT edited to fix this.** Those lines carry several
> bindings each — `XENON_LTHUMB_XAXIS` also holds `Axis xLean DeadZone=0.4` — the
> file has multiple binding sections, and the game rewrites it at exit. String
> surgery there risks breaking the controls outright, for a value we can invert.

## The grip is shared with the two-handed grab

The off-hand grip opens the plasmid radial **and** grabs a two-handable weapon.
Proximity disambiguates them, and two details are load-bearing:

- **Suppression is gated on ELIGIBLE, not on GRIPPED.** `FillFromPad` emits
  `XI_LSHOULDER` the moment the grip crosses its threshold, while the two-hand
  state machine only reacts on the next `CalcView` — so gating on *gripped* leaks
  one frame of LB and flickers the radial open on every grab.
- **`Input_GripDown` publishes the hysteresised state** rather than letting the
  state machine re-threshold the raw axis. A second test would engage on a
  different frame from the radial and reintroduce the same leak.

**`TwoHandBlockRadial` (default 1) goes further:** while a grabbable weapon is up,
the grip never opens the wheel at all, in or out of the zone. A near-miss should
do nothing rather than throw a menu at you, and removing that cost is what allows
a generous grab radius. One-handed weapons keep the proximity-gated behaviour.

## Every `xr*` call must be exported by the shim

`BioshockVR.dll` statically imports `openxr_loader.dll`, which on the SteamVR path
**is** the shim — so calling a function it does not export makes Windows refuse to
load the mod, reported as `FAIL: BioshockVR.dll not found beside dxgi.dll` for a
file that is present.

**The trap is the CALL, not the function.** `Input_Pulse` sat compiled and unused
from M6 until 2026-08-12; with no call site the linker never emitted the import,
and adding the first call broke the launch. Audit the built binary, not the
source. Resolve anything outside the shim's export table through
`xrGetInstanceProcAddr` and treat null as "feature absent" — which is what
`Input_Pulse` now does, so haptics are silently unavailable rather than fatal.

## Physical wrench

`Swing.cpp` is a **gesture-to-button adapter**, not collision: head-relative hand
velocity past a threshold emits a short synthetic right-trigger pulse, and
BioShock's own wrench attack runs normally — animation, sound, damage, timing and
Havok collision all unchanged.

Called once per **OpenXR frame** from `XR_SubmitPair`, never from `CalcView` — the
stereo camera path runs more than once per presented frame and would sample the
same pose twice, computing nonsense velocities.

⚠ **Gated on `HandsProbe_WeaponSlot() == 0`.** With `EnableHandsProbe=0` the slot
stays `-1` forever and every swing is silently rejected as "not the wrench". If
physical melee tests as completely dead, check that switch first.

`Swing_Reset()` on level load or anything that teleports the hand — otherwise the
jump reads as a swing.

The decisive A/B for melee complaints is `SwingEnabled=0`, `PitchServo=0`, then
attack with the trigger while looking level. If the trigger also hits the floor,
the stock melee orientation is wrong; if the trigger is reliable and gestures
miss, it is gesture timing or direction.

## `Keybinds.cpp` — built, complete, and never called

Nothing outside the file references `Key_Init`, `Key_Down`, `Key_Fired`, `Key_Vk`
or `Key_Name`. Every key check in the codebase is a raw `GetAsyncKeyState`, which
is why keys collide:

| Key | Readers |
|---|---|
| `VK_PRIOR` | `Game/GameState.cpp` (float snapshot, in `PollProbeKeys`), `Hands/HandsProbe.cpp` (hands-mode snapshot **and** the numpad-9 mode alias) |
| `VK_DELETE` | `Render/XRSession.cpp` (`PollFovKeys`, HUD-quad edit param), `Game/GameState.cpp` (the `GETTEST` probe) |

One press fires all of them. Wiring `Keybinds` gives the key map a single owner
and fixes the collisions and the per-frame VK sweep at the same time — which is
why it should be wired, not deleted. It also delivers the feature it was written
for: users without a numpad currently cannot rebind anything.
