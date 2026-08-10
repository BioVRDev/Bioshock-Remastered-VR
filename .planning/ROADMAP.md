# Roadmap

Ordered by value per test cycle, because every verification step here costs a
human putting on a headset. Current status lives in `STATE.md`.

---

## Now — refactor prep

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
3. **Restore `openxr_loader_steam.dll`.** The shim source was consumed, so
   `Setup.bat` cannot select the SteamVR path on a fresh install. Rebuild and place.
4. **Reconcile `README.md`** — it names `FirstTimeSetup.bat` and lists four install
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

## Then — cutscene detection

The largest open problem. Everything downstream is built and waiting: the HUD
gate, the cutscene anchor, comfort settings, right-stick look during scripted
sequences. See `docs/modules/gamestate.md` for the full graveyard.

Attack in this order, stopping at the first thing that works:

1. **String-scan the executable** for `PUSHINPUTCONTEXT`, `GETALL`, `EDITACTOR`.
   Free, no build. Any hit is far cheaper than what follows.
2. **`CurrentExorcismTarget`.** A pointer bracketing the Little Sister rescue.
   Narrow — solves one sequence, not cutscenes generally — but cheap and testable
   in a session. Fix the pawn-change snapshot reset first, then do a clean run
   with a marker key.
3. **The UE2 reflection bridge** — `docs/proposals/ue2-reflection-bridge.md`.
   The general answer, and a project rather than a session. Stage 1 is one
   keypress and one log line that decides whether stages 2–4 are worth starting.

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
