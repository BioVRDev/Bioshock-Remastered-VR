# HUD capture and draw classification

`Hud/DrawHook.cpp` (2153). **Read this before changing anything in the capture path.**
More time has been lost here than anywhere else in the project.

## What it does

Hooks `DrawIndexed` (slot 12), `Draw` (13), the instanced variants (20, 21) and
`OMSetRenderTargets` (33). It watches render-target changes to find the moment
the world composite finishes, then redirects the subsequent Scaleform/GameSWF
interface draws to a private render target with its own D24S8 depth-stencil.
That target is post-processed to repair alpha and submitted by `XRSession` as a
separate OpenXR quad.

```
world ─▶ scene target
scene composite / tonemap ─▶ LDR / backbuffer
subsequent GameSWF draws ─▶ redirected to the HUD target   ← the capture
HUD target ─▶ alpha-repaired texture ─▶ OpenXR quad
```

The private D24S8 matters: Flash uses stencil for masking. Blend correction must
preserve mask-sensitive destination-alpha blends rather than rewriting state
indiscriminately.

## The duplicate-world square — solved, and how

A flat opaque panel appeared in front of the player during certain scripted
moments (reproducibly, during the Little Sister rescue transformation).

The decisive early A/B was that `HudRedirect=0` removed it, which proved the HUD
capture was involved. **Three subsequent theories about the mechanism were all
wrong** — see `docs/INVARIANTS.md` for why each died.

What finally answered it was one throttled log line on the captured draw:

```
normal gameplay   CAPTURED: 5d  tex=no     ← health / EVE bars
during the square CAPTURED: 6d  tex=yes    ← a textured full-screen quad
```

Seventeen consecutive samples of ordinary play showed only `5d tex=no`. The
interface is **untextured GameSWF geometry**; the square was a **textured
full-screen quad** landing in the same one-draw-per-frame capture slot.

The fix is one term, in the redirect condition (grep `PSSrv0Res`):

```cpp
if (g_hudRedirect && g_hudGateOpen && g_hudClearedThisFrame &&
    PSSrv0Res(ctx) == nullptr &&                       // ← the guard
    kind == KIND_DRAW && g_hudRTV && g_hudDSV)
```

It works in every scene, with no timers, no gates, and no cutscene detection —
which is why it survived when four timing-based approaches did not. **Keep the
measured comment above it when this code moves.**

### Why the square only appeared on the quad

The effect is a low-alpha full-screen overlay. Blended over opaque world pixels
on the backbuffer it reads as nothing. Captured onto a freshly-cleared
*transparent* target it has nothing to blend against, and the alpha repair
(`alpha = max(existingAlpha, max(r,g,b))`) then forces it opaque.

### If a new artifact appears

Re-add the `CAPTURED:` diagnostic (throttled to 1 Hz, in the capture branch,
logging `count`, `KindSuffix(kind)` and whether `PSSrv0Res(ctx)` is non-null),
reproduce, and diff the signatures between normal play and the artifact. That is
one play session and it names the draw. Do not start from theory.

## Alpha repair

Health and EVE bars once rendered as thin slivers. The A/B that settled it was
setting `hq.layerFlags = 0`: the HUD became a black rectangle *but the tubes
rendered correctly*, proving RGB and geometry were intact and alpha was the
failure. The fix processes the captured texture and reconstructs alpha before
the OpenXR copy, via `DrawHook_HudTextureForSubmit(ctx)`.

`hq.layerFlags = 0` was a **diagnostic**. Normal blend flags must stay restored.
Preserve the full D3D11 state save/restore and the per-frame processed guard.

`HudAlphaFix=0` is not a usable workaround for anything — it makes every other
interface element noticeably more transparent.

## The gate

`DrawHook_EndFrame` computes:

```cpp
const bool wantOpen = !(GameState_MenuUp() || GameState_Paused());
```

with 12-frame hysteresis. It has been cluttered and reverted several times; keep
it minimal. **Closing the gate does not hide anything** — un-redirected draws go
back to the backbuffer and land in the eye image at screen scale. The gate
chooses *where* the interface renders, never *whether*.

⚠ **`g_indexedThisFrame` is reset inside `DrawHook_EndFrame`, before the
fingerprint path's `g_gameplayConfirmed` block reads it.** Any counter borrowed
across those two functions reads zero. This cost two builds.

### What the gate cannot see, and the two terms that cover it

The gate only knows about pausing. Two kinds of screen need the capture off
while the game is emphatically *not* paused, so both are terms in the redirect
condition rather than gate inputs:

```cpp
!GameState_SceneMovieUp() && !DrawHook_ComposedFrameUp() &&
```

- **`SceneMovies`** — the text belongs inside panels that are world geometry, so
  no head-locked surface can line up with it. Leave it where the game put it.
- **The composed-frame route** (`DrawHook_ComposedFrameUp()`, which owns
  `AnchorMovies`, `FollowMovies` and the empty draw-count list) — the panel shows
  the game's own composed frame, so lifting the interface layer out of that frame
  first empties the picture being shown.

**Measured, Build O, 2026-08-11.** With `Maps.swf` on top the capture took **3
draws per frame** (total 11129 → 11483 in a second); with `ingamemanual.swf` on
top it took **0**. The tester saw exactly that: a correctly placed, correctly
scaled, empty map beside a manual that read perfectly. The map's contents are
untextured GameSWF vector geometry and enter the capture; its paper, frame and
tabs are textured and stay behind. The anchored window ran a full minute in that
log without one `HUD GATE` line — the map does not pause.

While either kind of screen is up the health and EVE bars also stay in the game's
frame. For the composed-frame route that is where the panel shows them anyway.

## Other responsibilities

- **Cutscene bars** — detected structurally and suppressible. Textureless, so
  `PSSrv0Res` is null for them and the capture guard does not affect them.
  `DrawHook_CutsceneBarsActive()` exists and currently has no consumer.
- **Menu / anchor lists** — `MenuIndexCounts` routes full-screen menus by draw
  signature. `AnchorIndexCounts` was meant to route in-game UIs the same way and
  is **empty and staying empty**: counts false-fire during ordinary play. The
  in-game screens are routed by Flash movie NAME instead — `AnchorMovies`,
  `FollowMovies`, `SceneMovies` — read live off the playing-movie stack in
  `GameState.cpp`. `DrawHook_ComposedFrameUp()` is the single owner of "is this
  screen being shown as the game's own frame", read by both `Hooks.cpp` (which
  quad) and the capture condition (whether to redirect).
- **Fingerprint tooling** — Numpad `*` clears the table, `3` dumps it, `-` steps
  the isolate walker. The dump is the useful one; the walker cannot catch a rare
  short-lived effect (one candidate per press).

## Sections, and why there is no split

Sections, by banner text -- grep for these rather than a line number:
`the classifier` · `capture surfaces` · `ALPHA REPAIR` · `alpha correction` ·
`texture discrimination` · `the fingerprint table` · `suppression` ·
`isolate stepper` · `menu detection` · `ANCHOR list` ·
`THE COMPOSED-FRAME ROUTE` · `vertex-shader constant dump`.

**A split was considered and rejected** -- see `.planning/DECISIONS.md`. The
state is shared across the whole file.
