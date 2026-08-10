# Current state

**Updated:** 2026-08-09 · **Baseline:** `002a81a` "Pre refactor commit" ·
**Version string:** `1.0.3` (unreleased; `1.0.2` is public)

This file replaces the handoff documents that used to be written by hand between
chat sessions. It is the answer to "where is this project right now".

> **The two `BioShock_Remastered_VR_*` handoff MDs are now partly wrong.** They
> list the duplicate-world square as the open P0; it is fixed. Where they and
> `docs/INVARIANTS.md` disagree, the docs win — they were verified against source.

---

## Next step

**Run `.planning/sessions/M3.md` § M3-S1** — locate `Object::GetPropertyTextByName`
by pattern. Read the card, not this section.

**Why M3 and not M2:** M1-S2 was the pivotal test of the whole arc and it came
back **no**. M2 (StateBus, HUD gate, cutscene anchor, comfort) is all downstream
of a working signal, so it stays blocked until one exists. M3 — the native
property call, finding 2 — is the next untried source. M1-S3 is skipped.

### The premise, after M1 — 2026-08-09

The blocker was framed as *this UE2.5 fork has no reflection system*. That framing
is still wrong, and the corpus still says so. But **M1 turned the corpus findings
from three predictions into one measured negative and two untested claims**, and
the distinction it drew is the most useful thing to carry forward:

> **The corpus tells you what the script *can* do. It does not tell you what the
> shipped game *does*.**

1. **`myHUD.bHideHUD` — FALSIFIED LIVE (M1-S2).** The script really is written the
   way finding 1 said. The bit still never moves. Offsets confirmed and kept
   (`docs/ENGINE-MAP.md`); the *inference* that retail cutscenes run through
   `ActionCinematicEnter`/`Exit` is dead. Grave 1 in `CLAUDE.md`.
2. **`Core.Object` exposes native `GetPropertyTextByName(name)`**, and retail script
   calls it *on live instances*. **Untested.** Console `get` returns class defaults
   because the console resolves a **class** — that measurement stands, the
   conclusion drawn from it did not. **This is M3 and it is now the main line.**
3. **`LastPlayerInputContext` is also declared on `ShockPlayerController`**
   (`:42`). Every scan so far has been on the **pawn**. The controller copy has
   still never been looked at. **Untested**, and M3-S3 reads it.

Treat 2 and 3 the way 1 should have been treated: as predictions that owe a live
read. M1 cost two cycles and returned one solid measurement plus one clean
negative, which is the system working.

Do **not** re-enable the pitch latch, pitch servo, S75 unwind, or
ViewActor divergence as a shortcut. All four are falsified with evidence in
`docs/INVARIANTS.md`.

---

## Recently closed

**The duplicate-world square — FIXED.** A textured full-screen quad was landing
in the HUD capture's one-draw-per-frame slot, and the alpha repair forced it
opaque. The interface is untextured GameSWF geometry; the square was textured.
One term in the redirect condition (grep `PSSrv0Res`):

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

**M1-S1 — `myHUD` PINNED, and the PlayerController layout with it.**
`PlayerController.myHUD = controller+0x71C`, `HUD.PlayerOwner = myHUD+0x470`,
the six-bool DWORD at `myHUD+0x490` with `bHideHUD` as bit 0. Confirmed by a
self-validating back-reference on two controllers, stable over ten samples each,
and nine consecutive HUD fields matched their declared types on inspection.
Declaration-order arithmetic predicted `+0x710` and passed through seven
independent `ENGINE-MAP` anchors; the `+0xC` gap was `FMatrix` being 16-byte
aligned, so **every `PlayerController` field after `FixedRotation` shifts `+0xC`
from a naive walk** — recorded in `docs/ENGINE-MAP.md`.

**M1-S2 — `bHideHUD` FALSIFIED. The pivotal test of the arc returned no.**
The DWORD read `0x00000020` and never changed: 16 minutes, four controller
lifetimes, the bathysphere descent, a level load, the plasmid injection, combat,
barrels, both halves of a Little Sister sequence. **Eight marked boundaries,
zero transitions at any of them.** Decisive: the tester made the HUD visibly
appear and disappear by stepping in and out of the bathysphere entrance and the
DWORD did not move. So HUD visibility here is not driven by `bHideHUD` —
`HideMovie('HUD')` on the Scaleform controller is the new suspect.

**Two offsets renamed, free from S1's arithmetic.** `+0x5C0`/`+0x5C8` are
`PlayerController.aForward`/`aStrafe` — the **raw input axes**, which is *why*
the old measurement read `778–875` while pinned in a corner and provably not
moving. It remains an input-*requested* signal that geometry cannot false-fire.
And `+0x620` is `ViewTarget`, not a Pawn alias — which is why grave 2 (ViewActor
divergence) watched it track the pawn for whole sessions.

**`Core/Keybinds.cpp` is dead code.** `Key_Init` has **no caller anywhere**, so
every binding resolves to VK 0 and `Key_Down`/`Key_Fired` always return false.
Found the hard way: the M1-S2 marker key was routed through it and produced zero
marks for a whole run. Bind with `GetAsyncKeyState` until it is wired.

---

## Open — P1

### Cutscene / scripted-event detection
The longest-running problem. Everything downstream is built and waiting.

`GameState_Cutscene()` is inert: `g_cutscene` is written only by the demoted
ViewTarget path and by non-latching pitch telemetry. The context scan brackets
the right window and has **never locked** — zero `>>> CONTEXT` lines, ever.

Graveyard (evidence in `docs/INVARIANTS.md`): **`myHUD.bHideHUD`** · ViewActor
divergence · pitch-rate latch · pitch servo · S75/S78/S79 unwind · cached
view-target scans · console `get` · the input-ignored detector. **Nine now.**

Live leads, best first:
1. **The native property call (Tier 1)** — `Object::GetPropertyTextByName`, which
   retail script calls on live instances. **This is M3 and it is the main line.**
   It can crash, so it must be located by pattern and validated on `Health`
   before anything trusts it.
2. **`ShockPlayerController.LastPlayerInputContext`** — the *controller* copy,
   never examined; every scan so far was on the pawn. M3-S3. It is also the only
   lead that covers the Little Sister rescue, which pushes `NullInput` and never
   enters cinematic mode at all.
3. **`HideMovie('HUD')` on the Scaleform GUI controller** — new from M1-S2. The
   HUD demonstrably hides without `bHideHUD` moving, so something else is doing
   it, and `ShockPlayer.uc` calls exactly this during the rescue. Unexplored.
4. **`CurrentExorcismTarget`** — brackets the rescue only. The probe exists; the
   run that produced candidates contained no rescue and had a stale-snapshot bug
   on pawn change. `+0xEA4` and `+0xB58` survive. Superseded by lead 2, which
   costs the same and covers more.
5. **The UE2 metadata walk (Tier 2)** — `docs/proposals/ue2-reflection-bridge.md`.
   Parked; opens only if M3 fails.

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
- **`dxgiproxy` had never built.** It compiled both `dxgi_proxy.cpp` and a
  leftover Visual Studio wizard `dllmain.cpp`, each defining `DllMain` (LNK2005).
  Template deleted 2026-08-09; all three projects now build. Worth remembering
  that the dead-code audit only covered `BioshockVR/`.
- **`Keybinds.cpp` is never initialised — CONFIRMED, and it bit us.** `Key_Init`
  has zero callers, so the whole module is inert and `[KEYS]` is read by nothing.
  Every working hotkey calls `GetAsyncKeyState` directly, which means the
  collisions the header claims to have "corrected" are all still live:
  `VK_PRIOR` three readers, `VK_DELETE` two, `VK_NEXT` two, `VK_NUMPAD9` two.
  Wiring it fixes all four plus the 103-VK per-frame sweep in `PollFovKeys`, and
  ships rebinding for users with no numpad. `dist/BioshockVR.ini` now documents
  the real compiled-in keys and says plainly that rebinding needs a code fix —
  **a `[KEYS]` section was deliberately NOT added**, because nothing would read
  it. See `DECISIONS.md`.
- **Keep the `KEY: vk` keylogger in `PollFovKeys`.** It is the sweep above and it
  looks like pure noise, but it is the only reason M1-S2's eight marker presses
  were recoverable after the marker key turned out to be dead. If the sweep is
  optimised, keep a logging path for it.
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
- [x] **3b verified in a headset** — config echo **byte-identical** to the
      pre-refactor baseline, all 90 lines.
- [x] **3c** — reorganised into subsystem folders (`Core/ Render/ Hud/ Camera/
      Input/ Hands/ Game/`) mapping 1:1 to `docs/modules/*`, with
      folder-qualified includes. **The planned file SPLIT was abandoned on
      measurement** — see `DECISIONS.md`. Comment audit: orphaned fragments from
      3b removed, four stale claims corrected, all 27 path headers uniform.

No behaviour changes except the `ControllerLayout` default, logged in
`DECISIONS.md`.

### Refactor prep is complete

Housekeeping since: all logs now land in `Build\Final\logs\` (the loader
breadcrumb was the last one outside it); `Bioshock-Remastered-VR.slnx` and
`deploy.bat.example` removed; `docs/STYLE.md` written; `README.md` corrected;
`/newchat`, `/readme` and `/codemap` in place. Docs no longer cite line numbers
— they rot, and this session proved it when the square fix moved from 1425 to
1404. They cite greppable anchors instead.

Both projects rebuild clean, `Release|Win32`, with only the two pre-existing
`C4244` warnings in `CameraHook.cpp` (confirmed pre-existing by a clean rebuild
of the pre-refactor tree in a throwaway worktree).

3b was verified two ways. Statically: across all 114 config reads exactly one
line differs from before — the intentional `ControllerLayout` fix — and all 72
echo lines are byte-identical. At runtime: the `=== BioshockVR config ===` block
from a real run is **byte-identical to the pre-refactor baseline**, all 90 lines.

3c changed no code at all — file moves, include paths and comments — so it
carries no behavioural risk beyond the build itself, which is clean.

**The post-3c build has now run in a headset** — several times, across M1-S1 and
M1-S2 on 2026-08-09, including a full new game, a level load, a save reload and
~16 minutes of play. No regression reported and no crash. The refactor is
settled; the rollback copy is no longer needed.

---

## Last verified in a headset

**2026-08-09, build `M1-S2 cinematic flag` (23:19:49).** New game → intro
bathysphere → level load → out of the bathysphere → plasmid injection → combat
and barrels → load a save → both halves of a Little Sister sequence. ~16 minutes.

Health signals all correct: `>>> XR: runtime = OpenVR/SteamVR via BioshockVR
shim` · `EYEQ: depth min=1 max=1` · `hud: host found 30718 frames` ·
`POLL: synth 234/s realpad 0/s hook=ON xr=ON`. No errors, no warnings, no
crash, and the `myHUD` pointer re-resolved cleanly across all four controller
changes.

**Noticed and not chased**, all reported as long-standing rather than new:
- The **vita chamber "What is this?" prompt is head-attached, not world-anchored.**
  Related to the empty `AnchorIndexCounts` in P2 above.
- **HUD behaviour in the first bathysphere is generally odd** — absent on
  regaining control, appearing only on walking out, and toggling on stepping in
  and out of the entrance. That last one is what falsified `bHideHUD`, so it is
  evidence rather than only a bug.

**Still open from the previous headset session:** the ammo counter reads slightly
more transparent since the square fix. `HudAlphaFix=0` did not change it, so it
is not the alpha repair. Candidate: the counter is itself a textured draw now
excluded by the guard, putting it on the backbuffer instead of the quad. The
`CAPTURED:` diagnostic would answer it in one run.
