# ROADMAP.md — Phases 9–13 (load on demand)

**Companion to `HANDOFF.md`. Ask for this at a phase boundary. Phase 8 (the strip)
is described in `HANDOFF.md` §1 — it's the current phase and doesn't need this file.**

**Order matters. Each phase is one chat. Commit + push at every working checkpoint.**

\---

## Phase 9 — SQUARE RENDER (kills the black bars)

**Zero mod code changes.** This is a config change plus verification.

**Why FOV cannot fix this:** `Bioshock.ini` has `HorizontalFOVLock=True`. The slider
sets HORIZONTAL fov; vertical is *derived from the aspect ratio*
(`vFov = 2·atan(tan(hFov/2) × h/w)`). At 16:9 the vertical is permanently squashed.
Raising the slider just widens an already-wide image. **The Quest 3 eye is \~94°×99°
— nearly square. Feeding it 16:9 IS the bars.** (Full derivation in `SPEC.md` §8.)

**The fix:** force the game to **1440×1440** via `Bioshock.ini`. With the slider at
100 that gives **100°h × 100°v** — overfills the Quest in both axes.

**It is free:** 1920×1080 = 2.07 MP; 1440×1440 = 2.07 MP. Identical pixel count —
you're moving pixels from the sides (wasted) to the top and bottom (empty). Same GPU
cost.

`XR\_SetGameFov` already derives vertical from `h/w`, and `g\_bbW/g\_bbH` are measured
from the real swapchain. It adapts automatically.

**Test:** log reads `backbuffer : 1440 x 1440` and `GAME FOV = 100.0 h / 100.0 v`.
Bars gone, top and bottom. Set `GameFovDegrees` to match the in-game slider.

**Watch for:** the Scaleform HUD is laid out for 16:9 and may sit oddly. It gets
suppressed in Phase 13 anyway.

\---

## Phase 10 — MIRROR THROTTLE (removes the 240Hz hardware requirement)

**This is required before release.**

**The problem:** AER needs 2 Presents per compositor frame, but DWM caps Present at
the desktop refresh (`PERF.md` §1). So today the mod only works correctly on a
desktop ≥2× the headset refresh. **Most people have 144Hz. Many have 60Hz.**

**The fix: stop letting DWM see most of our Presents.**

`origPresent` blocks only because DWM won't accept frames faster than it composites.
But **nobody is looking at the monitor — they're wearing a headset.** We don't need
to mirror at 236fps.

In `hkPresent`, call the real `Present` **on a timer**:

```
if (now - lastRealPresent >= mirrorPeriod)  → call g\_origPresent   (mirror updates)
else                                        → return S\_OK without presenting
```

With `MirrorFps=60`, DWM only ever sees 60 calls/sec — **under any monitor's cap**,
so it never throttles. The other \~176 game frames never touch DWM at all.

**Then the ONLY thing pacing the game is `xrEndFrame`**, which blocks once per pair
at the compositor rate. The game naturally settles at exactly **2 game frames per
compositor frame. 236 Present/s. On any monitor.**

It also **self-limits**: no 690fps runaway into the physics-instability zone
(`PERF.md` §7), because the XR cycle is the metronome.

**Budget:** 2 game frames (3.0ms) + XR (3.2ms) = 6.2ms, inside the 8.33ms period.

**New ini keys:** `MirrorFps=60`, `SkipFlatPresent=1` (kill switch, **default OFF
until proven**).

**Risks, each with a log check:**

* Frozen desktop window → the timer keeps it alive.
* Missing GPU flush (Present normally flushes) → may need an explicit `ctx->Flush()`.
The XR copies probably cover it; verify.
* The game might use Present for its own timing (unlikely in UE2.5 — it'll be on
`QueryPerformanceCounter`) → that's what the kill switch is for.

**THE TEST, and it is unambiguous:** turn it on, **set the desktop to 120Hz on
purpose**, and confirm you still get **236 Present/s**. If you do, the mod works on
anyone's hardware, with no VD setting to change.

<b>~~Hook swapchain creation, force flip-model + ALLOW\_TEARING~~ — REJECTED.</b> See
`PERF.md` §4.

\---

## Phase 11 — HEAD TRACKING

**The single biggest remaining source of discomfort.** The image is currently welded
to your skull: turn your head and the world turns with you. **It contaminates every
judgment about stereo quality** — do not evaluate any remaining AER weirdness until
this lands.

Write `\*CameraRotation = ApplyWorldSpaceYaw(clean, hmdYaw, hmdPitch, hmdRoll)` on
**site0 only**. Math in `SPEC.md` §5–6. **Ask for `SPEC.md` and `CameraHook.cpp`
for this phase.**

**Aim stays decoupled for free** — gameplay reads `APlayerController.Rotation`,
which we never touch. Head-look won't move where the gun shoots. This is the
foundation for motion controls later.

### THE RULE THAT IS EASY TO GET WRONG

**Sample the HMD pose ONCE per eye-pair for the CAMERA write.** If you re-sample per
game frame, a head turn between the two frames of a pair renders the eyes from
*different head rotations* — a real stereo mismatch your brain fights.

**The composition-LAYER pose stays fresh on every submit** (that's what keeps
timewarp smooth). **Do not conflate the two.**

Remember `SPEC.md`: roll is **inverted** (`-roll`), and the eye offset goes along
the **FINAL** (head-rotated) right vector, not the clean one.

\---

## Phase 12 — TRUE DUAL-RENDER. It just got CHEAP.

**This was previously filed as "later, only if needed, risky." The economics have
completely changed. Treat it as a near-term phase.**

The blocker was always budget: dual-render needs two full game frames inside one
compositor period. We now know a game frame costs **1.5ms**, so two cost **3.0ms**
inside an 8.33ms period.

**The current pair cadence ALREADY IS the dual-render pipeline** — two engine-native
eye renders per compositor period, with \~5ms to spare. It's running right now.

The only thing separating us from true stereo: **the world clock advances 4.2ms
between the two frames of a pair.**

**The work: ONE hook** on the engine's tick/frame-delta, **forced to 0 on the second
Present of each pair** (the eye-tag FIFO already tells you which one that is).
Temporal disparity → **exactly zero**. The eyes then differ by nothing but the 6.4cm
IPD offset — which is the literal definition of true stereo.

**Zero framerate cost.** Still 236 Present/s, 118 submits/s, 118 unique frames per
eye.

**Bonus:** physics then advances 118×/sec at 8.5ms steps instead of 236×/sec at
4.2ms steps — **further** from the known high-FPS instability zone (`PERF.md` §7),
not closer.

**Risks — real, hence its own phase, hence kill switches:** divide-by-zero in engine
code, frozen physics/animation, input sampling effectively halving to 118Hz.

\---

## Phase 13 — HUD SUPPRESSION

Hook `ID3D11DeviceContext` vtable\[12] (`DrawIndexed`) and vtable\[13] (`Draw`).
Identify the Scaleform HUD by draw-call vertex counts:

```
DrawIndexed(234) = compass
Draw(11)         = health / EVE
Draw(9)          = gun reticle
Draw(21)         = plasmid reticle
```

**Gate the suppression:** only drop `Draw(9)` / `Draw(21)` when a HUD-active flag is
set (raised by a `234` or `11` draw, cleared each Present) — otherwise you eat world
particles that happen to share those vertex counts.

Ask for `SPEC.md` and `Hooks.cpp`.

\---

## Backlog (not phases yet)

* **Loading screens / movies run \~15fps in-headset.** During movies the game only
Presents \~30/s, and the pair cadence submits on every second Present. Not a bug —
the pair cadence applied to a 30fps source. **Clean fix:** fall back to
mono-every-Present when the camera hook isn't ticking (we already detect this —
`EYEQ underruns`).
* **Motion controls.** The aim/view decoupling (`SPEC.md` §3) is the foundation.
* **Window centering.** BSR launches top-left on an ultrawide; itsloopyo centers it
on first Present. Cosmetic.

