# Codemap

The index. One block per module: what it owns, which thread it runs on, and
**when to load its deep doc**. Read this to orient; load a module doc only when
the work actually touches that area.

Regenerate with `/codemap` after any structural change. **Measured 2026-08-10.**

> Line counts are advisory — they say "is this file too big to read whole", not
> "go to line N". Cite **banner text**, never a line number (`CLAUDE.md`).

---

## The shape of the thing

`BioshockVR.dll` is loaded by `dxgi.dll` beside `BioshockHD.exe`. It hooks
D3D11 and the engine camera, renders alternating stereo eyes into OpenXR,
drives head and hand transforms, captures the Scaleform HUD onto its own layer,
and synthesizes XInput from VR controllers.

**Two threads, and the distinction is load-bearing:**

| Thread | Runs | Key modules |
|---|---|---|
| Game | `eventPlayerCalcView` | `CameraHook`, `GameState`, `EngineBridge`, `HandsProbe`, `ArmHide`, `EngineExec` |
| Render | `IDXGISwapChain::Present` | `Hooks`, `XRSession`, `DrawHook`, `Swing`, `InputHook` (producer) |

They never share a loose flag. Data crosses via seqlocks or the **eye-tag FIFO**
— the tag travels *with* the frame, so a pipeline slip can't accumulate.

**Alternating-eye rendering:** the game renders one eye per Present. `XR_SubmitPair`
stashes eye 0 and submits the pair on eye 1. Healthy is ~2 Presents per submit
(e.g. 236 Present/s → 118 submits/s) and `EYEQ: depth min=1 max=1`.

```
CalcView (game thread) ── camera pose + eye tag ──▶ [FIFO] ──▶ Present (render thread)
                                                                  eye 0: stash
                                                                  eye 1: submit pair
```

---

## Layout

Source folders map 1:1 onto the module docs, so a folder name tells you which
doc to load.

```
BioshockVR/
  Core/     dllmain, Config, Keybinds        -> docs/modules/config.md
  Render/   Hooks, XRSession                 -> docs/modules/render.md
  Hud/      DrawHook                         -> docs/modules/hud.md
  Camera/   CameraHook                       -> docs/modules/camera.md
  Input/    InputHook, Swing                 -> docs/modules/input.md
  Hands/    HandsProbe, ArmHide              -> docs/modules/hands.md
  Game/     GameState, EngineExec, GameIni   -> docs/modules/gamestate.md
            EngineBridge                     -> docs/modules/enginebridge.md
OpenXRShim/src/                              -> docs/modules/shim.md
```

Includes are folder-qualified (`#include "Core/Config.h"`), so a file's
dependencies are readable without opening anything.

**Every source file on disk is listed in a `.vcxproj`** — checked this run, all
three projects. Anything not listed would not compile, which is how 1,911 lines
of stale shim duplicates once survived.

---

## Modules

### `Core/dllmain.cpp` (243) — entry point and logging
Finds a writable place for the log (harder than it sounds), and runs the init
thread that loads config, syncs the game ini and arms the hooks.
**Owns the build stamp** (`dllmain build:`) — a hand-written label plus
`__DATE__`/`__TIME__`. Update the label when you ship a change, or the stamp
will not advance for a change in another file and the log will identify the
wrong build.

### `Core/Config.h` / `Core/Config.cpp` (214 / 481) — every setting, one struct
~139 settings in `struct VrConfig`, one instance `g_cfg`, read from
`BioshockVR.ini` and echoed at startup. That echo is the authority on what
actually took effect. Replaced 138 loose globals and 161 duplicated `extern`
declarations across the consumers.
→ **`docs/modules/config.md`** when adding a setting or chasing a default mismatch.

### `Render/Hooks.cpp` (719) — D3D11 acquisition and frame orchestration
Creates a throwaway device+swapchain to read the shared vtable, MinHooks
`Present` (slot 8), and routes each frame between the stereo and mono paths.
Owns the deferred install of the draw hooks.
→ **`docs/modules/render.md`**

### `Render/XRSession.cpp` (1241) — OpenXR session, swapchains, submission
Session lifecycle, view location, projection layer, crosshair quad, HUD quad,
menu/mono path, and the per-call timing breakdown that solved the frame-pacing
work. Publishes head pose to the game thread via seqlock.
**Also owns the `KEY: vk` keylogger** in `PollFovKeys` — a 103-virtual-key sweep
that looks like noise and is not: it is the only reason M1-S2's marker presses
were recoverable after the marker key turned out to be dead. Keep a logging path
if the sweep is ever optimised.
→ **`docs/modules/render.md`**

### `Camera/CameraHook.cpp` (2525) — the camera seam ⚠️ largest file
Finds `APlayerController::eventPlayerCalcView` by an FName/string chain (never a
hardcoded RVA), hooks it, and writes the camera the engine actually renders.
Also owns the eye FIFO, the latched-pose channel, motion aim, head aim, snap
turn, `ModYaw`, the applied-shot publisher, delta clamp, and 6-DOF hand writes.
Internally sectioned by banner comments — grep for them rather than trusting
a line number: `module scan`, `the six stages`, `basis math`, `motion aim state`,
`HIDDEN PITCH SERVO`, `APPLIED SHOT DIRECTION`, `the detour`, `HEAD-AIM`,
`PAIR LOCK`, `6-DOF HANDS`, `ONE WORLD ADVANCE PER EYE PAIR`, `the FIFO API`,
`delta scan`, `install`.
→ **`docs/modules/camera.md`** for anything touching view, aim, or turning.

### `Hud/DrawHook.cpp` (1893) — draw classification and HUD capture ⚠️ second largest
Hooks `DrawIndexed`/`Draw`/instanced variants and `OMSetRenderTargets`.
Classifies draws, redirects the interface run to a private render target with
its own D24S8, repairs HUD alpha, suppresses the reticle and cutscene bars.
**Contains the fix for the duplicate-world square** (grep
`PSSrv0Res(ctx) == nullptr`) — the capture
skips any draw with a bound shader resource, because the interface is untextured
and the square was textured.
Banners: `the classifier`, `capture surfaces`, `ALPHA REPAIR`,
`texture discrimination`, `the fingerprint table`, `suppression`,
`menu detection`, `ANCHOR list`.
→ **`docs/modules/hud.md`** — mandatory before changing anything in the capture path.

### `Input/InputHook.cpp` (1200) — OpenXR actions → synthetic XInput
Producer on the render thread (one action set, synced per XR frame, published
through a seqlock); consumer is the `XInputGetState` detour. Owns hand poses,
the d-pad modifier, pause/help chords, head-relative movement, grip
threshold/hysteresis.
→ **`docs/modules/input.md`**

### `Game/GameState.cpp` (2093) — the game's own state, and the signals that work
**This file now owns the project's working scripted-event detection.** Three
Tier 0 signals, all measured, all read-only, all published through the same
interlocked channel as `g_paused`:

| Predicate | Source | Covers |
|---|---|---|
| `GameState_ScriptedAnim` | `hands+0x594` bit 2 | a scripted hand-animation sequence |
| `GameState_ForcedMove` | `controller+0x9E0` | the game interpolating the player into place |
| `GameState_Bathysphere` | `pawn+0x464` bit 1 | a bathysphere ride |

Both bit reads **fail closed on shape** — the bathysphere needs its oracle bit
clear, the forced move must read exactly 0 or 1 — after a stale pawn pointer
holding ASCII raised a false positive. Grep `GATE ON THE ORACLE`.

Also `Level.Pauser` for pause, and the old one-shot pawn scan for
`LastPlayerInputContext`. **That scan has still never locked** — zero
`>>> CONTEXT` lines in any session — so `GameState_MenuUp`/`RadialOpen`/
`ScriptedSequence`/`Cutscene` remain inert. They are a *different* mechanism from
the three above and must not be confused with them.

Also holds the two M1 probes, both **read-only, one-shot, gating nothing**:
banners `MYHUD PROBE` (locks `PlayerController.myHUD` at `+0x71C` by searched
back-reference) and `THE CINEMATIC FLAG` (reads the bool DWORD at `myHUD+0x490`,
logs transitions only). **`bHideHUD` never moves on this build** — grave 1. The
readers are kept as the reference implementation of an identity-checked engine
read. Other banners: `the scan`, `WORLD FOV CEILING`, `PAUSE / FULL-MENU
DETECTION`, `PAWN FRESHNESS`, `FLOAT SNAPSHOT / DIFF PROBE`, `FOV AUTO-DIFF`.
`GameState_Observe` is also where `EngineBridge_Tick` is driven.
→ **`docs/modules/gamestate.md`** — read before any cutscene-detection idea.

### `Game/EngineBridge.cpp` (553) — the native call bridge (Tier 1)
**Locate only. Nothing here calls into the engine.** Finds the engine's native
property accessors by their `int<Class>exec<Func>` registration symbol: the wide
string appears once in `.rdata`, exactly one 12-byte row in `.data` points at
it, and the row's second DWORD is the function pointer (**zero on disk**,
written at runtime). Three stages, not `FindCalcView`'s six, validated against
the table's own stride. One-shot on the game thread with backoff.
**M3-S1 passed 2026-08-10**: `GetPropertyTextByName` @ rva `0x7346E0` on Steam,
all four accessors, stable across two launches.
Banners: `module`, `the targets`, `stage 1`…`stage 4`, `the dump`, `the session`.
→ **`docs/modules/enginebridge.md`** before calling anything, and before
assuming the signature — it takes an `FFrame`, not a string.

### `Hands/HandsProbe.cpp` (1489) — pawn/hands/weapon discovery
Three-stage positional identification: pawn by proximity to the camera, Hands by
matching camera position *and* view rotator simultaneously. Per-weapon grip and
cursor offsets, live numpad tuning that saves back to the INI, weapon-slot and
plasmid-mode detection.
→ **`docs/modules/hands.md`**

### `Hands/ArmHide.cpp` (527) — sleeve/hand suppression, motion, whole-actor hide
Collapses ten sleeve bones to zero scale at the skeleton, leaving the 34
hand/finger bones and the weapon attachment alone. Fail-closed: two vtables
verified, skeleton ownership checked, bone count must be exactly 47.
**Bone 43 must never be touched** — telekinesis release uses it and crashes.

Also owns two things M7 depends on: `ArmHide_HandMotion` (model-space motion of
the right wrist — whether the rig is *actually animating*, which no script flag
answers) and `ArmHide_SetActorHidden` (arms, hands and weapon together via
`DrawScale3D`). **The actor hide is deliberately not a bone write:** this file
clears the dirty byte to make its writes stick, which freezes the whole array —
so hiding by bone would freeze the very bone the motion sampler reads. That
combination produced a bistable latch once already.
→ **`docs/modules/hands.md`**

### `Input/Swing.cpp` (193) — physical wrench gesture
Head-relative hand velocity → synthetic right-trigger pulse, composed with the
physical trigger. Borrows the stock melee system whole; does not simulate
collision. Gated on `HandsProbe_WeaponSlot() == 0`, so it is dead if the hands
probe is disabled.
→ **`docs/modules/hands.md`**

### `Game/EngineExec.cpp` (242) — the Unreal `Exec` seam
Runs console commands through `UGameEngine::Exec`. `set` writes the class
default, so it survives respawn, level change and save reload — which is why the
reticle kill uses it. **Now also reads**: per-slot output-device thunks, with
slot 4 answering "yes" to the accept-output query, make `get` return values.
Its proven limit is that `get` reads the **class default object**, not live
instance state — which is what `EngineBridge` exists to get past. Three absolute
addresses, INI-overridable, vtable-verified.
→ **`docs/modules/gamestate.md`**

### `Game/GameIni.cpp` (191) — game config synchronisation
Pushes FOV and viewport size into the game's `Bioshock.ini` so the mod's reported
FOV and the rendered image can never drift. Writes five keys, touches nothing else.
→ **`docs/modules/packaging.md`**

### `Core/Keybinds.cpp` (217) — rebindable keys ⚠️ **dead code, and it bites**
Complete and correct, with **zero callers — `Key_Init` is never invoked**, so
every binding resolves to VK 0 and `Key_Down`/`Key_Fired` always return false.
Every key check in the codebase is a raw `GetAsyncKeyState`, which is why the
collisions the header claims to have corrected are all still live: `VK_PRIOR`
three readers, `VK_DELETE` two, `VK_NEXT` two, `VK_NUMPAD9` two.
**This is not harmless.** M1-S2 bound its marker key through this API and got
zero presses for a full headset cycle, with no error anywhere. Bind with
`GetAsyncKeyState` until `Key_Init` has a caller. Kept because wiring it is the
fix, not deleting it — it also ships rebinding for users with no numpad.
→ **`docs/modules/input.md`**

### `OpenXRShim/src/` (1840 across 3 files + header) — OpenXR over OpenVR
A separate project producing `openxr_loader.dll`. Implements exactly the 36
OpenXR exports the mod imports (see `OpenXRShim/exports.def`), backed by
SteamVR, because SteamVR's own OpenXR runtime has no 32-bit support. Projection
layers are re-composited as textured quads at 50 m using the real HMD frustum.
Generates its own SteamVR action manifest and per-controller bindings.
→ **`docs/modules/shim.md`** — mandatory before adding any OpenXR call.

### `dxgiproxy/` (191) — the loader
Minimal `dxgi.dll` proxy: the game imports `CreateDXGIFactory1` from `DXGI.dll`,
Windows checks the exe's own folder first, so this wins and pulls in
`BioshockVR.dll` before handing the call to the real system DXGI. Loading happens
on the first export call, not in `DllMain`, because `LoadLibrary` under the
loader lock deadlocks. Writes `logs\BioshockVR_loader.log` as a breadcrumb.
→ **`docs/modules/packaging.md`**

---

## Declared but never called

Measured 2026-08-10. Nothing here is a bug on its own; each is a promise the
code does not keep, and two of them have already cost a session.

| Symbol | Why it matters |
|---|---|
| **all of `Key_*`** | `Key_Init` has no caller, so the whole keybind module is inert. See above. |
| **`GameState_Reset`** | **Nothing calls it.** So the "clear cached state at level load and save reload" story is not actually wired anywhere — the only lifetime reset that runs is the per-consumer one. Check this before trusting any "reset at boundaries" claim. |
| `GameState_ScriptedSequence`, `GameState_RadialOpen` | Providers with no consumers. "Everything downstream is built and waiting" is true of the *features*, not of these two predicates — nothing would light up even if the scan locked. |
| `EngineBridge_GetPropertyTextByName`, `EngineBridge_GetPropertyText` | **Deliberate, and the deliberateness expires.** M3-S1 ships the address with no caller on purpose — locating and calling have different risk profiles. M3-S2 is what gives them one. If S2 has come and gone and these are still uncalled, something was dropped. |
| `EngineExec_GetLastOutput` | The console read channel works; nothing reads its result programmatically. |
| `DrawHook_CutsceneBarsActive` | Cutscene-bar detection with no consumer. |
| `Hooks_Remove`, `DrawHook_Remove`, `CameraHook_Remove`, `XR_Shutdown` | **There is no teardown path at all.** The mod is never cleanly unloaded; the process just exits. Fine today, relevant the moment anything wants to re-init. |

Note when repeating this analysis: consumers **forward-declare** cross-module
functions locally (`bool GameState_Theater();` at the top of a `.cpp`) instead of
including the header, so a header-based scan over-reports. Grep the name.
`GameState_Theater` is used by four other modules and is not in `GameState.h` at all.

---

## Where to start, by symptom

| Symptom | Load |
|---|---|
| Wrong geometry, warp, eye misalignment, resolution | `render.md`, `shim.md` |
| HUD wrong size / missing / duplicated / bad alpha | `hud.md` |
| View pulled, shake, turning, aim direction | `camera.md` |
| Buttons dead, radial stuck, wrong controller mapping | `input.md`, `shim.md` |
| Weapon in the wrong place, arms stretching | `hands.md`, `ENGINE-MAP.md` |
| "Am I in a cutscene / menu / container?" | `gamestate.md` |
| Reading a live property off an engine object | **`enginebridge.md`** |
| Install, loader selection, logs, storefronts | `packaging.md` |
| Adding or trusting a memory offset | `ENGINE-MAP.md` + the `engine-offset` skill |
| Writing or reviewing code, formatting, comments | **`docs/STYLE.md`** |
| "What does the game actually do here?" | **`docs/UNREALSCRIPT.md`** — 1,765 decompiled classes in `research/uscript/` |
