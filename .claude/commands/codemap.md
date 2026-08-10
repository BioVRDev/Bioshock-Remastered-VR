---
description: Re-scan the source tree and regenerate docs/CODEMAP.md so the index cannot drift from the code.
---

Regenerate `docs/CODEMAP.md` from the current state of the source tree.

This is the project's own equivalent of a codebase-mapping command. The codemap
is the always-loaded index that lets a session load *only* the module docs it
needs instead of reading whole source files to orient. That only works if it is
true, so run this after any structural change.

## Do this

1. **Measure.** For every tracked `.cpp`/`.h` under `BioshockVR/`, `OpenXRShim/`
   and `dxgiproxy/`, get the line count and the section-banner comments
   (`^// ----`, `^// =====`). Those banners are the real internal structure —
   the code is heavily commented and the banners mark the seams.
2. **Check the project files.** Read the `ClCompile`/`ClInclude` lists in both
   `.vcxproj` files. **Any source file not listed there is not compiled** — that
   is exactly how 1,911 lines of stale shim duplicates survived, including a copy
   containing the pre-fix SteamVR warp conversion that reads as a lost fix.
   Flag anything untracked by a project.
3. **Look for dead code.** For each module's public functions, grep for callers
   outside the defining file. Report anything with zero. (`Keybinds.cpp` is a
   known, deliberate case — see `.planning/DECISIONS.md`.)
4. **Rewrite `docs/CODEMAP.md`** preserving its existing shape: the threading
   model and frame-flow summary at the top, one block per module (responsibility,
   thread, entry points, state owned vs. read, and the "load `docs/modules/X.md`
   when…" trigger), and the symptom→doc table at the bottom.
5. **Update line counts and section-banner line numbers.** These are the first
   thing to go stale after a refactor, and a wrong line number in the index is
   worse than none — it sends the next session to the wrong place.
6. **Report drift** rather than silently fixing it: modules whose responsibility
   no longer matches its description, module docs that reference code that moved,
   new files with no doc, deleted files still referenced.

## Constraints

- Do not change source code. This regenerates documentation only.
- Do not invent structure. If a module's purpose is unclear, read its header —
  they are unusually good in this codebase — and say so if it is still unclear.
- Keep `docs/CODEMAP.md` at roughly 250 lines. It is loaded every session; detail
  belongs in the module docs, not the index.
