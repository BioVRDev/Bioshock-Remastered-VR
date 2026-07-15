# PERF.md — Performance Model & Dead-Theory Graveyard (load on demand)

**Companion to `HANDOFF.md`. Ask for this the moment ANY framerate number looks
wrong. Read the graveyard (§3) BEFORE proposing a cause — three plausible,
confident, wrong theories already cost this project four hours.**

---

## 1. THE ROOT CAUSE OF EVERYTHING

**Virtual Desktop silently drops the Windows desktop from 240Hz to 120Hz while
streaming.** (VD setting: resolution/refresh sync. Now DISABLED — which also
stopped VD letterboxing my 32:9 ultrawide into 16:9 with black bars.)

**And the game's swapchain is blt-model** (`BufferCount=1`, `SWAP_EFFECT_DISCARD`,
windowed). **DWM throttles a blt-model windowed swapchain to the desktop
composition rate regardless of `SyncInterval`.** Setting `SyncInterval=0` stops the
*game* waiting on vblank; it does **not** stop *DWM* refusing to accept frames
faster than it composites. You only escape that with flip-model +
`ALLOW_TEARING` — see §4.

> ### **Present rate == desktop refresh rate. Always.**

| Desktop | Present/s |
|---|---|
| 240Hz (no VD) | **236** |
| 120Hz (VD streaming with sync on) | **118** |

That single fact explains **every** framerate number ever measured on this project
— including the old handoff's "236fps uncapped," which was never uncapped. It was
the 240Hz monitor.

---

## 2. MEASURED TRUTH

All from `XRMode=0` runs (XR completely absent, so nothing is hidden):

```
no VD, 240Hz desktop:   game 1.44ms | XR 0.00 | origPresent 2.78ms  →  236 Present/s
VD on, 120Hz desktop:   (same game time)                            →  118 Present/s
```

- **The game renders a frame in ~1.4–1.5ms.** It is capable of **~690fps**.
  **The game has NEVER been the bottleneck. Not once.**
- **`origPresent` blocking 2.78ms is DWM**, doing nothing but waiting for the
  composition clock. That one number was the entire bug.

With XR active and the pair cadence (submit every 2nd Present), on a 240Hz desktop:
```
frames: ~236 Present/s   submitted: ~118/s
PER PRESENT: game 1.50 | XR 1.61 | origPresent 1.14   (ms)
PER SUBMIT:  wait 0.05 | begin 0.00 | locate 0.00 | acquire 0.00 | copy 0.00 | end 3.16
```
Budget: 2 game frames (3.0ms) + XR (3.2ms) = 6.2ms, inside the 8.33ms compositor
period. Fits comfortably.

---

## 3. DEAD THEORIES — DO NOT RESURRECT ANY OF THESE

Each was proposed, pursued, and **disproven by measurement.**

❌ **"vsync is the cap."**
The game DOES present with `SyncInterval=1` (source: `Bioshock.ini` →
`[D3DDrv11.D3DRenderDevice11] UseVSync=True` and `[ShockGame.ShockUserSettings]
VSync=True`, both True regardless of what the in-game menu shows). We forced
`SyncInterval=0` in the Present hook. **Nothing changed.** Keep the override — the
monitor is meaningless in VR — but it is not the limiter.

❌ **"`xrEndFrame` is the cap."**
It blocks (6.9ms), but it only **absorbs slack DWM already imposed**. Proof: in the
every-2nd-Present mode, `xrEndFrame` gave up 6.9ms and **DXGI's `Present` instantly
absorbed 3.2ms of it** to land back on the identical 8.4ms total. Two different
mechanisms converging on the same number = something downstream is the metronome.

❌ **"`xrWaitFrame` is the cap."**
Measured **0.00 ms** for entire sessions. It wasn't blocking at all.

❌ **"the GPU / VD's encoder is the cap."**
With XR **entirely absent** (`XRMode=0`) and VD streaming: still 118 Present/s.

❌ **"the game is GPU- or CPU-bound."**
It renders a frame in 1.44ms.

❌ **"move the XR frame loop onto its own thread to escape OpenXR pacing."**
OpenXR was never pacing us. `XRMode=0` proved it: no XR at all, still 118. This
would have gained **exactly zero** and cost a session of D3D11 thread-safety work.

❌ **"the every-2nd-Present cadence can't work / must be abandoned."**
It works perfectly — **once the DWM cap is gone.** The original plan was right about
the physics all along; it just never knew the headset was halving the machine
underneath it.

---

## 4. `ALLOW_TEARING` / flip-model — REJECTED (for now)

Hooking swapchain creation to force `DXGI_SWAP_EFFECT_FLIP_DISCARD` + `BufferCount=2`
+ `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` *would* break the DWM leash — **but only if
Windows grants independent flip**, which generally requires the window to cover the
whole output with nothing composited on top.

The game runs **windowed 1920×1080 on a 5120×1440 ultrawide.** It will almost
certainly stay in *composed* flip — DWM back in the loop, same cap, new swapchain.

**The mirror throttle (see `ROADMAP.md`) is the better answer:** simpler, doesn't
touch swapchain creation, doesn't depend on driver eligibility.

---

## 5. THE CURRENT CADENCE REQUIREMENT

AER needs **2 Presents per compositor frame** — one per eye.

> **Desktop refresh must be ≥ 2× the headset refresh.**

| Desktop | Headset (VD setting) | Works? |
|---|---|---|
| 240Hz | 120Hz | ✅ (my machine) |
| 144Hz | 72Hz | ✅ |
| 165Hz | 80Hz | ✅ |
| 120Hz | 120Hz | ❌ half per-eye framerate |
| 60Hz | anything | ❌ |

**This is a hardware dependency and MUST NOT survive to release.** Most people have
144Hz; many have 60Hz. **The mirror throttle removes it entirely** — see
`ROADMAP.md` §2.

---

## 6. AER — WHAT IT COSTS

The eyes alternate per Present. Present N renders the LEFT eye, Present N+1 renders
the RIGHT eye, then we submit both, fresh, as one stereo pair.

- **118 unique frames per eye per second.**
- **4.2ms inter-eye temporal disparity** (one Present). The two eyes differ by the
  IPD offset PLUS 4.2ms of world motion.
- **4× better than Luke Ross's ~16.6ms@60fps**, which people play for hours.

**The test:** strafe down a tight corridor past close geometry — the worst case for
temporal disparity.

### Why alternating-stale is worse than fresh-pair (a real artifact, measured)
An earlier mode ran a full XR cycle every Present and let each eye's *image* refresh
only every other frame. That flips **which** eye is stale every frame, so the
inter-eye time offset **flips sign 118×/sec.** During a turn or a strafe that reads
as the world **shimmering in and out of depth** — felt, but hard to point at. The
current pair cadence avoids it: both eyes are always fresh, one Present apart,
constant sign.

---

## 7. KNOWN HIGH-FPS ENGINE ISSUES

BioShock has documented physics/animation instability above 60fps (ragdoll "death
jump" bug, first-person animation stutter on high-refresh monitors; the original
capped physics to 30fps, the remaster raised it to 60).

We run at 236fps — **but so did the game before the mod**, so this is not new.

**DO NOT UNCAP THE FRAMERATE FURTHER.** We need exactly 2 Presents per compositor
frame and not one more. Everything above that is wasted GPU, extra encoder
contention, and a walk deeper into the instability zone for **zero** benefit — we
only consume 2 frames per submit.

A Nexus mod ("BioShock Remastered Stability Patches And Bugs Fixes") targets
high-FPS physics instability via INI/map edits and is worth evaluating as a
recommended companion — it edits configs and maps, so it should not conflict with a
DLL.

**Bonus:** true dual-render (freezing the tick delta on the second frame of each
pair) makes physics advance 118×/sec at 8.5ms steps instead of 236×/sec at 4.2ms
steps — **further** from the instability zone, not closer.

---

## 8. THE LESSON — the most important line in this project

**When a number doesn't move, INSTRUMENT. Do not theorise.**

Four hours were lost to three consecutive plausible, confident, wrong theories. The
answer came from a **30-second run with the suspect feature turned off entirely**
(`XRMode=0`), plus **timers around individual API calls** — which put the whole bug
in one number: `origPresent 2.78ms`.

**The best experiments DISABLE something and see what changes.** Keep the timing
instrumentation (`PER PRESENT` / `PER SUBMIT` heartbeats) in the code forever. It
looks like debug cruft. It is the only window into runtime, and it is the single
thing that solved this.
