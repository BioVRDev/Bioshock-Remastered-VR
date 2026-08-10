---
name: ini-check
description: Verify BioshockVR.ini against the shipping default and the project invariants, and check that settings actually took effect. Use when a setting seems to have no effect, when the user reports config confusion, or before trusting any INI-dependent observation.
---

# INI check

## The rule

**The startup config echo in `BioshockVR.log` is the authority — not the file in
the editor.** Read-only files, the wrong storefront profile, and VirtualStore
redirection have all silently defeated edits in this project.

```
=== BioshockVR config ===
```

If a change is not in that echo, it did not take. Establish that before drawing
any conclusion from what the user saw in the headset.

## Steps

1. **Find the live file.** It must sit beside `BioshockVR.dll` in
   `Build\Final\`. The mod resolves it from its own module path, so a copy
   anywhere else is silently ignored.
2. **Diff against `dist/BioshockVR.ini`**, the shipping default.
3. **Compare the echo to the file.** Any key where they disagree is the finding.
4. **Check the invariants** in `docs/INVARIANTS.md` — see the table below.
5. **Check for a typo.** Unknown keys are ignored *silently*, so a misspelling
   looks exactly like a working setting. The echo is how you catch it.

## Formatting rules that bite

- Comments must be on **their own line**. Windows does not strip trailing text, so
  `MenuIndexCounts=1493;1095  ; note` reads the note as data.
- A line starting with `;` is ignored entirely — to enable a commented setting,
  delete the leading semicolon.
- **Environment variables are not expanded.** `%APPDATA%\...` is read as a literal
  folder named `%APPDATA%`.
- Unknown keys are ignored silently.

## Invariants to verify in the echo

| Key | Expected | Why |
|---|---|---|
| `ControllerMode` | `1` | mode 0 lets any XInput device silently kill all VR input |
| `PitchServo` | `0` | runaway feedback loop; froze the view and hand |
| `CrosshairFromShot` | `1` | consumes the applied shot direction |
| `HudRedirect` | `1` | the square is fixed structurally — do not disable the HUD |
| `HudAlphaFix` | `1` | `0` makes every interface element too transparent |
| `MirrorPresentEvery` | `0` | means **time-based** (~17 ms), not "never present" |
| `Fullscreen` | `0` | exclusive fullscreen changes projection calibration |
| `ModYaw` | `0` | breaks scripted movement (bathysphere) |
| `FreezeGameRotation` | `0` | inert without `ModYaw` |
| `ExorcismProbe` | `0` | diagnostic; scans the pawn twice a second |
| `SwingLog` | `0` | diagnostic |

Also confirm `GameFovDegrees` matches `HorizontalFOV` in the game's
`Bioshock.ini` — a 10° drift was the original turn-warp bug.

## Duplicate keys

Six keys appear twice in the shipped INI: `ControllerDeadzone`, `DisableHeadBob`,
`GripHysteresis`, `GripThreshold`, `HeadRelativeMove`, `SwingEnabled`.

**This is expected and verified harmless** — both occurrences of each carry
identical values, and they are deliberate documentation repeats between the
"start here" block and the numbered reference section. Windows reads the first.
Do not report them as a defect and do not "fix" them.

If you find a *new* duplicate with **differing** values, that is a real finding —
the second one is dead and the user is editing a line that does nothing.

## Diagnostics currently in the build

`ExorcismProbe`, `HudLeakLog`, `SwingLog`, and the `GETTEST` probe on `VK_DELETE`.
All should be off in a normal run. `HudLeakLog` in particular guards a hypothesis
that was **falsified** — it has never fired — so a nonzero value there is noise.
