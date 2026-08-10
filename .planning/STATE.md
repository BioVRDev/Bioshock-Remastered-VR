# Current state

**Updated:** 2026-08-09 · **Baseline:** `002a81a` "Pre refactor commit" ·
**Version string:** `1.0.3` (unreleased; `1.0.2` is public)

This file replaces the handoff documents that used to be written by hand between
chat sessions. It is the answer to "where is this project right now".

> **The two `BioShock_Remastered_VR_*` handoff MDs are now partly wrong.** They
> list the duplicate-world square as the open P0; it is fixed. Where they and
> `docs/INVARIANTS.md` disagree, the docs win — they were verified against source.

---

## Recently closed

**The duplicate-world square — FIXED.** A textured full-screen quad was landing
in the HUD capture's one-draw-per-frame slot, and the alpha repair forced it
opaque. The interface is untextured GameSWF geometry; the square was textured.
One term at `DrawHook.cpp:1425`:

```cpp
PSSrv0Res(ctx) == nullptr
```

Works in every scene with no timers, gates or cutscene detection. **Four
timing-based approaches failed first** — see `docs/INVARIANTS.md`. It was solved
in one play session by a throttled log line printing the captured draw's
signature, after three sessions of theory.

**The console read channel — OPEN, with a known ceiling.** Per-slot output-device
thunks plus returning `1` from slot 4 (the "will you accept output?" query) make
`get` return values. `get ShockPlayer bReticleDisabled -> [True]`.

**Its ceiling is proven:** `get` reads the **class default object**, not live
state. `Health` stayed `200.0` after damage; `Location` stayed `(0,0,0)` after
walking. Useful for defaults and for confirming `set`; useless as a state source.

**New measured offset.** `PlayerController +0x5C0/+0x5C8` is the Acceleration
request the engine writes from the left stick — `0.000` at rest, `778–875` when
the stick is held, **including while pinned in a corner and provably not moving**.
An input-*accepted* signal, not velocity. Walking into geometry cannot false-fire it.

---

## Open — P1

### Cutscene / scripted-event detection
The longest-running problem. Everything downstream is built and waiting.

`GameState_Cutscene()` is inert: `g_cutscene` is written only by the demoted
ViewTarget path and by non-latching pitch telemetry. The context scan brackets
the right window and has **never locked** — zero `>>> CONTEXT` lines, ever.

Graveyard (evidence in `docs/INVARIANTS.md`): ViewActor divergence · pitch-rate
latch · pitch servo · S75/S78/S79 unwind · cached view-target scans · console
`get` · the input-ignored detector.

Two live leads, both in `docs/modules/gamestate.md`:
1. **`CurrentExorcismTarget`** — a pointer that brackets the Little Sister rescue
   exactly. Narrow but cheap. The probe exists; the run that produced candidates
   contained no rescue and had a stale-snapshot bug on pawn change. `+0xEA4` and
   `+0xB58` are the surviving candidates. Unresolved.
2. **The UE2 reflection bridge** — `docs/proposals/ue2-reflection-bridge.md`. The
   general answer. A project, not a session.

### Aim / movement coupling
`Controller.Rotation` (`+0x1E4`) drives the view, the weapon trace **and** the
walk direction. `AimSource=1` therefore makes the character walk wherever the
controller points. `AimSource=2` cannot work; the body-follow servo did not feel
right; `ModYaw` breaks scripted movement.

**The unmeasured question that gates everything:** does the firing trace read
`Controller.Rotation`, or the weapon socket? If the socket, the seam exists.
Settle this before designing anything else here.

### Rotation comfort — parked
`ModYaw` and `FreezeGameRotation` are compiled and **default 0**. `ModYaw=1`
zeroes `sThumbRX`, which freezes `Controller.Rotation`, which forced-move
sequences steer by — the opening bathysphere walks the player into the back wall
and the projector never plays. `FreezeGameRotation` requires `ModYaw`, so both
are inert. Keep them; do not delete.

---

## Open — P2

- **`AnchorIndexCounts` is empty**, so the "What is this?" context-help screen
  does not resize with the rest of the HUD. Fill it the way the square was found:
  bring the screen up, Numpad `*` to clear, Numpad `3` to dump, take the signature
  that only appears while it is up.
- **`HeadAimMode=2` starves scripted pitch gates.** The plasmid injection scene
  waits for the view to pitch down at the syringe and hangs forever. `=1` clears
  it. Reproducible.
- **Diagnostic key collisions.** `VK_PRIOR` has three readers, `VK_DELETE` two.
  Wiring `Keybinds.cpp` (complete, zero callers) fixes both plus the 103-VK
  per-frame sweep in `PollFovKeys`.
- **Plasmid hand, first equip.** On the first plasmid pull of a session it bound
  to the *right* hand instead of the left, and both plasmids showed the
  right-hand model. Cycling weapons cleared it and it did not recur. Observed
  2026-08-09 on the post-refactor build; **almost certainly pre-existing** —
  the refactor was verified semantically identical, and `ControllerLayout` (its
  one changed default) governs button layout, not hands. Smells like probe
  ordering on first equip. Low priority, but reproduce before touching
  `HandsProbe` for any other reason.
- **Index binding hazard**: `menu` (trackpad click) and `rest_l` (trackpad touch)
  share the left trackpad, so a menu press always arrives with the modifier held.
  With `DpadFlip=1` or `DpadModifier=4` that turns pause into context-help.
  Predicted from the JSON, unconfirmed on hardware.
- **WMR binding hazard**: grip bound as a digital button to an analog action
  thresholded at 0.80. Likely symptom on a Reverb G2 is no radial at all.
  Provisional profile, never hardware-tested.
- **Physical wrench** implemented but never conclusively validated.
- **Alternate-build lifecycle**: clear cached pointers immediately on pawn-null.
  GOG delta signature finds zero matches; Epic/GOG reticle Exec uses
  Steam-specific engine addresses.

---

## In flight

Repo repair and refactor prep — plan at
`~/.claude/plans/c-users-raywi-downloads-downloads-biosh-virtual-flamingo.md`.

- [x] Git repaired: `main` = `002a81a`, old lineage preserved as
      `archive/parked-experiments` (local + remote).
- [x] `.gitignore` no longer hides every `.md`; `dist/` now tracks the installer
      scripts and the shipping INI.
- [x] Doc system built.
- [x] **3a** — deleted 1,911 lines of uncompiled shim duplicates (including a
      stale `shim_main.cpp` still carrying the pre-fix SteamVR warp conversion);
      moved `BioshockVR/OpenXRShim/*` → `OpenXRShim/src/*`.
- [x] **3b** — `Config.h`/`Config.cpp`. 138 globals and 161 duplicated `extern`
      lines became one struct. `dllmain.cpp` 909 → 297 lines.
- [ ] **3c** — split `CameraHook.cpp`, `DrawHook.cpp`, `HandsProbe.cpp`.
      **Deliberately held until 3b is verified in a headset** — 3b touched every
      file, and stacking a second structural change on an unrun binary is exactly
      the batching this project's history says not to do.

No behaviour changes except the `ControllerLayout` default, logged in
`DECISIONS.md`.

### ⏳ Waiting on one headset run

Both projects rebuild clean, `Release|Win32`, with only the two pre-existing
`C4244` warnings in `CameraHook.cpp` (confirmed pre-existing by a clean rebuild
of the pre-refactor tree in a throwaway worktree).

Static verification of 3b was decisive: across all 114 config reads, exactly one
line differs from before — the intentional `ControllerLayout` fix — and all 72
echo lines are byte-identical. What remains is a runtime confirmation.

**To close it:** deploy, launch, and compare the `=== BioshockVR config ===`
block against the pre-refactor baseline. Everything should match except
`ControllerLayout`, and only if your INI lacks that key (the shipped INI sets it
to `0` explicitly, so the echo should be unchanged even there).

Then the presentation route, since the build touched every file:
main menu → load → combat → crate → vending → **Little Sister rescue** → pause.

---

## Last verified in a headset

The `002a81a` build, on the session that produced the square fix: square gone,
HUD correct in normal play, `CAPTURED: 5d tex=no` steady. One cosmetic
regression noted and not diagnosed — **the ammo counter reads slightly more
transparent**. `HudAlphaFix=0` did not change it, so it is not the alpha repair.
Candidate: the ammo counter is itself a textured draw now excluded by the guard,
which would put it on the backbuffer instead of the quad. The `CAPTURED:`
diagnostic would answer it in one run.
