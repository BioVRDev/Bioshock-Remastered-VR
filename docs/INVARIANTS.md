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
- **`Present` fires twice per stereo pair, and the backbuffer holds a DIFFERENT
  eye each time. Anything flat must take ONE of them.** Verified in a headset
  twice over, 2026-08-12. The menu quad submitted on both Presents, so the flat
  panel alternated between the left and right views — one IPD of horizontal
  shimmer on a surface being read. The desktop mirror had the same bug for the
  same reason, reported as *"on the monitor it flickers a lot which means people
  cant record it"*. Both now take `eye == 0`. **Put the eye test BEFORE the
  mirror's interval timer**, or a skipped frame eats the interval and halves the
  desktop rate.

- **The `paused` term was silently anchoring every machine screen, and clicking
  through unpauses.** Measured 2026-08-11, and the tester diagnosed it before
  either agent did. `Hooks.cpp` routes the composed frame on
  `starved || paused || anchorUi || …`; a machine's *first* page pauses, so it
  was anchored and correct, and its *second* page unpauses while the interface
  stays up — the panel's only reason to exist evaporating underneath it:

  ```
  23:49:11.888  PAUSE: PAUSED                     <- screen 1, anchored
  23:49:18.998  PAUSE: unpaused                   <- clicks OK
  23:49:19.002  MENU SCREEN off                   <- screen 2 drops to the HUD
  ```

  **This generalises to every multi-page interface in the game.** Fixed by naming
  the second page (`PlasmiNow`) in `AnchorMovies`; the general fix is
  `ShockPlayer.CurrentStation`, still unhunted.

- **The composed-frame route and the HUD capture are mutually exclusive by
  construction** — running both draws the interface twice, once inside the flat
  picture and once on the panel. That is why the HUD gate closes on pause, and it
  is the only thing that stopped menus riding the HUD quad. `PanelMovies` inverts
  both couplings for a named screen and ships empty; **the tester tried it on the
  tonic flow and preferred the flat route**, so it has no users yet.

- **A screen shown as the game's own composed frame must have the HUD capture
  off.** The anchored and head-following routes both submit the composed frame as
  a mono quad; the capture lifts the interface layer *out* of that frame, so
  whatever it takes is missing from the picture. MEASURED, Build O: with
  `Maps.swf` on top the capture took **3 draws per frame** and the map rendered
  with correct paper, frame and tabs and **blank contents**; with
  `ingamemanual.swf` on top it took **0** and that screen read perfectly. The
  divider is texturing — the map's contents are untextured GameSWF vector
  geometry and pass the `PSSrv0Res` guard into the capture. **The HUD gate cannot
  cover this**: it closes on `GameState_MenuUp() || GameState_Paused()`, and these
  screens do not pause — the anchored window ran a full minute without one
  `HUD GATE` line. `DrawHook_ComposedFrameUp()` is the one owner, read by the quad
  router and by the capture condition.

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
- **Natives are reachable by their `int<Class>exec<Func>` registration symbol**
  — measured M3-S1, 2026-08-10. The exe holds that wide string once in `.rdata`,
  and exactly one 12-byte row in `.data` points at it; the row's second DWORD is
  the function pointer, written at runtime (it is **zero on disk**). This is
  general: any registered native can be found this way. The row's RVA differs
  between Steam and Epic, so locate by the string, never by the row.
- Any address you did not derive must be INI-overridable and verified before use
  (`EngineExec`, `ArmHide` both do this). Fail closed on mismatch.
- **`AActor::Location = +0x1D8`**, one unit = 1 cm. `+0x1A0` was a bad early scan.
- **Bone 43 is untouchable.** Telekinesis release walks the attachment path
  through it; moving or scaling it crashes the game.
- **The render bone array is a common model space, in centimetres, on the
  actor's own axes** — measured M6-S1, 2026-08-10. Bones sit tens of units apart
  in one shared frame; parent-relative would have given small per-bone offsets.
  A rendered distance is model units × `HandsScale`. The lane order reads as
  (forward, right, up) from the rest pose, which is anatomy, not a measurement —
  it lives in `LeftHandAxisMap` for that reason.
- **The array keeps evaluating in ordinary play even while the dirty byte is
  cleared every CalcView** — bone 27 moved between every pair of M6-S1 dumps
  with the sleeve pass running. The engine re-flags each tick and our writes
  stick because they land after evaluation. **Do not generalise this to scripted
  sequences**: the M7-S4 latch was real and is recorded below.
- **The build stamp does not always advance.** `dllmain build:` carries
  `__TIME__` from `Core/dllmain.cpp`, so an incremental build that does not
  recompile that file ships a new DLL reporting an old time — observed
  2026-08-10, a DLL built at 18:08 stamped 17:52. Bumping the label is what
  moves the stamp, so bump it every session; the file timestamp in the game
  folder is the independent check.
- Hand rotation offsets **compose as quaternions**. Adding Euler trims shears
  near vertical wrist orientations.
- Never add a per-frame memory scan. One-shot, lock, stop.
- **A scripted-sequence signal exists and is measured** — `hands+0x594` bit 2,
  `CurrentlyExecutingScriptedHandAnimationSequence` (M7-S1, 2026-08-10). Exact
  bracket, zero false positives in six minutes of mixed play. **It does not cover
  the Little Sister rescue or the EVE injection** — those are Hands *states*, a
  different mechanism. `docs/ENGINE-MAP.md` § *Hands actor*.
- **An oracle you only LOG is not a guard.** The bathysphere read had a perfect
  self-check built in — the same engine call that sets `bCannotFall` clears the
  capsule bit, so a genuine ride always shows `capsule=0`. It was printed on
  every line and gated on nothing, and a stale pawn pointer holding ASCII
  (`0x32313936`) raised a false `BATHYSPHERE MODE ON` whose own log line said
  `capsule=1`. **Gate on the oracle, do not just print it.** Same for any bool:
  a lone bool reads exactly 0 or 1, so anything else is a wrong pointer.
- **You cannot WRITE a bone and measure it at the same time — hiding, driving or
  anything else.** `ArmHide` clears the skeleton's dirty byte so its writes
  stick, which stops the engine re-evaluating **the whole bone array**, so any
  signal read from that array freezes the instant anything writes through it.
  M7-S4 hid the arms on a motion signal sampled from the same array and produced
  a **bistable latch**: a scene entered hidden could never un-hide (motion read a
  flat `0.0000` while the same metric peaked at 3.77 elsewhere in the run), and a
  scene entered visible could never hide. Hide through `DrawScale3D` on the actor
  instead, or measure something the write does not touch.

  **It came back through the other door on 2026-08-11, and the wording is why.**
  The rule said *hide*; the M6-S1 cluster **drive** writes bones and clears the
  same byte, and the motion probe's fixed `kMotionBone = 27` sits inside the
  right cluster — which is the driven one during every plasmid scene. Arms stayed
  hidden for the whole balcony scene, with `raw` reading **exactly** `0.0000` for
  189 consecutive samples in one run and 223 in another. Bit-for-bit zero rather
  than merely small is the signature: a rigid transform from a captured reference
  reproduces the identical pose while the controller is still.

  **It presented as a movement-mode bug** (arms appeared in mode 2, not in 0 or
  3) at one run per mode. `MovementMode` rotates a stick and cannot reach the
  bone array; mode 2 was simply the run with more controller motion. The
  sampled bone is now chosen against the driven cluster.
- **When a differential probe shares a log budget with noisy windows, the noisy
  windows eat it.** M7-S1's 200-transition cap was consumed in six seconds by the
  controller and pawn windows (117/256 and 206/288 fields non-zero and churning),
  so the differential was dead before the event it was built to catch. The run was
  saved only because the *named* watch was deliberately logged outside the cap.
  **Cap per window, and always log the named prediction separately.**

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

### Locomotion and the aim field
- **The game's movement stick has a SQUARE deadzone, so ROTATING it distorts the
  direction.** `User.ini` binds both movement lanes with a per-axis threshold:

  ```
  XENON_LTHUMB_XAXIS=Axis xStrafe   Speedbase=1.0 DeadZone=0.225 | ...
  XENON_LTHUMB_YAXIS=Axis xForward  Speedbase=1.0 DeadZone=0.225 | ...
  ```

  Rotating the stick to redirect walking **moves magnitude between the two
  axes**, and the game then shrinks each axis independently — so what it walks is
  not what was sent. Modelling it as `out = (|a| − d)/(1 − d)` reproduces the
  logged sent-vs-received pairs **seven for seven**:

  | sent | predicted | logged |
  |---:|---:|---:|
  | −72.0 | −83.4 | −83.4 |
  | −74.7 | −87.0 | −87.0 |
  | +78.6 | +90.0 | +90.0 |
  | +162.1 | +173.5 | +173.6 |
  | −13.6 | −0.8 | −0.8 |

  The ±90.0 saturation is the signature: once the forward lane falls under 0.225
  it is zeroed and the walk collapses to pure strafe. The residual clusters at
  **±11°** and is worst near 90°, where the forward component is smallest.

  `StickPrecomp` inverts it — `send = sign(u)·(|u|·m·(1−d) + d)` — which the game
  then decodes back to exactly `u·m`. Applied **only when we rotated**; an
  unrotated stick meets the game's deadzone exactly as it always has.

  > **The general rule this cost four builds to learn: when a correction is
  > provably exact and the symptom survives, stop refining the correction and
  > measure what the other side actually received.** `R` was algebraically exact
  > from the first attempt; the distortion happened after the value left us.


- **Graveyard entry 13 binds AIM, not locomotion.** `Controller.Rotation` drives
  the view, the weapon trace *and* the walk direction, and no arrangement of that
  one field separates the first two. **The walk direction is separable anyway**,
  because the game applies the stick angle *on top of* the field:

  ```
  walk = aimFieldYaw + stickAngle + R
  ```

  Rotating the stick by `R` redirects walking while leaving aim, the weapon trace
  and forced-move sequences untouched. `HeadRelativeMove` had been doing exactly
  this since it shipped; the four `MovementMode` values are just values of `R`.
  Entry 13 as previously written would have forbidden work that demonstrably
  works.

- **Compute anything derived from the aim field at the write site, from the
  rotator actually written.** `ComposeHeadLocal()` is a **basis multiplication**,
  so in general the resulting yaw depends on the pitch too, and
  `aimFieldYaw = base + headYaw + controllerOffset` is an approximation.
  `PublishShotDir` has always done it the right way for the crosshair;
  `PublishWalkRotation` now does it for locomotion.

  > **⚠ CORRECTION, and the reason it is worth the space.** This entry first
  > claimed the composition error *was* the cause of a residual locomotion
  > coupling. **It was not, and the claim was not checked against the live
  > config.** With `HeadAimMode=2` — the shipping default — the base pitch is
  > dropped, `M` becomes `Rz(yaw_base)`, a **pure yaw**, and
  > `want.yaw == base.yaw + aimY` *exactly*. The measured form and the predicted
  > form are numerically identical there, which is why the drift survived the
  > change unaltered: *"still present and the exact same"*. The measured form is
  > still correct and still preferred — it is right for `HeadAimMode` 0 and 1 —
  > but **check which head-aim mode is live before blaming the composition.**

- **A cinematic reference must be dropped on BOTH edges of the window.**
  Anything that differences a value frame-to-frame across a scripted scene needs
  a reference, and that reference is valid **only within one window**. A latch
  set on the first scripted frame and never cleared makes the *second* window of
  a session difference against a value left over from the **end of the first** —
  an arbitrary jump. Measured as *"both runs had the balcony fall land in
  different spots; first almost perfect, second way off"*: the first scene of a
  run is clean and every one after it inherits garbage.

  **Drop it at the edge detector, not inside the feature.** The follow block sits
  behind `headAim`, `headTracking`, the UI gate and the starvation gate, so a
  window that ends while any of those is false would never clear it. The
  reference mod states the same rule for its own cinematic reference: *"drop the
  look reference so the next shot opens framed as authored rather than wherever
  this one ended."*

- **The pawn's rotator tracks the aim field exactly — falsified 2026-08-11.**
  UE2 builds movement acceleration from `GetAxes(Pawn.Rotation)`, so the pawn
  looked like a candidate basis for a residual walk drift. It is not: 60 of 62
  samples read `aim-pawn +0.0`, **including while a 76° controller offset was
  held**, which is the exact condition the hypothesis was invented for. Do not
  re-propose the pawn rotator as a separate movement basis.

- **A scripted scene can rotate you on the game's OWN camera and never touch the
  aim field — SOLVED 2026-08-11.** The balcony fall had never turned the player
  since the mod existed. Measured across the whole 67-second scene:

  ```
  game injected 0.00 deg/s into the AIM FIELD, 125.21 deg/s onto its own CAMERA
  gates cut=0 freeze=0 gameplayFreeze=0 rotBlocked=0
  ```

  Nothing was being discarded — every gate was open. The rotation simply lives on
  `*CameraRotation`, which head aim overwrites wholesale every frame, while the
  Little Sister scene (which always worked) puts its rotation on the aim field.
  **Two different scenes, two different fields.** `ScriptedCameraFollow` follows
  the camera too and ships on. Do not assume a scripted rotation arrives on
  `Controller.Rotation` because a previous one did.

- **Turn rate is NOT frame-rate linked — falsified 2026-08-11.** 40 samples,
  CalcView between 142 and 239 calls/s, **no correlation at all**: at ~230
  calls/s the measured rate ran from 52.9 to 215.6 deg/s. What it tracks is stick
  deflection, on a curve that is nearly vertical at the very top — `0.98` gives
  ~105 deg/s, `0.99` ~140, `1.00` ~200. A 2% difference in push doubles the rate,
  which is the whole of the long-standing "sometimes slow, sometimes fast"
  report. `TurnAxisMax` keeps the cliff unreachable.

- **The stick-rotation identity is only valid while WE own the aim field.**
  `R` subtracts the head term `H` and the controller term `O` on the assumption
  that the field carries `base + H + O`. **During a scripted sequence it does
  not** — M7-S3 deliberately suppresses the write (grep `THE SUPPRESSION LIVES
  HERE NOW`), so the field holds the game's own heading and contains neither.
  Subtracting them there steers the sequence off its intended path.

  Measured 2026-08-11 on the plasmid balcony scene: *"when you enter it, you are
  turned slightly to the left, which causes the walking path to move you to the
  wrong position."* Gate on `CameraHook_OwnsAimField()`, never on the mode alone.

  **It presented as a mode-0-only bug and was not.** Mode 2 subtracts only `O`,
  which is zero while the head owns the aim — so an *unarmed* scripted scene
  masks it completely while an armed one drifts by up to `AimClampDeg`. The mode
  that looked clean was the one that got lucky. **A mode-specific symptom is not
  evidence of a mode-specific cause.**

- **NEVER write `Controller.Rotation` while a sequence is moving the player, and
  a scripted animation's aim ownership must be decided ONCE — 2026-08-11.**
  Both halves were measured on the balcony fall, and each cost a session.

  *The write.* Any write in that window fights the game's own move. Three falls
  under a build that never writes there, entered at wildly different controller
  angles, landed on the **same spot with the same facing** — so the entry heading
  does not steer a forced move at all. See § *Falsified*.

  *The decision.* `ControllableScriptedFix` re-judged "is the player still in
  control" **every frame**, from `HudIsUp()` — a 500 ms peak-hold on a
  render-thread draw counter. One millisecond after the forced-move flag dropped
  it could flip, the aim write resumed for 107 ms while the game was still moving
  the player, and that fall landed **3.7 m** from the two whose window stayed
  intact — and those two agreed **to a tenth of a unit**. Whether the HUD happens
  to draw inside that half-second is a race, which is exactly why the symptom was
  *"different every single run"*.

  The verdict now locks inside the first second and is frozen for the animation
  (`GameState_ScriptedInControl`, grep `ONE VERDICT PER SCRIPTED ANIMATION`).
  Undecided reads as "the game owns you" — fail closed. **A per-frame predicate
  over a racy signal is not a safe way to answer a per-scene question.**

- **One scene raises two signals in sequence, and the order between them is not
  guaranteed — 2026-08-11, the Little Sister crawl.** The forced move walks the
  player into place, then the scripted animation plays. M7-S6 measured the forced
  flag dropping **0.09 s after** the animation begins, so they normally overlap
  and their union never gaps. On one run the order reversed:

  ```
  22:46:10.556  FORCEDMOVE: --- forced move done ---
  22:46:10.557  SCRIPTED: aim released back to the player     <- the collapse
  22:46:10.557  SCRIPTED EXIT : aim y +0.00 p +0.00 | base y -19.64
  22:46:10.561  SCRIPTED: *** SCRIPTED ANIMATION BEGAN ***    <- 5 ms later
  ```

  **One frame** at 231 CalcView/s. In it the mod released the aim, re-armed
  `g_aimBase` from an aim field reading **exactly (0,0)** while the true heading
  was `-19.64`, and wrote that back — a write inside a live scene, which the
  invariant above forbids. The damage held for the whole 58-second scene, visible
  in a field nothing else touches: `aim-pawn` sat at exactly **`-18.6` for 58
  seconds**, having read `+0.0` on every sample before it and returning to `+0.0`
  after. The crawl walked that far wrong.

  Two rules come out of it, and either alone would have prevented it:

  1. **The union of the two signals is held** — rises instantly, falls only after
     `ScriptedWindowHoldMs` with neither set (`GameState_ScriptedWindow`, grep
     `ONE SCENE, ONE WINDOW`). Held on the *shared* signal, not in either
     consumer, because the aim suppression and the turn-axis release layer
     deliberately different policies on top of it.
  2. **Arming the aim base is a write, so it gets the harder check.** Whatever
     lands in `g_aimBase` becomes the player's frame of reference for both the
     aim field and two of the four movement modes. An exactly-zero pitch *and*
     yaw is the transient's fingerprint and is refused (bounded, so a genuine
     zero cannot hang the aim). Comparing against the pawn's rotator is **not** a
     usable test here — the two legitimately diverge inside a scene.

  **A signal that is correct is not the same as a signal that is continuous.**
  Both of these were reading exactly what they were built to read.

- **A scene can put its rotation on the aim field AND the game's own camera at
  the same time — 2026-08-11.** The balcony's opening snap moves both, and by the
  same amount to the hundredth:

  ```
  game injected 41.03 deg/s into the AIM FIELD, 41.03 deg/s onto its own CAMERA
  ```

  Following both applied it **twice**, and the view finished one whole snap past
  the authored heading — the error equalled the snap in three consecutive runs,
  sign included (+41, −4, −77). The camera is downstream of the aim field, so it
  already carries anything the scene did to `Controller.Rotation`: follow the
  camera alone (grep `ONE SOURCE FOR SCRIPTED YAW`) and each rotation counts once
  whichever field the scene used.

- **A deg/s rate averaged over a long window cannot see a one-frame event.** The
  double-count above hid for three sessions behind the measurement that motivated
  the camera follow in the first place — *"0.00 deg/s into the aim field, up to
  125 deg/s onto the camera"*, averaged across 67 seconds. The one second where
  both moved together averaged into invisibility. **When a probe reports a rate,
  ask what it would do to a spike.**

### Architecture
- **A "does this look like an engine object" check must not be applied to a
  buffer — 2026-08-11.** `GsLooksLikeObject` requires a vtable whose first slot
  is executable. A `TArray`'s data pointer fails that by construction: its first
  word is element 0, whose first word points at a vtable, and **a vtable lives in
  read-only data, not executable memory**. Running the object test on array data
  rejected a correct pointer every time and hid the Flash interface controller for
  three sessions. Fail closed is right; failing closed on the wrong predicate is
  not. Use `GsLooksLikeBuffer` for anything that is not itself an object.
- **A pointer walk that bails must say WHICH STEP bailed.** The same chain logged
  one line — `chain did not resolve` — and latched it, so a permanent failure and
  a not-yet-ready one were indistinguishable. Report the step and the value, and
  retry on a backoff rather than latching the first miss.
- **Draw signatures may control cosmetic presentation. They must never gate
  camera or input behaviour.** A false-positive draw count once froze turning
  because the aim-base update was gated on `DrawHook_MenuUp()`. **This is the same
  mistake `ControllableScriptedFix` made from the other direction** — see the
  ownership invariant above.
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
- **`myHUD.bHideHUD` as the cinematic flag.** *Falsified live, M1-S2,
  2026-08-09.* The offset work is sound and stands: `myHUD = controller+0x71C`,
  the six-bool DWORD at `myHUD+0x490` with `bHideHUD` as bit 0, all confirmed
  by back-reference (`docs/ENGINE-MAP.md`). **The DWORD read `0x00000020`
  and never changed once** across a 16-minute run and four controller
  lifetimes — the intro bathysphere descent, a level load, walking out of the
  bathysphere, the plasmid injection, combat, barrels, and both halves of a
  Little Sister sequence. Zero transitions, zero identity failures.
  **Eight marked cutscene boundaries, zero transitions at any of them.** The
  marker key was itself broken during the run (see below), but the presses were
  recovered afterwards from the unrelated `KEY: vk` keylogger in
  `Render/XRSession.cpp` — eight `vk 0x70` presses, four sequences by two ends,
  matching the tester's account exactly. So this is not merely "nothing was
  observed": every boundary the tester marked has a timestamp, and the DWORD is
  flat across all of them.
  **The decisive datapoint:** the tester made the HUD visibly appear and
  disappear by walking in and out of the bathysphere entrance, and the DWORD
  did not move. So HUD visibility on this build is **not** driven by
  `bHideHUD`. `docs/ARCHITECTURE.md` finding 1 read the corpus correctly —
  `ActionCinematicEnter`/`Exit` really do write that bool — but the retail
  sequences evidently do not run through those script actions.
  **The lead it leaves:** `HideMovie('HUD')` on the Scaleform GUI controller,
  already noted in `docs/modules/gamestate.md` for the rescue, is now the
  prime suspect for how the HUD actually hides.
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
- **Substituting a heading into the aim field at a scripted window's entry —
  falsified in a headset, 2026-08-11.** The theory was that a forced move steers
  by whatever we last left in `Controller.Rotation`, so handing it the player's
  body heading would fix the balcony landing. **A forced move steers by nothing
  of ours:** under a build that never writes the field during a sequence, three
  falls entered at *far right*, *straight on* and *far left hugging the machine*
  all landed on the same spot with the same facing. With the substitution on,
  both straight-on runs landed badly wrong — **the write itself is the damage**,
  which is the ownership invariant above. Do not re-add a write of any kind
  inside a scripted window.

### The quest arrow

- **"Leave the rotation alone and it keeps pointing at the objective" — FALSIFIED
  in a headset, 2026-08-12.** The `DriveQuestArrow` Location write works: the
  arrow parks relative to the head, stops riding the gun and stops bobbing. Its
  *rotation* still moves with the controller.

- **Cancelling the weapon's rotation per frame — FALSIFIED, and it is the pitch
  servo's lesson again.** `R->yaw -= gun.yaw` every CalcView assumed the game
  rewrites that field each tick. It does not, so the correction **compounded**;
  the probe caught arrow pitch walking `-1.1 → +44.6 → +151.1 → -103.3` within
  seconds. **A cumulative correction to a field nobody resets is a runaway.**
  `ArrowUnparentRot` ships 0. An absolute write (`ArrowLevel` zeroing roll) is
  idempotent and safe by construction; that distinction is the whole finding.

- **There is nothing to detach.** A scan of the arrow actor's first `0x400` bytes
  for any slot holding a pointer we can name found **`arrow+0x0AC` → the pawn**
  and **no weapon pointer at all**. So the gun-following is not actor parenting,
  and severing an attachment cannot be the fix.

- **SHELVED 2026-08-12, not solved.** Final observed behaviour: *"it moves with
  the controller but then it snaps back to the correct direction"* — i.e. the
  game re-asserts the right heading periodically and something drags it between
  those updates. Position, size (`ArrowDrawScale`, actor DrawScale `+0x2AC`, the
  field `GunScale` already writes) and scripted-scene suppression are all
  verified working. **Do not re-open this with another rotation write; the open
  question is what drags it between the game's own updates.**

  ⚠ **A methodology note that cost a round.** The run that produced the rotation
  data was one the tester was asked to hold *still* so the attachment scan could
  complete, and the gun barely moved in it. A tracking claim was then read out of
  that sample. **A still gun cannot answer whether something tracks the gun.**

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
