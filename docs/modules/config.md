# Configuration

`dllmain.cpp` (909) — entry point, logging, and the config system.

## How it works today

Roughly **130 settings**. Each one exists in three places:

1. A non-static global in `dllmain.cpp`, with its compiled default
   (`bool g_cfgHudRedirect = true;`).
2. A read in the load block
   (`g_cfgHudRedirect = CfgInt("HudRedirect", 1);`).
3. A line in the grouped startup echo.

Consumers then re-declare each global as a loose `extern` at the top of their own
`.cpp`:

| File | `extern` declarations |
|---|---:|
| `CameraHook.cpp` | 35 |
| `InputHook.cpp` | 23 |
| `DrawHook.cpp` | 23 |
| `HandsProbe.cpp` | 22 |
| `XRSession.cpp` | 19 |
| `Swing.cpp` | 15 |
| `Hooks.cpp` | 14 |

Adding a setting means editing three or four files, and nothing verifies the
`extern` matches the definition across translation units.

**This has already produced a live bug**: `ControllerLayout` is defined as `1`
(`dllmain.cpp:130`) but read with a default of `0` (line 613). Two different
answers to "what is the default". The shipped INI sets it explicitly, so only
users whose INI lacks the key are affected — which is precisely the kind of
defect that survives for a long time.

`CameraHook.cpp` also carries a duplicate `extern int g_cfgModYaw;`. Legal C++,
and harmless, but symptomatic.

## Readers

- `CfgBool(key, def)` — `GetPrivateProfileIntA != 0`
- `CfgInt(key, def)` — no clamp. **Prefer `CfgIntRange`**: an out-of-range typo
  falls through every branch silently.
- `CfgIntRange(key, def, lo, hi)` — clamps and logs.
- `CfgFloat(key, cur, lo, hi)` — note the signature is *(name, current, min, max)*,
  not *(name, default, …)*.

## The echo is the authority

`BioshockVR.log` opens with a `=== BioshockVR config ===` block echoing every
value actually read. **If a change is not in that echo, it did not take.** Check
there before drawing any conclusion from what you see in the headset — read-only
INIs, the wrong storefront profile, and VirtualStore redirection have all
silently defeated edits.

This echo is also the verification tool for the config refactor: capture it
before and after, and diff. Identical output proves all ~130 settings resolve to
the same values through the new structure.

## Planned shape

- `Config.h` — `struct VrConfig { … };` grouped by subsystem, plus
  `extern VrConfig g_cfg;`
- `Config.cpp` — the `Cfg*` readers, `Config_Load()`, `Config_Echo()`
- consumers — delete the `extern` block, `#include "Config.h"`,
  `g_cfgHudRedirect` → `g_cfg.hudRedirect`

Mechanical and type-checked by the compiler. It is also the project's first
shared contract between modules, and the natural place to hang a later
subsystem-lifecycle layer.

## Settings worth knowing

Defaults that are load-bearing, with the reasoning in `docs/INVARIANTS.md`:

| Key | Value | Why |
|---|---|---|
| `ControllerMode` | `1` | mode 0 lets any XInput device silently kill VR input |
| `PitchServo` | `0` | runaway feedback loop; froze the view and hand |
| `CrosshairFromShot` | `1` | consumes the applied shot direction |
| `HudRedirect` | `1` | the square is fixed structurally; do not disable the HUD |
| `HudAlphaFix` | `1` | `0` makes every interface element too transparent |
| `MirrorPresentEvery` | `0` | means *time-based* (~17 ms), not "never" |
| `ModYaw` | `0` | breaks scripted movement — see `docs/modules/camera.md` |
| `FreezeGameRotation` | `0` | inert without `ModYaw` |
| `HideInactiveHand2/5` | `0` | two-handed weapons keep both hands |

Live-tuning keys write their results back into the INI, so the file is not purely
input. Per-weapon grip offsets are tuned on the numpad in-headset and saved.
