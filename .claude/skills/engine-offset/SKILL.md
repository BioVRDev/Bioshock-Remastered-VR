---
name: engine-offset
description: The procedure for finding, validating, and trusting a new engine memory offset or hooked function in BioShock Remastered. Use whenever work needs a new address, offset, field, or pattern scan, or when deciding whether an existing offset can be trusted on another storefront build.
---

# Adding an engine offset

This engine is a UE2.5 fork with **no reflection system**. Nothing can be read
out of an SDK; every address here was measured. A wrong offset does not produce a
glitch — it produces a bone matrix full of garbage, or a crash on a code path you
did not know existed.

Existing measurements: `docs/ENGINE-MAP.md`.

## 1. Prefer a pattern to an address

Hardcoded RVAs have failed across every storefront. The `CalcView` FName/string
chain found its function on Steam, Epic **and** GOG; the delta-function signature
finds zero matches on GOG; `EnginePtrRva` is Steam-only, which is why the reticle
Exec fails on Epic and GOG.

Prefer, in order:
1. A string/FName chain to code that references the thing.
2. A byte signature, verified to match **exactly once**.
3. A derived reference (walk from something already found).
4. An absolute address — only with an INI override and a verification check.

## 2. Predict before you hunt

**Read the decompiled script first.** `research/uscript/` holds 1,765 decompiled
classes — see `docs/UNREALSCRIPT.md`. It has repeatedly answered in minutes what
memory scanning could not answer in sessions.

The best offsets in this project were predictions, not searches.
`AActor::Location = +0x1D8` came from script saying `GetViewRotation()` returns
`Rotation + …` — so `Rotation` is the actor member already being written at
`+0x1E4`, and UE2 puts `Location` (12 bytes) immediately before it. That is one
comparison to confirm, not a scan.

**UE2 lays out properties in declaration order**, so a `.uc` file gives you the
field *sequence* a blind scan lacks. Anchor on a measured offset, walk the
declarations forward, add sizes: 4 for float/int/pointer/object-ref, 8 for
`FName`, 12 for `FVector`/`FRotator`/`FString`/`TArray` — and watch `bool`,
which packs into a shared bitfield rather than taking 4 bytes each.

That turns an offset hunt into arithmetic. It is still a prediction: verify it
against a live read before writing.

## 3. Design a test the wrong answer fails

This is the part that matters, and it is where this project's good measurements
differ from its bad ones.

- **Use a pair constraint.** The Hands actor was identified by matching the camera
  position *and* the view rotator simultaneously — far stronger than either alone.
- **Use an oracle.** The reflection-bridge design brute-forces layouts and keeps
  only the one that reproduces the already-known `Location = 0x1D8`. It cannot
  settle on a wrong layout by construction.
- **Design against the obvious confounder.** Acceleration at `+0x5C0` was
  confirmed by holding the stick **while pinned in a corner** — provably not
  moving, yet the field still read ~875. That single trial is what proves it is
  the input *request* and not velocity, and it is why walking into a wall cannot
  false-fire it.

Ask: what else could produce this reading? Then build the trial that separates them.

## 4. Fail closed

Verify before every write, not once at startup:
- the pointer is readable (`Readable(p, n)` / `IsMemoryValid`),
- the object is the object you think (back-pointers are ideal — the holdable's
  `+0x450` points back at Hands),
- structural invariants hold (`ArmHide` demands **exactly 47 bones** and two
  matching vtables, then writes nothing on any mismatch).

A refused feature beats corrupted game state. Log the refusal.

## 5. Make it overridable and never scan per frame

Any address not derived by pattern gets an INI key (`EnginePtrRva`,
`ArmHideHandsVt`, `ArmHideSkelVt` are the precedents) so a new build can be fixed
without a compiler.

**Never add a per-frame memory scan.** One-shot scans that stop after locking are
fine; use backoff for retries. Reset all probe state through a real reset path on
pawn/level transitions — a stale snapshot compared against a new object reports
every differing field as a transition, which has already produced one misleading
candidate list.

## 6. Record it

Add the offset to `docs/ENGINE-MAP.md` **with its provenance** — how it was
measured and what trial ruled out the alternative. An offset without that is a
number nobody can later audit.

## The differential technique

For "find the field that changes when X happens": snapshot a window of the object,
perform X, diff. Existing implementations scan floats (`SnapshotFloats` /
`DiffFloats` in `GameState.cpp`, PgUp/PgDn) and pointer-shaped slots.

Three cautions, all learned the hard way:
- **Reset the snapshot when the object changes.** Comparing a new pawn against an
  old pawn's snapshot reports a burst of false transitions.
- **Let the scene settle first.** ~8 slots move during ordinary play; the event
  must be the loudest thing in the window.
- **Add a marker key.** The tester cannot see timestamps live, so a key that logs
  `>>> ===== MARK =====` is what lets a visual event be pinned to a log line.
