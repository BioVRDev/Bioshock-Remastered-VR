---
name: session-wrap
description: Update .planning/STATE.md, DECISIONS.md and ROADMAP.md so the next session starts from the real state instead of re-deriving it. Use when the user says they are stopping, wrapping up, or wants a handoff written.
---

# Session wrap

This replaces the handoff documents that used to be written by hand between chat
sessions. Those documents went stale — by `002a81a` they still listed the HUD
square as the open P0 months after it was fixed, and a session starting from them
would re-derive a falsified theory. Keeping these files honest is the whole point.

## Update `.planning/STATE.md`

- **Recently closed** — move anything that landed, with the *evidence* that proved
  it, not just the claim. "Fixed" without a measurement is how the handoffs rotted.
- **Open P1/P2** — add what was learned, including narrowing. "This approach is
  dead and here is the log line that killed it" is as valuable as a fix.
- **In flight** — tick off completed steps; leave partial work described well
  enough to resume cold.
- **Last verified in a headset** — which build, what route, what was seen.
  Including anything odd that was noticed and not chased.

Keep it scannable. It is read at the start of every session.

## Update `.planning/DECISIONS.md`

Append an entry for any decision with a rationale someone might otherwise
reverse. **Every retired approach gets the evidence that killed it** — a
falsified idea with no recorded reason comes back within a month.

Do not log routine implementation choices here. This is for decisions, not diffs.

## Update `.planning/ROADMAP.md`

Reorder if priorities moved. If something turned out cheaper or more expensive
than expected, say so — the ordering is by value per **headset test cycle**,
because that is the real cost here.

## Update `docs/` when the code moved

- New measured offset → `docs/ENGINE-MAP.md`, **with its provenance**: how it was
  measured and what trial ruled out the alternative.
- New settled fact or newly dead approach → `docs/INVARIANTS.md`.
- Structural change to a module → its `docs/modules/*.md`, and run `/codemap`.

## Check the docs did not drift

Whatever the session touched, confirm the relevant doc still matches the code.
Line references in `docs/CODEMAP.md` and the module docs are the first thing to
go stale after a refactor. `/codemap` regenerates the index.

## Then

Give the user a short summary: what landed, what is verified vs. assumed, and the
single most valuable next step. If something is untested in a headset, say so
plainly rather than implying it works.
