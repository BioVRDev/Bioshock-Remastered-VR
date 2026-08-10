# Roadmap

Ordered by value per test cycle, because every verification step here costs a
human putting on a headset. Current status lives in `STATE.md`.

---

# Current arc — live state → cutscenes → QOL

**One session, one idea, one falsifiable outcome.** A session ends when its
outcome is decided, not when the idea feels done. Cards are in
`.planning/sessions/M<n>.md`, one file per milestone. Findings and design in
`docs/ARCHITECTURE.md`.

| | Session | Type | Outcome |
|---|---|---|---|
| ✅ | **M0** workflow, docs, cards | no code | this section exists |
| ✅ | **M1-S1** pin `PlayerController.myHUD` | diagnostic | **`+0x71C`**, back-reference `+0x470`, stable. Layout in `ENGINE-MAP.md` |
| ✅ | **M1-S2** read `bHideHUD`, prove it tracks cinematic mode | diagnostic | **NO** — flat across 8 marked boundaries. Grave 1 |
| ⛔ | **M1-S3** harden into `EngineBridge` | — | **skipped**: S2 returned no, nothing to harden |
| ✅ | **M3-S1** locate `GetPropertyTextByName` | diagnostic | **YES** — rva `0x7346E0` (Steam), all four accessors, stable across two launches. `enginebridge.md` |
| ☐ | **M3-S2** call it on `Health` | diagnostic | the value **tracks damage** — bridge proven, or not |
| ☐ | **M3-S3** read the controller's `LastPlayerInputContext` | diagnostic | `kContexts` finally gets an input. **Now has computed candidates: `controller+0x9C4` or `+0x9C0`**, walked back from the measured `+0x9E0` |
| ✅ | **M7** scripted-event detection and QOL | visible | **"Perfect on every front."** Sequences land where they intend; arms follow rig motion; entry stall gated; gameplay shake gone |
| ☐ | **M2-S1** StateBus | refactor | `GameState_Cutscene()` returns the real signal; no behaviour change |
| ☐ | **M2-S2** HUD gate on the real signal | visible | HUD hidden in cutscenes, correct everywhere else |
| ☐ | **M2-S3** cutscene anchor, gated | visible | opening anchored; in-world moments unaffected |
| ☐ | **M2-S4** rotation comfort, gated | visible | no forced rotation in cutscenes; bathysphere still walks correctly |

**M1-S2 was the pivotal test and it returned no** (2026-08-09). `bHideHUD` is
grave 1 in `CLAUDE.md`; the script writes it, the shipped game does not.
**M2 moves behind M3** — every M2 session is downstream of a working signal, and
there is not one yet. M3 is the card set in flight; its S2 is the new pivotal
test, and it is the one session in this arc whose failure mode is a **crash**
rather than a wrong number, so it ships default-off behind an INI switch.

**M3-S1 passed on 2026-08-10.** The addresses are measured and stable, so the
question is no longer *can we find live engine state* but *can we invoke it
without crashing*. S2 inherits one correction: the symbol is `exec`-style and
takes an `FFrame`, not a string — there is no direct call to make. Read the ⚠ box
on its card before starting.

**The lesson M1 bought, and it applies to every remaining card:** the corpus
tells you what the script *can* do, not what the shipped game *does*. Findings 2
and 3 are still predictions that owe a live read.

### Later milestones — stubs, promoted to cards when they open

**M4 — QOL the signal unlocks.** S1: arms/sleeves visible only during cutscenes.
S2: right-stick look during scripted sequences. S3: settle the aim/movement
measurement — does the firing trace read `Controller.Rotation` (`+0x1E4`) or the
weapon socket? **Either answer is worth having.**

**M5 — metadata walk (Tier 2).** `docs/proposals/ue2-reflection-bridge.md`,
unchanged. Opens only if M3 fails.

**M6 — the hand rig. PROMOTED TO A CARD: `.planning/sessions/M6.md`, and it is
next.** Roomscale, left-handed mode, detached hands and two-handed grip —
researched 2026-08-10 in **`docs/proposals/vr-features-research.md`**. The
finding that shapes the milestone: those four asks need only **two** enablers,
not four.

- Three of them (handedness, detached hands, two-handed grip) are one mechanism
  — a **rigid bone-cluster transform**, applied late, on the bone array
  `ArmHide` already writes to. `docs/modules/hands.md` § *Future direction*
  predicted this before the features were asked for.
- **Roomscale is downstream of M3-S2.** It needs `AActor::Move` — swept and
  collision-checked — and the symbol is confirmed present in the exe. So S2
  should be built with a second caller in mind; that raises its value well
  above cutscene detection.
- Two lottery tickets worth buying first, one console command each:
  `set Pistol AttachBone L_Grip` (may collapse left-handed mode to nothing) and
  confirming `Exec` reaches `PlayerController` exec functions (gates a free QOL
  pass — quick save/load on a controller chord).

---

# Backlog — outside the current arc

## Done — refactor prep

Behaviour-preserving. See the plan file for detail.

| | Step | Verification |
|---|---|---|
| ✅ | Git repair, archive old lineage | `main` == `origin/main` == `002a81a` |
| ✅ | `.gitignore`, `dist/`, doc system | docs are tracked |
| ✅ | **3a** delete uncompiled shim duplicates, move shim sources | both projects build clean |
| ✅ | **3b** `Config.h`/`Config.cpp` | 1 of 114 reads differs (intentional); 72/72 echo lines identical; zero new warnings |
| ⏳ | **verify 3b in a headset** | config echo vs baseline, then the presentation route |
| ☐ | **3c** split `CameraHook` / `DrawHook` / `HandsProbe` | build + full headset route |

**3c is held until 3b has run.** 3b touched every file in the mod; stacking
another structural change on a binary that has never launched would make a failed
headset test ambiguous across ~2,000 changed lines. One change per test cycle is
this project's most expensively-learned rule.

---

## Next — the bathysphere probe, requested 2026-08-10

**`FreezeGameplayRotation` ships with a known gap and the tester wants it closed
properly.** It discards the game's rotation during ordinary play — shake, kick,
the auto-pan toward enemies — but **a bathysphere ride is not a scripted
animation**, so it freezes those too and the ride's camera stops following the
sphere. It is default 0 for that reason and is marked `TEMPORARY` at the freeze
site in `Camera/CameraHook.cpp`.

**Excluding it by level name is not possible today** and was considered and
rejected: the mod does not know what map it is on. `g_level` is used only for
`Level.Pauser`, so this would be a fresh offset hunt on `LevelInfo` — the same
cost as the real fix, with none of the reuse.

**The real fix, already identified in the corpus.**
`Scripting`'s `ActionEnableBathysphereModeForPlayer` sets three fields on
`ShockPlayer` for the duration of a ride:

```unrealscript
Player.bUseHavokRigidBodyCapsuleCollisions = false;
Player.bUseHavokPhantomCollisions          = false;
Player.bCannotFall                         = true;
```

**`bCannotFall` is the signal** — a plain bool, set on entry and cleared on exit,
Tier 0, no native call. Find its offset the way `hands+0x594` was found: predict
by declaration order from a *proven* anchor, then validate live. Then AND it into
the freeze condition and the switch can default on.

Worth doing in the same session: the Little Sister **rescue** and the **EVE
injection** are still undetected — both are Hands *states*
(`ExorcisingGatherer`, `InjectingEve`) rather than scripted sequences, measured
in M7-S1. Reading the Hands state would close both plus the bathysphere in one
mechanism, but it needs either the native call (M3-S2) or a state-frame walk.

## Next — cheap wins with real user impact

These are small, independent, and none depends on cutscene detection.

0. **Two one-command probes**, both from `docs/proposals/vr-features-research.md`,
   both worth doing in the first minute of any play session:
   `set Pistol AttachBone L_Grip` (does the rig have a left grip bone?) and any
   `PlayerController` exec — e.g. `ForcePause` — to confirm `UGameEngine::Exec`
   reaches them. The second gates quick-save-on-a-chord, which is the single
   highest-value QOL item found.
1. **Fill `AnchorIndexCounts`.** The "What is this?" screen does not resize with
   the HUD because the list is empty. Bring the screen up, Numpad `*`, Numpad `3`,
   take the signature that only appears while it is up. One play session, no code.
2. **Diagnose the ammo-counter transparency.** Noted after the square fix and not
   explained; `HudAlphaFix=0` did not change it. Re-add the `CAPTURED:` diagnostic
   and look for a `tex=yes` line during normal play — if the counter is textured it
   is being excluded by the guard and rendering on the backbuffer. One run.
3. **Reconcile `README.md`** — it names `FirstTimeSetup.bat` and lists four install
   files; neither matches the real package.
5. **Confirm `CollectLogs.bat` knows about `logs\openxr_shim.log`.** The shim log
   moved; support bundles may have been shipping without it.

## Then — wire `Keybinds.cpp`

Complete, correct, and never called. Wiring it gives the key map one owner and
fixes three things at once: the `VK_PRIOR` three-way collision, the `VK_DELETE`
two-way collision introduced by the GET probe, and the 103-virtual-key sweep
`PollFovKeys` runs every XR frame from both submit paths. It also ships the
feature it was written for — users without a numpad currently cannot rebind
anything. See `.planning/DECISIONS.md` for why it was not deleted.

## Then — settle the aim/movement question

**One measurement gates a whole design space:** does the weapon firing trace read
`Controller.Rotation` (`+0x1E4`), or the weapon socket?

If the socket, motion aim can be decoupled from locomotion and the "walks where
the controller points" problem is solvable. If `Controller.Rotation`, it is not,
and the honest answer is to document the coupling and stop. Either result is
worth having; the current state is guessing.

## Then — cutscene detection → **superseded, see § *Current arc* above**

The largest open problem, and it now has a plan. Everything downstream is built
and waiting: the HUD gate, the cutscene anchor, comfort settings, right-stick look
during scripted sequences.

**The attack order that used to be here is retired.** It opened with a string scan
and treated the reflection bridge as the general answer, because it assumed live
property reads were impossible. `docs/ARCHITECTURE.md` shows they are not: the game's
cutscenes set `myHUD.bHideHUD`, and `Core.Object` exposes a native live-property
accessor that retail script calls on instances. The current order is
§ *Current arc* above — Tier 0 offset read first, native call second, metadata walk
parked as the fallback.

`CurrentExorcismTarget` is no longer a headline lead: it brackets the Little Sister
rescue only, and M3-S3 reads the input context that brackets the rescue *and*
several other sequences for the same effort.

Do **not** re-enable the pitch latch, pitch servo, S75 unwind, or ViewActor
divergence as a shortcut.

## Later

- **Rotation comfort**, once a real scripted-sequence signal exists. `ModYaw` and
  `FreezeGameRotation` are already written and default-off, waiting for a state
  they can be gated on.
- **Per-plasmid grip/cursor offsets.** All plasmids share slot 8, so Electrobolt
  and Telekinesis overwrite each other.
- **Crosshair as a published ray + optional scene trace**, replacing fixed depth.
- **Alternate-build hardening**: clear cached pointers on pawn-null; rederive the
  GOG delta signature (currently zero matches); derive the engine pointer/vtable
  instead of hardcoding, so the reticle Exec works on Epic and GOG.
- **Hardware validation** for Index (resting grip, radial, the trackpad
  `menu`/`rest_l` conflict) and WMR/Reverb (digital grip bound to an analog action —
  likely no radial at all).
- **Physical wrench validation** — implemented, never conclusively tested.
- **Layered architecture** — subsystem lifecycle registry and a validated engine
  memory-accessor layer. Deferred, not rejected; the refactor leaves the seams.
