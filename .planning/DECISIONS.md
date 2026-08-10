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
