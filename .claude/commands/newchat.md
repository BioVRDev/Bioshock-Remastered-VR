---
description: Bring every planning and doc file up to date, then print a short prompt to paste into a fresh chat. Use when the user says they are stopping, wrapping up, ending the session, or wants a handoff written. Replaced the old session-wrap skill, which did a strict subset of this.
---

Close out this session so the next one starts from the real state instead of
re-deriving it, then hand the user a prompt to paste.

The point of this project's doc system is that a new chat needs almost nothing
said to it — `CLAUDE.md` auto-loads and routes to `docs/CODEMAP.md`, and
`.planning/STATE.md` holds the current focus. This command's job is to make sure
that is *true* before the session ends.

## 1. Update the planning files

**`.planning/STATE.md`** — the file a new session reads first.
- Move anything that landed into **Recently closed**, with the *evidence* that
  proved it. "Fixed" with no measurement is how the old handoffs rotted.
- Update **Open P1/P2**. Record narrowing as well as fixes — "this approach is
  dead and here is the log line that killed it" is as valuable as a solution.
- Update **In flight**: tick off what completed, describe partial work well
  enough to resume cold.
- Update **Last verified in a headset**: which build, what route, what was seen,
  including anything odd that was noticed and not chased.
- **Set a clear next step.** One or two sentences naming the single most
  valuable thing to do next and why.

**`.planning/DECISIONS.md`** — append an entry for any decision someone might
otherwise reverse. Every retired approach gets the evidence that killed it.
Skip routine implementation choices; this is for decisions, not diffs.

**`.planning/ROADMAP.md`** — reorder if priorities moved. Ordering is by value
per *headset test cycle*, because that is the real cost here.

## 2. Check the docs did not drift

- New measured offset → `docs/ENGINE-MAP.md`, **with its provenance**: how it was
  measured and what trial ruled out the alternative.
- New settled fact or newly falsified approach → `docs/INVARIANTS.md`.
- Module changed shape → its `docs/modules/*.md`.
- Structure changed → run `/codemap`; line numbers and counts in the index are
  the first thing to rot.
- Any comment made false by this session's work → fix it now (`docs/STYLE.md`).

**README drift check.** `README.md` is the only user-facing document, so it goes
stale silently — nobody working on the mod reads it. Flag it for `/readme` if any
of these changed this session: the packaged file list, `Setup.bat`'s behaviour,
runtime or headset support, the tuning keys, or a feature moving between
"working" and "open" in `STATE.md`. Do not run `/readme` every session; do say
when it is due.

## 3. Check the repo is clean

- `git status` — nothing uncommitted that should be committed.
- Everything committed with a message that explains *why*, not just what.
- Ask whether to push if there are unpushed commits.
- The deployed DLL in the game folder matches the current build, or say plainly
  that it does not.

## 4. Print the prompt

Output a short block for the user to paste into a fresh chat. It should be
**three lines or fewer** — if it needs to be longer, the session card and
`STATE.md` are not doing their job and should be fixed instead of compensated for.

**If the next step is a session card** (the normal case while a milestone is in
flight — check `.planning/ROADMAP.md` § *Current arc*), name it and nothing else.
The card carries the specification, the read list and the falsifiable outcome, so
the prompt does not have to. `CLAUDE.md` auto-loads and carries the rest:

```
Read .planning/sessions/M<n>.md § M<n>-S<n> and run that session in plan mode.
```

**If there is no open card** — a milestone just closed, or the work is outside the
current arc:

```
Read .planning/STATE.md and pick up where we left off.
Current focus: <one line naming the next step>
```

Add one more line only for something genuinely unusual the files cannot carry — a
machine change, an uncommitted edit, a test that needs re-running before anything
else.

**Before printing it, check the card set is honest.** If the session that just
finished changed what the next card should do, update that card now. A stale card is
worse than no card, because the next session will follow it. If a milestone closed,
tick it in `ROADMAP.md` § *Current arc* and promote the next stub to a full card in
`.planning/sessions/`.

Then tell the user, in one sentence each:
- what landed this session,
- what is verified versus assumed,
- what the next session will start on.

Be honest about anything untested in a headset. Do not imply something works
when it has only been built.
