---
name: log-triage
description: Read BioshockVR.log or openxr_shim.log in the correct diagnostic order and interpret the health signals. Use whenever the user shares a log, reports a bug from a play session, or asks why something did not work.
---

# Log triage

## Where the logs are

**Read them off disk yourself — never ask the user to paste a log.** The mod
truncates `BioshockVR.log` at startup, so it always contains exactly one run.

| File | Location |
|---|---|
| `BioshockVR.log` | `Build\Final\logs\` |
| `openxr_shim.log` | **`Build\Final\logs\`** — moved; not beside the DLL |
| `setup.log` | `Build\Final\logs\` |
| `Bioshock.log` | game profile folder — **only opened at shutdown**, so it can never contain anything from a live session. Do not ask for it. |

An absent `openxr_shim.log` beside the DLL is not evidence of a load failure —
check `logs\` first.

## Read in this order

Stop at the first thing that is wrong; later lines are meaningless if an earlier
stage failed.

| Line | What it tells you |
|---|---|
| `BioshockVR version:` | release identity |
| `dllmain build:` | **proves the new binary loaded.** Check this first, always. |
| `=== BioshockVR config ===` | the authority on what settings took effect |
| `camera: BioshockHD.exe base … size` | storefront/build fingerprint |
| `gameini:` | selected profile, FOV/resolution sync |
| `backbuffer:` vs viewport | the game honoured the requested dimensions |
| `windowed: NO (exclusive)` | wrong, and it changes projection calibration |
| `>>> XR: runtime =` | native runtime or shim |
| XR session state sequence | visibility/focus lifecycle |
| camera scan `STAGE` lines | camera hook discovery |
| `delta:` | fixed offset, scan result, or refusal |
| `!!! ARMHIDE:` | real vtable/build behaviour |
| `frames: … submitted … state 5` | focused, cadence, pair ratio |
| `EYEQ: depth min=1 max=1` | eye-tag pipeline health |
| `hud: host found …` | HUD capture engagement |
| `POLL:` | XInput polling and synthesis |

## Healthy signatures

```
frames ~236/s   submitted ~118/s   state 5      ~2 Presents per submit
EYEQ: depth min=1 max=1                          threads in lockstep
POLL: getState 90/s synth 90/s realpad 0/s       VR input is winning
PER PRESENT game 2.2-2.9 ms   XR 1.1-2.1 ms
```

## Failure signatures

| Observed | Means |
|---|---|
| `POLL: … synth 0/s realpad 90/s` | a real/virtual pad is winning — `ControllerMode` is 0 |
| `EYEQ` min = 0 during gameplay | eye tags dropped or consumed out of step |
| `hud: host found 0 frames` | capture never engaged (a standing GOG report) |
| `game` time up, `XR` time unchanged | **not the mod.** Compare same save, same spot. |
| build stamp unchanged | stale DLL — discard the entire run |

## OpenXR error codes

| Code | Name | Reading |
|---:|---|---|
| `-32` | `FILE_ACCESS_ERROR` | runtime registered but unreadable — often a dangling x86 registration |
| `-35` | `FORM_FACTOR_UNAVAILABLE` | runtime loaded, no headset available on it |
| `-39` | `POSE_INVALID` | layer pose non-unit or zero quaternion |
| `-51` | `RUNTIME_UNAVAILABLE` | no usable runtime registered |

## Discipline

- **The config echo is the authority, not the INI in the editor.** Read-only
  files, the wrong storefront profile and VirtualStore redirection have all
  silently defeated edits.
- Quote the actual lines back. Counts and timestamps are the evidence.
- **Absence of a log line is evidence.** A guard that never fires disproves its
  hypothesis — that is exactly how the HUD "world leak guard" was killed after
  zero hits in 11,682 lines.
- Timestamps matter and the tester cannot see them live. If an event needs
  pinning to a log line, ask them to press a marker key at the moment it happens.
- Check `docs/INVARIANTS.md` before proposing a cause. Several plausible readings
  have already been measured and falsified.
