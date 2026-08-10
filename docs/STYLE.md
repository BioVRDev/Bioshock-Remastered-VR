# Style

This codebase already has a strong, consistent style. This file writes it down so
it does not drift — it is a description of what is there, not a new convention
being imposed.

Measured across the 14,244 lines of `BioshockVR/`:

| Convention | Consistency |
|---|---|
| Allman braces (`{` on its own line) | 298 vs 2 |
| 4 spaces, never tabs | 100% |
| `#pragma once` | 13 of 13 headers |
| Lines within 80 columns | 94% |
| Comment density | 20% |

## There is no auto-formatter, deliberately

A `.clang-format` was written and tested against the tree. Even tuned to be as
permissive as possible it wanted to change **37% of all lines**, and every
disagreement was one where the hand-formatting is better:

```cpp
extern void  LogFile(const char* msg);   // aligned across the block; it collapses this
va_list a; va_start(a, fmt);             // one idiom, one line; it splits this
(int)(x)                                 // it wants (int) (x)
```

Worst of all it aligns wrapped arguments to the opening parenthesis, pushing
code past column 40 and out of the 80-column budget the rest of the file keeps.

`.editorconfig` handles the mechanical parts — indentation, encoding, line
endings, trailing whitespace. Layout stays human.

**Do not bulk-reformat.** Beyond destroying the above, it would break `git blame`
on code whose history is often the only record of *why* it is shaped that way.

## Formatting

- **Allman braces.** Opening brace on its own line, for functions and blocks.
- **4 spaces.** No tabs, anywhere.
- **80 columns.** Not absolute — a few lines run to 100 where breaking them would
  hurt — but it is the default, and it is what makes side-by-side diffs work.
- **Hanging indent for continuations**, 4 or 8 spaces. Do not align to the
  opening parenthesis.
- **Align trailing comments** into a column within a block. This is why some
  declarations carry two spaces before the type.
- **One idiom per line, not one token.** `va_list a; va_start(a, fmt);` stays
  together because it is a single thought.

## Naming

| Kind | Convention | Example |
|---|---|---|
| Public API | `Module_Function()` | `CameraHook_Install`, `XR_SubmitPair` |
| File-local state | `g_` + camelCase | `g_aimBase`, `g_hudHost` |
| Compile-time constants | `k` + PascalCase | `kAimOffsets`, `kPosSide` |
| File-local functions | `static` + PascalCase | `FindCalcView`, `NoteDraw` |
| Config fields | camelCase on `g_cfg` | `g_cfg.hudRedirect` |
| Locals and parameters | camelCase | `headPos`, `displayTime` |

The `Module_Function()` form is load-bearing: with 403 globals and no namespaces,
a bare `Install()` would be meaningless. The prefix names the owner.

## Headers

- `#pragma once`, never include guards.
- First line is the path: `// BioshockVR/<Folder>/<File>.h`.
- **Forward-declare instead of including** where a pointer will do. `DrawHook.h`
  forward-declares `ID3D11Texture2D` rather than pulling `<d3d11.h>` into every
  consumer, and says so in a comment.
- Includes are folder-qualified: `#include "Core/Config.h"`. Order encodes
  layering, not alphabet — do not sort them.

## Comments — the part that actually matters

This is the codebase's real asset, and it is why a 2,600-line file is navigable.
The rule is not "comment more". It is:

> **A comment carries the measurement, the reason, or the falsified alternative.
> Never a restatement of the code.**

Five patterns worth keeping:

**1. Record the measurement, with its number.**
```cpp
// MEASURED: CalcView runs on the GAME thread, Present runs on the RENDER
// thread. They are different threads with a pipeline between them, so Present
// CANNOT simply flip a flag and expect the next CalcView to see it.
```

**2. Record what was tried and failed, and why.** This is what stops a dead idea
coming back in three months.
```cpp
// HISTORICAL CORRECTION: this comment used to claim the resulting artifact WAS
// the flat "second copy of the world" square. It was not.
```

**3. Say when something is a diagnostic**, so nobody mistakes it for shipping
behaviour. `hq.layerFlags = 0` was an alpha probe that lived on as folklore.

**4. Annotate the thread.** Every cross-thread channel says which side writes and
which reads. Getting this wrong is the most expensive class of bug here.

**5. Mark the guard rails as guard rails.**
```cpp
// Bone 43 is untouchable -- telekinesis release walks the attachment path
// through it and moving it crashes the game.
```

Section banners divide long files: `// ---- name ----` for sections,
`// ====` boxes for anything a reader must not miss. The module docs index these,
so keep them meaningful.

**When you change code, check the comment above it is still true.** The refactor
found four comments that had quietly become false — including a header that
described a scan as working when it has never once succeeded.

## Discipline

These are enforced by review, not by tooling, and each was learned expensively.
Full list with evidence in `docs/INVARIANTS.md`.

- **Fail closed.** Verify a prologue, a vtable, an object identity before
  writing. A refused feature beats corrupted game state.
- **New behaviour ships behind an INI switch, default off.**
- **No per-frame memory scans.** One-shot, lock, stop; back off on retry.
- **Measure before theorising.** Every long dead end here came from reasoning
  about what the code should do instead of logging what it did.
- **Reset lifetime, not range.** Clear cached state at pawn, level and save
  boundaries rather than capping a value to hide staleness.
