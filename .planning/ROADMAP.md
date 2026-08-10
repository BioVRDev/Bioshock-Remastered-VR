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
| ☐ | **M1-S1** pin `PlayerController.myHUD` | diagnostic | a stable non-null HUD pointer, confirmed by back-reference |
| ☐ | **M1-S2** read `bHideHUD`, prove it tracks cinematic mode | diagnostic | **the pivotal test** — a transition bracketing the bathysphere |
| ☐ | **M1-S3** harden into `EngineBridge` | hardening | INI-overridable, fail-closed, reset at boundaries |
| ☐ | **M2-S1** StateBus | refactor | `GameState_Cutscene()` returns the real signal; no behaviour change |
| ☐ | **M2-S2** HUD gate on the real signal | visible | HUD hidden in cutscenes, correct everywhere else |
| ☐ | **M2-S3** cutscene anchor, gated | visible | opening anchored; in-world moments unaffected |
| ☐ | **M2-S4** rotation comfort, gated | visible | no forced rotation in cutscenes; bathysphere still walks correctly |

**M1-S2 is the pivotal test of the whole arc.** If `bHideHUD` transitions around
the opening bathysphere, M2 is mechanical and the longest-standing problem is
closed. If not, skip M1-S3 and open M3 immediately.

### Later milestones — stubs, promoted to cards when they open

Their specifications depend on M1's verdict, so writing detail now is waste.

**M3 — native call bridge (Tier 1).** Opens if M1/M2 leave gaps, **and they will**,
for the Little Sister rescue (pushes `NullInput`, never enters cinematic mode).
S1: locate `Object::GetPropertyTextByName` by the FName/string chain, reusing the
staged scan at anchor `module scan`/`FindCalcView` in `Camera/CameraHook.cpp` — log
the address, call nothing. S2: call it on `Health` and **confirm it tracks damage**
— the discriminator between "bridge broken" and "property empty". S3: read
`ShockPlayerController.LastPlayerInputContext` (the controller copy, never
examined) and feed the existing `kContexts` table, which has never had an input.

**M4 — QOL the signal unlocks.** S1: arms/sleeves visible only during cutscenes.
S2: right-stick look during scripted sequences. S3: settle the aim/movement
measurement — does the firing trace read `Controller.Rotation` (`+0x1E4`) or the
weapon socket? **Either answer is worth having.**

**M5 — metadata walk (Tier 2).** `docs/proposals/ue2-reflection-bridge.md`,
unchanged. Opens only if M3 fails.

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

## Next — cheap wins with real user impact

These are small, independent, and none depends on cutscene detection.

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
