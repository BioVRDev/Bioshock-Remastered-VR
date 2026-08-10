# The OpenXR-over-OpenVR shim

`OpenXRShim/` — a **separate project** producing `openxr_loader.dll`. Never link
it into `BioshockVR.dll`.

## Why it exists

SteamVR's own OpenXR runtime has no 32-bit support, and the game is 32-bit. The
shim presents the OpenXR surface the mod imports and implements it on OpenVR, so
native SteamVR headsets (Index, Vive, Vive Pro, Bigscreen Beyond, Varjo,
Somnium) work with no changes to the mod.

Headsets with a working 32-bit OpenXR runtime — Quest/Rift via Oculus, anything
via Virtual Desktop/VDXR, Pimax and WMR where available — use the standard
Khronos loader instead. `Setup.bat` selects by **copying** the chosen source over
`openxr_loader.dll`.

## The maintenance contract

> **Adding or renaming an OpenXR action in `Input_XrCreate` requires updating the
> shim's generated manifest and bindings too. Adding any new OpenXR *function*
> requires adding a shim export.**

`OpenXRShim/exports.def` is the complete surface — **36 functions**. If the mod
calls something not on that list, the mod loads fine under the real loader and
**fails to load at all under the shim**.

This has happened: a direct static import of `xrGetCurrentInteractionProfile`
added an import the shim does not export, and the mod stopped loading in shim
mode before `xrCreateInstance`. **Resolve optional functions through
`xrGetInstanceProcAddr`, or export them.**

`xrSuggestInteractionProfileBindings` is effectively a **no-op** here. Native
profile suggestions do not affect SteamVR bindings at all — those come from
string literals in `shim_input.cpp`, regenerated on every launch, which is why
hand-editing `openvr_input\*.json` never sticks.

## Geometry — the fix that took the longest

OpenXR projection layers carry an arbitrary per-view FOV and pose; OpenVR's
`Submit()` does not. So `xrEndFrame` re-composites: each view is drawn as a
textured quad into a per-eye render target using the real HMD frustum, and the
two targets are submitted. The quad sits at **50 m** (`Dq`), where a planar
homography is indistinguishable from ideal rotational reprojection.

**The vertical conversion is the one that matters:**

```cpp
float l, r, t, b;
vrSystem->GetProjectionRaw(eye, &l, &r, &t, &b);
rawL = l;  rawR = r;
rawU = b;  rawD = t;      // NOT -t / -b, and NO swap guard
```

The previous `U = -t, D = -b` mirrored the vertical principal point on vertically
asymmetric headsets, producing stretch, shrink and apparent distance changes on
head turns — while the SteamVR desktop mirror looked perfectly normal. That
mirror looking fine is exactly what sent the investigation into reprojection,
frame pacing, plane distance, head-locking and frustum staleness for several
sessions. All of those were rejected.

The swap guard must stay removed: it cannot detect a mirrored principal point,
which is the bug it would be concealing. Validate `l < r` and `D < U`, and log
the raw values before conversion.

## Resolution

Eye targets are sized from the **largest app swapchain** recorded in
`xrCreateSwapchain` (gated at ≥512×512 so the crosshair and HUD swapchains cannot
shrink it), not from OpenVR's recommended size. Using the recommendation produced
a visible ~66% downscale: the app was creating ~2750×2850 while the shim built
~1780×1908. The fixed `2750×2850` diagnostic is retired — sizing is dynamic.

## Coordinates and spaces

OpenXR and OpenVR share conventions (right-handed, +Y up, −Z forward), so poses
carry over with no axis surgery. OpenXR `LOCAL` space is emulated by latching the
first valid HMD pose (yaw + position) as the origin of everything reported.

## Controller bindings

Generated per launch into `openvr_input\`. Present: Oculus Touch (best covered,
thumbrest `rest_l`/`rest_r` confirmed working), Index/knuckles, Vive wands
(deliberately limited — too few controls), and WMR/holographic (**provisional,
never hardware-tested**).

Two known binding hazards, both predicted from the generated JSON and neither
confirmed on hardware:

- **Index**: `menu` (trackpad click) and `rest_l` (trackpad touch) share the left
  trackpad. You cannot click without touching, so every menu press arrives with
  the modifier already held. With `ControllerDpadFlip=1` or
  `ControllerDpadModifier=4`, pressing menu to pause would emit BACK
  (context help) instead of START. Touch controllers are unaffected — separate
  physical thumbrest.
- **WMR**: grip is bound as a *digital button* to `grip_l`/`grip_r`, which are
  `XR_ACTION_TYPE_FLOAT_INPUT` read through `GetAnalogActionData` and thresholded
  at `GripThreshold=0.80`. WMR grips are digital, so the action may report
  inactive or stuck at 0. Since grip opens the weapon and plasmid radials, the
  likely symptom on a Reverb G2 is **no radial at all**.

Index grip stays trigger-based; the code-side threshold plus hysteresis already
handles resting pressure. Do not also switch to `force_sensor` without testing.

## Logging

`ShimLog` writes to **`<dll folder>\logs\openxr_shim.log`**, not beside the DLL.
An absent log there is not evidence of a load failure — check the new path first.
Confirm `CollectLogs.bat` knows about it or support bundles will ship without it.

## Build

**`Release | Win32` is the only configuration that produces a DLL.** The other
three are `ConfigurationType=Application` and quietly build an `.exe`.
