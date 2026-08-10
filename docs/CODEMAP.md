# Codemap

The index. One block per module: what it owns, which thread it runs on, and
**when to load its deep doc**. Read this to orient; load a module doc only when
the work actually touches that area.

Regenerate with `/codemap` after any structural change.

---

## The shape of the thing

`BioshockVR.dll` is loaded by `dxgi.dll` beside `BioshockHD.exe`. It hooks
D3D11 and the engine camera, renders alternating stereo eyes into OpenXR,
drives head and hand transforms, captures the Scaleform HUD onto its own layer,
and synthesizes XInput from VR controllers.

**Two threads, and the distinction is load-bearing:**

| Thread | Runs | Key modules |
|---|---|---|
| Game | `eventPlayerCalcView` | `CameraHook`, `GameState`, `HandsProbe`, `ArmHide`, `EngineExec` |
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
OpenXRShim/src/                              -> docs/modules/shim.md
```

Includes are folder-qualified (`#include "Core/Config.h"`), so a file's
dependencies are readable without opening anything.

---

## Modules

### `Core/dllmain.cpp` (297) — entry point and logging
Finds a writable place for the log (harder than it sounds), and runs the init
thread that loads config, syncs the game ini and arms the hooks.

### `Core/Config.h` / `Core/Config.cpp` (206 / 494) — every setting, one struct
138 settings in `struct VrConfig`, one instance `g_cfg`, read from
`BioshockVR.ini` and echoed at startup. That echo is the authority on what
actually took effect. Replaced 138 loose globals and 161 duplicated `extern`
declarations across the consumers.
→ **`docs/modules/config.md`** when adding a setting or chasing a default mismatch.

### `Render/Hooks.cpp` (825) — D3D11 acquisition and frame orchestration
Creates a throwaway device+swapchain to read the shared vtable, MinHooks
`Present` (slot 8), and routes each frame between the stereo and mono paths.
Owns the deferred install of the draw hooks.
→ **`docs/modules/render.md`**

### `Render/XRSession.cpp` (1443) — OpenXR session, swapchains, submission
Session lifecycle, view location, projection layer, crosshair quad, HUD quad,
menu/mono path, and the per-call timing breakdown that solved the frame-pacing
work. Publishes head pose to the game thread via seqlock.
→ **`docs/modules/render.md`**

### `Camera/CameraHook.cpp` (2615) — the camera seam ⚠️ largest file
Finds `APlayerController::eventPlayerCalcView` by an FName/string chain (never a
hardcoded RVA), hooks it, and writes the camera the engine actually renders.
Also owns the eye FIFO, the latched-pose channel, motion aim, head aim, snap
turn, `ModYaw`, the applied-shot publisher, delta clamp, and 6-DOF hand writes.
Internally sectioned: scan `:256–441` · aim `:476–836` · hands `:882–946` ·
delta `:947` · FIFO API `:2340` · install `:2530`.
→ **`docs/modules/camera.md`** for anything touching view, aim, or turning.

### `Hud/DrawHook.cpp` (2174) — draw classification and HUD capture ⚠️ second largest
Hooks `DrawIndexed`/`Draw`/instanced variants and `OMSetRenderTargets`.
Classifies draws, redirects the interface run to a private render target with
its own D24S8, repairs HUD alpha, suppresses the reticle and cutscene bars.
**Contains the fix for the duplicate-world square at `:1425`** — the capture
skips any draw with a bound shader resource, because the interface is untextured
and the square was textured.
→ **`docs/modules/hud.md`** — mandatory before changing anything in the capture path.

### `Input/InputHook.cpp` (1362) — OpenXR actions → synthetic XInput
Producer on the render thread (one action set, synced per XR frame, published
through a seqlock); consumer is the `XInputGetState` detour. Owns hand poses,
the d-pad modifier, pause/help chords, head-relative movement, grip
threshold/hysteresis.
→ **`docs/modules/input.md`**

### `Game/GameState.cpp` (1178) — the game's own UI/context state
`Level.Pauser` for pause, plus a one-shot scan of the pawn for the
`LastPlayerInputContext` FString and a `kContexts` classification table.
**The scan has never locked** — zero `>>> CONTEXT` lines in any session — so
`GameState_MenuUp`/`RadialOpen`/`ScriptedSequence`/`Cutscene` are effectively
inert. Everything downstream of cutscene detection is built and waiting.
→ **`docs/modules/gamestate.md`** — read before any cutscene-detection idea.

### `Hands/HandsProbe.cpp` (1732) — pawn/hands/weapon discovery
Three-stage positional identification: pawn by proximity to the camera, Hands by
matching camera position *and* view rotator simultaneously. Per-weapon grip and
cursor offsets, live numpad tuning that saves back to the INI, weapon-slot and
plasmid-mode detection.
→ **`docs/modules/hands.md`**

### `Hands/ArmHide.cpp` (507) — sleeve and inactive-hand suppression
Collapses ten sleeve bones to zero scale at the skeleton, leaving the 34
hand/finger bones and the weapon attachment alone. Fail-closed: two vtables
verified, skeleton ownership checked, bone count must be exactly 47.
**Bone 43 must never be touched** — telekinesis release uses it and crashes.
→ **`docs/modules/hands.md`**

### `Input/Swing.cpp` (232) — physical wrench gesture
Head-relative hand velocity → synthetic right-trigger pulse, composed with the
physical trigger. Borrows the stock melee system whole; does not simulate
collision. Gated on `HandsProbe_WeaponSlot() == 0`, so it is dead if the hands
probe is disabled.
→ **`docs/modules/hands.md`**

### `Game/EngineExec.cpp` (279) — the Unreal `Exec` seam
Runs console commands through `UGameEngine::Exec`. `set` writes the class
default, so it survives respawn, level change and save reload — which is why the
reticle kill uses it. **Now also reads**: per-slot output-device thunks, with
slot 4 answering "yes" to the accept-output query, make `get` return values.
Its proven limit is that `get` reads the **class default object**, not live
instance state. Three absolute addresses, INI-overridable, vtable-verified.
→ **`docs/modules/gamestate.md`**

### `Game/GameIni.cpp` (218) — game config synchronisation
Pushes FOV and viewport size into the game's `Bioshock.ini` so the mod's reported
FOV and the rendered image can never drift. Writes five keys, touches nothing else.
→ **`docs/modules/packaging.md`**

### `Core/Keybinds.cpp` (219) — rebindable keys ⚠️ **dead code**
Complete and correct, with **zero callers**. Every key check in the codebase is
a raw `GetAsyncKeyState`, which is why `VK_PRIOR` has three independent readers
and `VK_DELETE` has two. Kept because wiring it is the fix, not deleting it.
→ **`docs/modules/input.md`**

### `OpenXRShim/src/` (1861 across 3 files + header) — OpenXR over OpenVR
A separate project producing `openxr_loader.dll`. Implements exactly the 36
OpenXR exports the mod imports (see `OpenXRShim/exports.def`), backed by
SteamVR, because SteamVR's own OpenXR runtime has no 32-bit support. Projection
layers are re-composited as textured quads at 50 m using the real HMD frustum.
Generates its own SteamVR action manifest and per-controller bindings.
→ **`docs/modules/shim.md`** — mandatory before adding any OpenXR call.

### `dxgiproxy/` (156) — the loader
Minimal `dxgi.dll` proxy: the game imports `CreateDXGIFactory1` from `DXGI.dll`,
Windows checks the exe's own folder first, so this wins and pulls in
`BioshockVR.dll` before handing the call to the real system DXGI. Loading happens
on the first export call, not in `DllMain`, because `LoadLibrary` under the
loader lock deadlocks. Writes `logs\BioshockVR_loader.log` as a breadcrumb.
→ **`docs/modules/packaging.md`**

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
| Install, loader selection, logs, storefronts | `packaging.md` |
| Adding or trusting a memory offset | `ENGINE-MAP.md` + the `engine-offset` skill |
| Writing or reviewing code, formatting, comments | **`docs/STYLE.md`** |
| "What does the game actually do here?" | **`docs/UNREALSCRIPT.md`** — 1,765 decompiled classes in `research/uscript/` |
