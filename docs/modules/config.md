# Configuration

`Config.h` (206) and `Config.cpp` (494).

## How it works

**138 settings**, one struct. Each exists in three places, all now in two files:

1. A field in `struct VrConfig` (`Config.h`), carrying its compiled default:
   `bool hudRedirect = true;`
2. A read in `Config_Load()`: `g_cfg.hudRedirect = CfgBool("HudRedirect", true);`
3. A line in the grouped startup echo.

Consumers `#include "Config.h"` and say `g_cfg.hudRedirect`.

**Field names are mechanical**: `g_cfgHudRedirect` → `g_cfg.hudRedirect`. Strip
the prefix, lowercase the first letter, change nothing else. One exception,
`g_cfg6DofHands` → `g_cfg.sixDofHands`, because an identifier cannot start with a
digit.

## What this replaced

138 loose globals in `dllmain.cpp`, re-declared as **161 `extern` lines** across
the consumers — 35 in `CameraHook.cpp`, 23 each in `InputHook.cpp` and
`DrawHook.cpp`, 22 in `HandsProbe.cpp`, 19 in `XRSession.cpp`. Adding a setting
meant editing three or four files, and nothing checked that a consumer's `extern`
matched the definition across translation units.

**It had produced a live bug.** `ControllerLayout` was defined as `1` while its
INI read defaulted to `0` — two different answers to "what is the default",
affecting any user whose INI lacked the key. Consolidating the declarations is
what made it visible. It is now `1` in both places, and is the only intentional
behaviour change in the refactor (`.planning/DECISIONS.md`).

`dllmain.cpp` went from 909 lines to 297 and is now just the entry point, the
log-path search, and the init thread.

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

## How the refactor was verified

Not by reading it. Three checks, all mechanical:

1. **The key/default/range table.** Every `Cfg*("Key", default, lo, hi)` call was
   extracted before and after and diffed with the rename normalised away. Across
   all 114 reads, exactly **one line** differed — the intentional
   `ControllerLayout` fix.
2. **The echo.** All 72 `CfgEcho` lines byte-identical.
3. **The count.** 138 struct fields, 138 original globals. Nothing lost or added.

Plus a clean rebuild of the pre-refactor tree in a throwaway worktree to confirm
the two pre-existing `C4244` warnings in `CameraHook.cpp` were not introduced by
the change. **Zero new warnings.**

The remaining runtime check is the startup echo from a real run — capture the
`=== BioshockVR config ===` block and compare against the pre-refactor baseline.

This struct is the project's first shared contract between modules, and the
natural place to hang a later subsystem-lifecycle layer.

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
