# Decisions

Append-only. Every entry records what was decided, why, and — for anything
retired — **the evidence that killed it**. The point is that a plausible idea
cannot quietly come back a month later.

Short-form technical invariants live in `docs/INVARIANTS.md`; this file is for
decisions with a date and a rationale.

---

## 2026-08-09 — `main` reset to `002a81a`, old lineage archived

`HEAD` was detached at `002a81a` and local `main` (`4e6bc09`) was on a divergent
line forked at `ff698ec`, carrying three experiment commits `002a81a` does not
contain. Nothing could be committed.

`main` now points at `002a81a`. The three commits (`bf0206c` Cutscene
improvements, `4e0546b` Flicker fix, `4e6bc09` WIP mod-yaw/freeze/aim) are
preserved as branch `archive/parked-experiments` and tag
`archive/parked-experiments-2026-08`, pushed to origin before `main` was
force-updated with `--force-with-lease`.

`master` (`68c6e6e`, v1.02) left alone as a historical marker.

`stash@{0}` retained pending review. It is based on `bf0206c` — the abandoned
lineage — and contains the mid-experiment state: the texture exclusion (already
in `002a81a`), the bars veto, `GameState_InputIgnored`, and a HUD gate with a
**duplicated line that would not compile**. Its only content not in `main` is the
`CAPTURED:` diagnostic logging.

---

## 2026-08-09 — `.gitignore` no longer excludes all markdown

The file ended with a blanket `*.md`, making every document in the repo invisible
to git. `README.md` survived only because it predated the rule. Replaced with
targeted patterns (`HANDOFF*.md`, `*_HANDOFF*.md`, `NOTES.md`, `scratch/`).

Without this, the entire documentation system would have been silently untracked.

---

## 2026-08-09 — the shipped INI's duplicate keys stay

Six keys appear twice: `ControllerDeadzone`, `DisableHeadBob`, `GripHysteresis`,
`GripThreshold`, `HeadRelativeMove`, `SwingEnabled`.

The handoff documents flag duplicate keys as a source of repeated confusion, and
the original plan was to dedupe. **Measurement changed that**: all six pairs carry
identical values. They are deliberate documentation repeats between the "start
here" block and the numbered reference section. Windows reads the first
occurrence, so there is no ambiguity to resolve.

Deleting either copy degrades user-facing documentation for zero behavioural
gain. They stay, and `docs/modules/packaging.md` records why so nobody "fixes"
them later.

---

## 2026-08-09 — `Keybinds.cpp` is kept, not deleted

310 lines with **zero callers** — genuinely dead by the letter of the refactor
brief. Kept anyway.

It is a complete implementation of a feature users need (rebindable keys for
those without a numpad), and wiring it is simultaneously the correct fix for two
live defects: `VK_PRIOR` has three independent readers and `VK_DELETE` has two,
because every key check in the codebase is a raw `GetAsyncKeyState` with no
central owner. Deleting it would remove the fix along with the dead code.

Scheduled in `ROADMAP.md` as a change, not folded into the refactor.

---

## 2026-08-09 — `ControllerLayout` default corrected to `1`

The global was defined as `1` while its ini read defaulted to `0`. The shipped INI sets it explicitly, so only users whose INI lacks the
key were affected.

Corrected to match the global while consolidating config. **This is the only
intentional behaviour change in the refactor**; everything else is verified
identical by diffing the startup config echo.

---

## 2026-08-09 — layered architecture deferred

The refactor does dead-code removal, config consolidation and file splits. It
does **not** introduce subsystem interfaces, a lifecycle registry, or an engine
memory-access layer.

Rationale: the verification step for this project is a human putting on a headset,
so every structural change costs a real test cycle. Behaviour-preserving,
compiler-verifiable changes first; deeper structure once the baseline is trusted.

`Config.h` is deliberately the first shared contract, and the file splits create
the seams a lifecycle registry and a validated memory-accessor layer would slot
into. The door is open, not walked through.

---

## 2026-08-09 — GSD Core adopted in spirit, not installed

`open-gsd/gsd-core` was evaluated. Its core idea — push heavy research into
fresh-context subagents, keep durable state in tracked files — is the same
problem this project solves by hand-writing handoff documents.

Not installed, for three reasons: it requires `npx` and Node, npm and GitHub CLI
are all absent from this machine; ~65 commands are shaped for web application
work (UI specs, sketches, visual review, generated tests, parallel executors)
that cannot apply to a native mod verified by wearing a headset; and it adds a
second vocabulary on top of the refactor.

Taken instead: the persistent `.planning/` state files and the codebase map, hand
built as plain repo files. `/codemap` is the local equivalent of
`/gsd-map-codebase`. No dependency, works in the desktop app immediately.

---

## 2026-08-09 — source split abandoned; folders instead

The approved plan called for splitting `CameraHook.cpp`, `DrawHook.cpp` and
`HandsProbe.cpp` along their section banners. Measurement killed it before any
code moved.

Each file is **one hook plus the state machine it maintains**, and the state
spans the whole file because a single function drives it:

| File | Statics | Evidence |
|---|---:|---|
| `CameraHook.cpp` | 64 | `g_aimBase` 779..2217 · `g_lpQuat` 152..2380 · `g_eyeQ` 118..2365 |
| `DrawHook.cpp` | 98 | `g_hudHost` 159..1733 · `g_indexedThisFrame` 180..1823 |
| `HandsProbe.cpp` | 37 | `g_hands` 227..1705 · `g_pawn` 226..1704 · `g_gun` 769..1706 |

Cutting them by section would convert 30–50 file-level statics into shared
cross-file globals — precisely the problem `Config.h` had just removed — and
would split seqlock writes (`g_lpSeq`/`g_lpQuat`/`g_lpValid`) that must happen
together. Smaller files, worse coupling, and a class of silent breakage that
only shows up in a headset.

The file sizes reflect a cohesive design, not bloat. Their internal sections are
already indexed by `docs/modules/*` and kept honest by `/codemap`.

**Done instead**, which is what the size complaint was actually about: source
organised into subsystem folders mapping 1:1 onto the module docs, with
folder-qualified includes so a file's dependencies read at a glance.

One genuine seam does exist and was left alone for now — `EnumReadableRegions` +
`FindCalcView` (~230 lines) touch only `g_modBase`/`g_modSize`, so "find the
function" is separable from "hook it and drive it". Worth doing as part of the
deferred layered architecture rather than on its own.

---

## 2026-08-09 — no auto-formatter, and no line numbers in docs

**No `.clang-format`.** One was written, tuned against the tree, and measured:
even at maximum permissiveness it wanted to change **37% of all lines**. Every
disagreement was one where the hand-formatting is better — it collapses
column-aligned `extern` blocks, splits paired idioms like
`va_list a; va_start(a, fmt);`, and aligns wrapped arguments to the opening
parenthesis, pushing code past column 40 in a file that otherwise holds 80.

A formatter that fights the code on every line is not a guard rail. `.editorconfig`
covers indentation, encoding, line endings and trailing whitespace; layout stays
human. The style is documented in `docs/STYLE.md` instead. **Do not bulk-format**
— it would also break `git blame` on code whose history is often the only record
of why it is shaped that way.

**Docs cite anchors, not line numbers.** `CLAUDE.md` has always said "never line
numbers — this file moves constantly", and the docs then used 48 of them. This
session proved the point: the square fix moved from `DrawHook.cpp:1425` to
`:1404` when the extern blocks were deleted, and every module doc's section index
was stale within one commit.

All of them are now greppable anchors — banner text (`the classifier`,
`ALPHA REPAIR`) or code (`PSSrv0Res(ctx) == nullptr`). File sizes stay as
numbers because they are informative rather than navigational, and `/codemap`
refreshes them.

---

## 2026-08-09 — `bHideHUD` is not the cinematic flag; Tier 0 dead-ends

**M1-S1 succeeded and M1-S2 falsified what it was for.** Both results stand on
their own and neither is wasted.

**S1 pinned the offsets and they are good.** `PlayerController.myHUD =
controller+0x71C`, `HUD.PlayerOwner = myHUD+0x470`, the six-bool DWORD at
`myHUD+0x490` with `bHideHUD` as bit 0. Declaration-order arithmetic predicted
`+0x710` and passed through seven independent `ENGINE-MAP` anchors; the `+0xC`
gap was `FMatrix` being 16-byte aligned, which the live read settled in one run.
Nine consecutive HUD fields matched their declared types on inspection. This
also renamed two long-mislabelled offsets: `+0x5C0`/`+0x5C8` are `aForward`/
`aStrafe` (raw input axes — which explains the old "reads 875 while pinned in a
corner" measurement) and `+0x620` is `ViewTarget`, not a Pawn alias.

**S2 then measured the bit and it never moves.** `0x00000020`, unchanged across
16 minutes and four controller lifetimes covering the bathysphere descent, a
level load, the plasmid injection, combat, barrels and both halves of a Little
Sister sequence. Zero transitions, zero identity failures. **The decisive
observation was the tester's:** they made the HUD visibly appear and disappear
by stepping in and out of the bathysphere entrance, and the DWORD did not move.

So `docs/ARCHITECTURE.md` finding 1 read the corpus correctly —
`ActionCinematicEnter`/`Exit` really do write that bool — but the retail
sequences do not run through those script actions. **The corpus tells you what
the script *can* do, not what the shipped game *does*.** That is the
transferable lesson, and it applies to findings 2 and 3 as well.

**Consequences.** M1-S3 is skipped; `EngineBridge` is not built speculatively
and arrives when a working signal needs it. The next move is M3-S1. The lead
this leaves is `HideMovie('HUD')` on the Scaleform GUI controller, already
noted in `docs/modules/gamestate.md` for the rescue and now the prime suspect
for how the HUD actually hides.

**Two process failures worth more than the result.**

`Core/Keybinds.cpp` is **dead code — `Key_Init` has no caller anywhere**, so
every binding resolves to VK 0 and `Key_Down`/`Key_Fired` always return false.
The S2 marker key was routed through that API and produced zero marks for a
whole run. It looked like the established pattern; nobody had checked it was
wired up. Bind with `GetAsyncKeyState` until `Key_Init` gets a caller.

**The run was saved by a diagnostic nobody was looking at.** `PollFovKeys` in
`Render/XRSession.cpp` logs `KEY: vk 0x%02X` for every nav/numpad/F-row key,
and it had quietly recorded all eight F1 presses. The marks were recovered
after the fact and the S2 verdict is stronger for it. **Check the log for
evidence already in it before writing off a run** — and keep that keylogger.

The shipped `dist/BioshockVR.ini` promised a `[KEYS]` rebinding section that
never existed. **Adding one was considered and rejected**: with `Key_Init`
uncalled it would be read by nothing, so it would have converted a broken
cross-reference into settings that silently do nothing. The section now lists
the real compiled-in keys, including the four live key collisions, and says
plainly that rebinding needs a code fix.

**Do not treat a source comment about the tester's hardware as fact.** Two
comments claim `PGUP`/`PGDN` "never register". The tester uses both routinely.
Acting on the stale comment produced a keybinding detour that was never needed.

---

## 2026-08-10 — M3-S1 located the natives by symbol name, not by FName scan

> **RESULT: PASSED in one cycle.** `GetPropertyTextByName` @ rva `0x7346E0` on
> Steam, identical across two launches, all four accessors located, stride 12
> with 8 of 8 neighbours agreeing. The row's function slots **are** written at
> runtime — the one real risk, resolved on the first attempt. Numbers in
> `docs/modules/enginebridge.md`; the mechanism is now an invariant in
> `docs/INVARIANTS.md` § *Engine access*.

The card specified reusing `FindCalcView`'s six-stage FName/string scan.
**Static analysis of the two shipped executables — read-only, offline, no
headset — found a strictly better anchor**, and the implementation deviates
accordingly. Recorded here so the deviation is not mistaken for drift.

`BioshockHD.exe` contains `L"intUObjectexecGetPropertyTextByName"` exactly once
in `.rdata`. That is UE2's `IMPLEMENT_FUNCTION` registration symbol, and exactly
one DWORD in the image points at it, from a 12-byte row of the engine's native
table in `.data`. `FindCalcView` needs six stages because an FName literal only
reaches a cached-index global; this anchor lands on the function's own row. So
the scan is three stages, validated against the table's own stride regularity.

Every principle of the card survives: locate by pattern never by RVA, staged,
fail closed, log the near misses, one-shot with backoff.

**Two things the card did not anticipate.** All four property accessors are
located rather than one — four adjacent rows down the same code path, free, and
four distinct executable addresses is far stronger evidence than one. And the
row's function slots are **zero on disk**, so the runtime read is the whole
question; the tick retries with backoff and dumps the raw rows if they never
fill, which makes a NO cost one cycle instead of two.

**The value of going one rung past the card.** The static pass cost no headset
cycle and settled three things the session would otherwise have spent one on:
that the anchor exists at all, that it is unique, and that its row sits at a
**different RVA on Steam (`0x11BE684`) than on Epic (`0x11BD6B4`)** — which is
`docs/ENGINE-MAP.md` § *Storefront divergence* made concrete, and proof that a
hardcoded address would already have been wrong on one of the two builds we can
check.

**A trap worth keeping.** `intUObjectexecGetPropertyText` is a prefix of
`intUObjectexecGetPropertyTextByName`. Matching without the null terminator makes
the shorter name hit inside the longer one and returns the wrong function. This
happened during the static pass; the terminator is now part of the needle.

**A correction owed to M3-S2.** The symbol is `exec`GetPropertyTextByName, i.e.
`void UObject::execGetPropertyTextByName(FFrame&, RESULT_DECL)` — a `__thiscall`
member taking a bytecode frame. There is no `obj->GetPropertyTextByName('Health')`
entry point. S2 was written assuming one; its card now carries a ⚠ box saying so.

`EnableNativeScan` ships **default 1**, a deliberate exception to
"new behaviour ships default-off": the scan writes nothing, hooks nothing and
gates nothing, and its entire product is the log line, so shipping it off would
waste the cycle it exists to spend. Same reasoning as the M1 `MyHudTick` probes.

---

## 2026-08-10 — four VR features collapse onto two enablers, not four

Research only, no headset time — `docs/proposals/vr-features-research.md`.
Recorded here because the *shape* of the answer is the decision, and someone
scoping these four features separately would spend far more than they need to.

**Roomscale, left-handed mode, detached hands and two-handed grip were scoped
together and came back as two projects.**

1. **Handedness, detached hands and two-handed grip are one mechanism** — a rigid
   bone-cluster transform on the `hkQsTransform` render bone array `ArmHide`
   already writes to. The rotation lane is sitting there untouched; the rig is
   mapped (left cluster 6–21, right 27–44, bone 43 the weapon attachment).
   `docs/modules/hands.md` § *Future direction* had already described this
   mechanism before the features were requested. Build it once; the three
   features are configuration on top.
2. **Roomscale is downstream of M3-S2.** It needs a swept, collision-checked
   move, which is `AActor::Move` — and `intAActorexecMove` is **confirmed present
   in the shipped exe** by the same scan M3-S1 shipped. So S2's `FFrame`
   machinery should be built with a second caller (a vector parameter) in mind.
   **This raises M3-S2's value well above cutscene detection** and is the single
   most useful thing the research produced.

**Rejected as the roomscale primitive: writing `Location` (`+0x1D8`) directly.**
The offset is known, it is tempting, and it is `SetLocation(bNoTest=true)`
semantics — no collision at all. You would walk through walls. Velocity injection
is kept as a fallback: collision comes free but it is a push, not an exact
displacement, so it will not track 1:1.

**Two lottery tickets worth buying before building anything**, one console
command each through the *existing* Exec channel:

- `set Pistol AttachBone L_Grip`. `Holdable.AttachBone` is a `config name`
  defaulting to `"R_Grip"`, so if the rig has a left counterpart the weapon moves
  hands using the game's own attachment system and left-handed mode collapses to
  almost nothing. **Caveat: `R_Grip` is the only such name anywhere in the
  corpus**, so there is no positive evidence `L_Grip` exists.
- Any `PlayerController` exec (e.g. `ForcePause`) to confirm `UGameEngine::Exec`
  reaches them. This gates a **free QOL pass**: `User.ini` binds exec functions
  straight to gamepad buttons — with `onHold`, `onRelease` and `MODIFIER` chords,
  per input context — so **quick save/load on a controller chord** needs no
  engine work at all. Highest-value QOL item found.

**One real bug fixed on the way.** `kContexts` was diffed against the game's own
30-entry `Contexts=` list in `User.ini`. Exactly one was missing:
**`ExcorcisingGatherer`** — the Little Sister rescue, i.e. the one sequence M3-S3
exists to detect. **The game misspells it** (`Excorcising`, while the corpus
spells the *animation* name correctly), so adding it from memory or from
`ShockPlayer.uc` would silently never match. Added, classified `CTX_SCRIPTED` by
inference, compiled, **not deployed and never run**. Re-run that diff against
`User.ini` rather than trusting the corpus or the table.

---

## 2026-08-10 — M6-S1 and the free hand

**The cluster write goes at CalcView, not Present, and the research doc was
wrong to say "apply late".** `docs/proposals/vr-features-research.md` inherited
that from S59/S60 — but that measurement was about the actor **rotator**, which
the game tick rewrites, not about the bone array. Bone writes are held by the
dirty byte instead, and the sleeve pass writing from CalcView every frame is the
standing proof. Present is also the render thread and would race the game
thread's evaluation for no measured benefit. **Do not "fix" this back.**

**Rejected: a damping constant for head-based movement.** The report was "turning
90 degrees left almost moves you backwards", which reads like over-sensitivity
and invites a tuning factor. It was a **duplicate**: the aim field carried the
head *and* `HeadRelativeMove` rotated the movement stick by the head offset
again, and 90 twice is 180. The fix is to suppress the second rotation.
A damping constant would have masked it at one angle and stayed wrong at every
other one.

**`MovementMode` supersedes the `AimSource` + `HeadRelativeMove` interaction.**
Those two switches encoded three sensible modes and one broken combination (head
aim with the stick still being rotated). One key now names the mode; the old
pair is read only as the seed when `MovementMode` is absent. Head aim seeds mode
0 whatever `HeadRelativeMove` says — **reproducing the broken pairing faithfully
is not a compatibility win.**

**Two predicates for the aim source, deliberately not one.**
`AimUsesHeadNow()` drives the aim field *and* the stick gate — those must agree
or the head is applied twice. `ModeUsesHead()` drives the view composition and
ignores the empty-handed case, so picking a weapon up never changes how the world
is presented. Collapsing them into one predicate reintroduces one bug or the
other.

**Bone 43 takes the cluster's position and nothing else.** It is the weapon
attachment and telekinesis release walks the attachment path through it —
telekinesis being a plasmid, which is exactly when the right cluster is driven.
Moving it is what `HideBone` already proved safe; rotation is untested and scale
is known fatal. It is skipped on restore for the same reason. **Still unverified
in a headset as of this entry.**

**Rejected on evidence: filling `AnchorIndexCounts` to anchor the loose HUD
screens.** It is the obvious move and it is in the backlog, but the tester
reports index counts **false-firing during ordinary play**. A gameplay-vs-menu
gate would narrow the window it can misfire in, not close it, and a wrong entry
in a signature list has frozen the camera before. The replacement is a real
state signal — `FlashGUIController` keeps a list of **named** playing movies, so
"which screen is up" has an exact answer the game already maintains. M6-S5.

**Per-plasmid tuning does NOT wait on the native property call.** M3-S2 has never
run and this does not need it. `ShockPlayer` declares `PossibleAbilities` as a
**config** array of ability classes — fixed order, so the index is a stable ini
key — and `ActiveAbility` is already a `Class<Ability>` rather than an instance,
so there is no `UObject::Class` hunt at all. M6-S4.

**The frame-structure dump moved off Numpad 9 to F3.** It shared that key with
the grip tuning cycle and wrote ~400 lines to disk per press — 2012 in one
measured session. Tolerable when the cycle had three modes; not once it had five.
