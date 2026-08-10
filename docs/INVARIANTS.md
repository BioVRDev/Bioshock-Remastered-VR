# Invariants and dead ends

Two lists. The first is settled and must not drift. The second is falsified —
each entry cost real time, and every one of them is plausible enough to be
proposed again. **Check both before suggesting anything.**

Verified against the source at `002a81a`, not copied forward from the handoff
documents — several of their claims no longer hold. Where this file and an older
handoff disagree, this file wins.

---

## Settled — do not change without new measurement

### Rendering and geometry
- **`g_fovMode = 0`.** Mode 1 submits the real asymmetric headset frustum while
  the game renders a symmetric image. It was a diagnostic, was left on once, and
  caused severe eye misalignment.
- **Shim vertical projection is `U = b`, `D = t`.** Not `U = -t, D = -b`, and
  **no swap guard** — a swap cannot detect a mirrored principal point, which is
  the bug it would be hiding. Validate `l < r` and `D < U`, and log the raw
  values. This was the SteamVR warp fix.
- **Shim plane distance `Dq = 50`.** `1000` was a discarded "plane at infinity" test.
- Game FOV and the FOV reported to OpenXR must describe the same image.
  `GameIni.cpp` keeps them synchronised; the log echo is the proof.
- Foreground FOV is derived, not constant:
  `ForegroundFov = 2·atan(tan(HFOV/2) / aspect · 1.3333)`. `127` is correct only
  for 3072×3264 at FOV 110.
- **D3D11 vtable slots**: `Present` 8 · `DrawIndexed` 12 · `Draw` 13 ·
  `DrawIndexedInstanced` 20 · `DrawInstanced` 21 · `OMSetRenderTargets` 33 ·
  `OMSetBlendState` 35. **14/15 are `Map`/`Unmap` — never hook them as instanced.**
- `MirrorPresentEvery = 0` means **time-based** desktop presentation (~17 ms), not
  "never present". Never presenting measured *worse* (85 Present/s vs 240).
- Exclusive fullscreen is not an acceptable fix for anything. It snaps the
  portrait VR render to a real monitor mode and changes projection calibration.

### Threading and frames
- `eventPlayerCalcView` is the **game thread**; `Present` is the **render thread**.
  Cross-thread data uses a seqlock or the eye-tag FIFO — never a loose flag.
- The projection layer carries **the latched pose the image was rendered from**,
  not the freshest pose at submit time. This was a major flicker fix.
- ~2 Presents per XR submit. `EYEQ: depth min=1 max=1` is the health signal.
- `DeltaClamp` advances the world once per eye *pair*, so both eyes represent one
  instant. Reset carried state when a new world identity appears.
- Advance the aim base **before** composing view, aim field and hands in the same
  frame. Doing it after leaves the gun one frame of yaw ahead of the view, which
  reads as the weapon swelling and flickering while you turn.

### Engine access
- Find functions by **pattern**, never by hardcoded RVA. The FName/string chain
  for `CalcView` works on Steam, Epic *and* GOG; every hardcoded address has
  failed across storefronts.
- Any address you did not derive must be INI-overridable and verified before use
  (`EngineExec`, `ArmHide` both do this). Fail closed on mismatch.
- **`AActor::Location = +0x1D8`**, one unit = 1 cm. `+0x1A0` was a bad early scan.
- **Bone 43 is untouchable.** Telekinesis release walks the attachment path
  through it; moving or scaling it crashes the game.
- Hand rotation offsets **compose as quaternions**. Adding Euler trims shears
  near vertical wrist orientations.
- Never add a per-frame memory scan. One-shot, lock, stop.

### Input
- **`ControllerMode = 1`** (replace XInput with VR input) is the default. Mode 0
  lets any successful XInput slot-0 device win, so Virtual Desktop, Steam Input,
  ViGEm or a plugged-in pad silently kills every VR button.
- **Optional OpenXR functions must be resolved with `xrGetInstanceProcAddr`.** A
  direct static import of `xrGetCurrentInteractionProfile` added an import the
  shim does not export, and the mod stopped loading in shim mode entirely.
  `OpenXRShim/exports.def` is the complete surface — 36 functions.
- Native OpenXR interaction-profile suggestions and the shim's SteamVR bindings
  are **separate systems**. `xrSuggestInteractionProfileBindings` is a no-op in
  the shim; its bindings are generated from string literals in `shim_input.cpp`.

### Architecture
- **Draw signatures may control cosmetic presentation. They must never gate
  camera or input behaviour.** A false-positive draw count once froze turning
  because the aim-base update was gated on `DrawHook_MenuUp()`.
- Fix lifetime, not range. Do not cap a feature to hide stale state — reset it at
  save, level and pawn boundaries.
- Clear every cached actor pointer immediately when the pawn goes null.

---

## Falsified — do not retry without new evidence

### The HUD square (solved)
- **The scene-sampling "world leak guard" is dead.** The theory was that a
  full-screen post-process pass sampling the scene target was being captured as
  HUD. It was built with `HudLeakLog` telemetry and **fired zero times across
  11,682 log lines** while the capture was taking 130+ draws per frame.
- **`DrawHook_NoWorldRender()` in the HUD gate is dead.** It fired once, at
  startup loading, and never during the square.
- **`g_gameplayConfirmed` in the HUD gate is dead** — and instructively so. It is
  computed in the fingerprint function, while the gate lives in `DrawHook_EndFrame`,
  which resets `g_indexedThisFrame` *before* the confirm block reads it. The flag
  was therefore permanently false and the HUD never captured at all. **When
  borrowing a per-frame counter, check which function resets it.**
- The isolate walker cannot find a rare short-lived effect: it steps one candidate
  per keypress, so an N-candidate table needs N separate 5-second windows.
- **What actually worked:** the interface is untextured GameSWF geometry
  (`CAPTURED: 5d tex=no`); the square was a textured full-screen quad
  (`CAPTURED: 6d tex=yes`). One bound shader resource separates them.
  `PSSrv0Res(ctx) == nullptr` in the redirect condition of `Hud/DrawHook.cpp`.

### Cutscene detection (still open — these are the graves)
- **ViewActor divergence.** Does not leave the pawn on this build; `+0x450`,
  `+0x620` and `+0x914` all track the pawn for entire sessions.
- **Pitch-rate as a binary detector.** Latched during ordinary combat for four
  consecutive seconds. Demoted to telemetry.
- **The pitch servo.** Fed right-stick Y back into the head-aim accumulator,
  produced a runaway loop, and froze the view and hand. Must stay off.
- **S75/S78/S79 render-side turn/unwind.** Made scripted sequences worse.
- **`LastPlayerInputContext` as a live string.** The scan brackets the window
  correctly (`+0x728..+0xA7C`, anchored on `FirstPersonHands.PlayerHands`) and has
  **never locked** in any session. Nothing in the decompile writes it from script.
- **Console `get` for live state.** The read channel works, but `get` returns the
  **class default object**: `Health` stayed `200.0` after taking damage,
  `Location` stayed `(0,0,0)` after walking. Fine for defaults, useless for state.
- **The input-ignored detector** (stick deflected + Acceleration ≈ 0) is sound in
  principle and the offset is measured — but it needs the player to push the
  stick, so it produces no verdict when a cutscene starts while standing still.

### Aim and movement
- **`AimSource = 2` ("character mode") cannot exist.** It stops writing
  `Controller.Rotation` and assumes the game keeps updating it — but `ModYaw`
  zeroes `sThumbRX`, so the game's heading freezes permanently. Bullets fire in
  one fixed world direction while the crosshair follows your head.
- **The body-follow yaw servo** (transfer head yaw into the heading, add the same
  step to a recenter reference) was ported from the reference mod and did not
  feel right here. Removed.
- **`ModYaw` breaks scripted movement.** Zeroing `sThumbRX` freezes
  `Controller.Rotation`, and forced-move sequences steer by it — the opening
  bathysphere walks the player into the back wall and the projector never plays.
  `FreezeGameRotation` requires `ModYaw`, so both are currently inert by default.
- **The coupling is structural.** `Controller.Rotation` (`+0x1E4`) drives the
  view, the weapon trace *and* the walk direction. No arrangement of that one
  field separates aim from movement. Decoupling requires finding what the firing
  trace reads, which is unknown.

### Other
- `TryPassThrough`, loader export patching, 5-byte JMP redirection. Reverted in
  favour of selector/copy packaging.
- Fixed `2750×2850` shim eye targets. Use dynamic app-swapchain sizing.
- `hq.layerFlags = 0` — an alpha diagnostic only; restore normal blend flags.
- `MenuMaxIndexed = 0` as a permanent setting. Broke pre-game menus and did not
  remove the square.
- Aggressive idle-animation parking (`IdleAnimMode=2`); `AdditiveHandBobAnim` and
  `WeaponBobDamping` as sway fixes — both observed inert.
- `GripTunedFgFov` as a closed-form calibration law. Measurements too inconsistent.
- `CutsceneTheater=1` as a general fix. It forces *every* cutscene onto the flat
  quad; it anchors the opening correctly but is wrong for in-world moments.
