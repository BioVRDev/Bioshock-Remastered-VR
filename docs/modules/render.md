# Rendering, frame flow, and OpenXR submission

`Hooks.cpp` (825) and `XRSession.cpp` (1443).

## Frame flow

`Hooks.cpp` creates a throwaway D3D11 device and swapchain to read the shared
vtable, then MinHooks `IDXGISwapChain::Present` (slot 8). On the first Present it
installs the camera hook and the draw hooks — never from `DllMain`, and never
before the executable has unpacked.

The game renders **one eye per Present**. `XR_SubmitPair(image, eye)`:

- `eye 0` — stash this backbuffer, no XR cycle.
- `eye 1` — run one full XR cycle, submitting (stashed left, this right).

So ~236 Present/s in, ~118 XR submits/s out. The one-Present (~4.2 ms) disparity
between the two images **is** the stereo. The eye value comes from
`CameraHook_NextEye()`, popped from the FIFO — it is not a shared flag.

`XR_SubmitMenuMono(image)` is the separate path for when the camera hook is
starved (`CameraHook_Starved()` — no view for >250 ms), i.e. the game is not
rendering a world: menus, loading, movies. One mono frame per Present on a
world-locked quad. `XR_ResetMenuAnchor()` drops the anchor so the next such frame
takes a fresh one; it is called from Present, which owns the anchor.

A normal XR frame may carry a stereo projection layer, a crosshair quad, and a
HUD quad.

## Health signals

| Signal | Healthy | Meaning |
|---|---|---|
| `frames … submitted … state 5` | ~236 / ~118, state 5 | focused, correct cadence |
| `EYEQ: depth min=1 max=1` | min == max | the two threads are in lockstep |
| `EYEQ` min = 0 in gameplay | — | tags dropped or consumed out of step |
| `PER PRESENT` game time | 2.2–2.9 ms | above ~4 ms, look at the scene, not the mod |

`XR_Breakdown()` reports milliseconds inside each individual OpenXR call —
`waitFrame`, `beginFrame`, `locateViews`, `acquire`, `copy`, `endFrame`. **This is
not debug cruft**; it is what solved the frame-pacing work. It stays.

When investigating a performance report, separate `game` from `XR` first. If `XR`
is unchanged and `game` rose, the mod is not the cause — and same-spot,
same-save comparison is the only meaningful test, since two runs in different
locations differ for unrelated reasons.

## Desktop mirror

Windowed BitBlt presentation is throttled by DWM to monitor refresh, and AER
needs two game Presents per headset frame — so a 60 Hz monitor halves VR rate.
Measured on 60 Hz:

| Policy | Present/s | XR submits/s |
|---|---:|---:|
| Present every game frame | 60 | 30 |
| Never call original Present | 85 | 42 |
| Present every 4th game frame | 240 | 120 |

Never presenting is *worse* — the driver loses a useful frame boundary and
serializes work. `MirrorPresentEvery=0` therefore means **time-based**: present
the desktop at most once per ~17 ms. Positive values keep a fixed divisor for
debugging.

Exclusive fullscreen is not an acceptable fix. It snaps the portrait VR render
request to a real monitor mode, changes projection calibration, and ties
behaviour to the user's display.

## Pose consistency

The projection layer carries the **latched pose the image was rendered from**
(`CameraHook_GetLatchedPose`), not the freshest pose at submit time. This was a
major flicker fix — do not "improve" it by sampling a newer pose.

Head pose is published render→game through a seqlock (`XR_GetHeadQuat`,
`XR_GetHeadPos`, OpenXR LOCAL space). Hand poses ride the same pattern in
`InputHook`, in the *same* space, so head and hands are directly comparable
without conversion.

## Crosshair

`CrosshairFromShot=1` consumes the exact applied shot direction published by
`CameraHook`, rather than recomputing aim on the render thread from a newer
controller pose (which bypassed game-thread clamp and smoothing and drifted from
the shot). The point is placed in world space using the latched pose and the quad
billboards toward the current eye midpoint — a head-locked `g_viewSpace` dot
slides against the world when the runtime applies it relative to a newer head pose.

Remaining limits, both physical: a fixed `CrosshairDistance` cannot align at every
depth, and one dot cannot represent weapon spread. The complete answer is a
published shot ray plus an optional scene trace.

## Known cruft

`PollFovKeys()` sweeps virtual keys `0x21`–`0x87` (103 codes) **every XR frame**,
from both the stereo (`:636`) and mono (`:1195`) call sites. It is edge-throttled
for logging but the sweep itself is unconditional. It also owns `VK_DELETE` for
cycling the HUD-quad edit parameter, which now collides with the `GETTEST` probe
in `GameState.cpp`. Wiring `Keybinds.cpp` is the fix for both.
