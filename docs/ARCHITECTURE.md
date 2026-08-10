# Architecture: EngineBridge and StateBus

What the decompiled script corpus revealed, and the design that follows from it.
Execution order in `.planning/ROADMAP.md` § *Current arc*.

This is the "validated engine memory-accessor layer" that `.planning/DECISIONS.md`
(2026-08-09, *layered architecture deferred*) left the door open for. It arrives
because a feature needs it, not as a standalone refactor.

> **Copyright note.** `research/uscript/` is 2K's content and is gitignored. This
> file is tracked, so it cites **declarations, names, offsets and file:line
> provenance only** — never bodies at length. Keep it that way.

---

## The problem it solves

The mod cannot answer *"am I in a cutscene right now"*. Four features are written,
compiled and inert waiting for that answer: the HUD gate, the cutscene anchor,
`ModYaw`/`FreezeGameRotation` rotation comfort, and right-stick look during
scripted sequences.

Eight approaches are falsified (`docs/INVARIANTS.md` § *Cutscene detection*).
Every one of them inferred state from a side effect — pitch rate, draw counts,
view-target identity, timing. **The design here reads the state the game itself
keeps**, which is the one class of approach that has not been tried.

---

# Part 1 — the findings

**Dated 2026-08-09.** Source: `research/uscript/` (see `docs/UNREALSCRIPT.md`).

Every document in this project has said, in effect: *BioShock's UE2.5 fork has no
reflection system, so reading a named property off a live object requires building
one from metadata.* **That is not what the corpus shows.** Three findings, in
ascending cost. Each is independently sufficient — stop at the first that answers
the question you have.

## Finding 1 — `myHUD.bHideHUD` is an exact cinematic-mode flag

`Scripting/Classes/ActionCinematicEnter.uc` calls native `cinematicEnter()` then
sets `myHUD.bHideHUD = true`, reached via
`PlayerController(parentScript.Level.GetLocalPlayerController().Pawn.Controller)`.
`ActionCinematicExit.uc` is the mirror: `cinematicExit()` then `bHideHUD = false`.

**`Engine.HUD.bHideHUD` (`Engine/Classes/HUD.uc:84`) is written from exactly those
two places in all 1,765 classes.** The three other grep hits are:

| Hit | Why it does not matter |
|---|---|
| `ShockGame/Classes/ShockCheatManager.uc:8` | A **different** bool of the same name on a different class |
| `Engine/Classes/HUD.uc:205, 371, 418` | `AddDebugLine`/`Cone`/`Sphere` set it false — debug helpers, not called in retail |

**Why it is cheap:** `myHUD` is `Engine/Classes/PlayerController.uc:115`, and
`CameraHook` already holds the PlayerController — it is `pThis` in the
`eventPlayerCalcView` detour (`docs/modules/camera.md`, anchor `the detour`). Two
pointer hops and a bit test off a pointer the mod has every frame. No scan, no new
hook. Failure mode is a wrong number, never a crash.

**What it does not cover:** scripted sequences that only push an input context.
See finding 3.

## Finding 2 — the engine has a native live-property reader

`Core/Classes/Object.uc` declares four natives (empty bodies):

| Line | Declaration |
|---:|---|
| 1886 | `function string GetPropertyText(string PropName)` |
| 1891 | `function string GetPropertyTextByName(name PropName)` |
| 1896 | `function SetPropertyText(string PropName, string PropValue)` |
| 1902 | `function SetPropertyTextByName(name PropName, string PropValue)` |

**Retail script calls these on live instances.** `Scripting/Classes/Script.uc:366`
copies a message by round-tripping each property through
`GetPropertyTextByName`/`SetPropertyText`; `Script.uc:775` reads a filter property
the same way; `Scripting/Classes/ActionGetProperty.uc:22` calls
`A.GetPropertyTextByName(Property)` on an actor resolved by label at runtime.

**What this corrects.** `docs/INVARIANTS.md` records — correctly — that console
`get` returns the class default object: `Health` stayed `200.0` after damage,
`Location` stayed `(0,0,0)` after walking. **That measurement stands. The
conclusion drawn from it did not.** `get` reads defaults because the console
command resolves a *class*. It is not evidence that live instance reads are
impossible; the engine exposes exactly the accessor needed, and the console is
simply the wrong caller.

This retires `docs/proposals/ue2-reflection-bridge.md` as the *first* move.
Nothing in it is wrong and none of it is deleted — the metadata walk becomes
Tier 2.

## Finding 3 — input context is a second, separate signal

`LastPlayerInputContext` is declared **twice**:

| Class | Line | Declaration |
|---|---:|---|
| `ShockGame.ShockPlayer` | 136 | `var private travel string LastPlayerInputContext` |
| `ShockGame.ShockPlayerController` | 42 | `var private string LastPlayerInputContext` |

**Only the pawn has ever been scanned** — window `pawn+0x728..+0xA7C`, recorded as
`pawn+0x934`, never locked in any session. The controller copy has never been
examined. It is a plain `string` rather than `travel`, consistent with being the
live working copy while the pawn's is persisted through level travel. **A lead, not
a conclusion.**

Contexts change via `ConsoleCommand("PUSHINPUTCONTEXT <name>")`/`POPINPUTCONTEXT`
from at least: `ShockPlayer.uc` (2442, 2488, 4894, 4941, 6802) ·
`ShockPlayerController.uc` (293, 1332, 1340, 1348, 1358, 1988) ·
`ShockAI/Atlas.uc:581` · `ShockAI/AtlasCommanderAction.uc:275` ·
`ShockGame/ResearchCamera.uc` (59, 108) ·
`ShockGame/BaseResurrectionStation.uc:84` ·
`ActionSetOrUnsetInputContext.uc` · `AnimNotify_SetOrUnsetInputContext.uc`.

So `PUSHINPUTCONTEXT` is a **native Exec handler**, and the context names in script
(`NullInput`, `RADIALACTIVE`, `InResurrectionStation`, `PhotoGradingUIActive`,
`BathysphereUIActive`) are several of the ones `kContexts` already classifies.

**The consequence: the Little Sister rescue does not use cinematic mode.**
`ShockPlayer.uc:2442` pushes `NullInput`. Cinematic mode and input context are two
different signals and the project needs both — finding 1 will not detect the
rescue, and that is expected rather than a failure.

## Also worth knowing

- **`Scripting` is the scripted-sequence package** (99 classes) — what level
  designers actually built cutscenes *with*, so it is the authority on how any
  scripted moment is constructed.
- **`ActionCinematicFadeView`** exists alongside enter/exit — relevant to the
  cutscene-bars suppression in `Hud/DrawHook.cpp`.
- **`ActionGetProperty`/`SetProperty`/`PropertyTest`** mean level scripts read and
  write arbitrary properties by name at runtime — further confirmation that the
  finding-2 accessors are live code, not vestigial declarations.
- **`ActionPlayMovie`, `ActionOpenMenu`, `ActionChangeLevel`,
  `ActionSetPlayerInvincibility`** — other bracketing actions if cinematic mode
  proves too narrow.
- **`ShockPlayerController.bIsForcingPlayerMove`** (`:47`, beside
  `ForcePlayerMoveTargetLocation`) — the obvious Tier 0 candidate if a forced-move
  signal is ever needed.

## Status of each finding

| Finding | Status |
|---|---|
| 1 — `bHideHUD` written only by cinematic enter/exit | **Corpus claim verified. Live behaviour FALSIFIED (M1-S2).** Offsets measured and confirmed (`controller+0x71C`, bool DWORD `+0x490`), and the bit never moves — not even while the HUD visibly hides. The script really does write it; the retail sequences do not run through those actions. See `docs/INVARIANTS.md`. |
| 2 — natives exist, script calls them on instances | **Verified in the corpus, and the addresses are now MEASURED LIVE (M3-S1, 2026-08-10).** Natives are registered by an `int<Class>exec<Func>` symbol string, one per 12-byte row of a table in `.data`; `GetPropertyTextByName` is at rva `0x7346E0` on Steam. **Calling is still untried**, and the signature is `exec`-style — it takes an `FFrame`, not a string. `docs/modules/enginebridge.md`. |
| 3 — the controller's copy was never scanned | **Verified** it is declared and unexamined. That it holds live data is **inference**. |

**Nothing here is confirmed in a headset.** All three are predictions from script
until a live read agrees — the standard that produced `AActor::Location = +0x1D8`.

## What was deliberately not researched

A comparative survey of other VR mods (UEVR, REFramework, RealVR, HL2VR, SKSE) was
scoped then **skipped**: the corpus answered the question specifically and
decisively, and a survey would have cost far more context to land somewhere less
applicable to a 32-bit UE2.5 fork. Revisit only if Part 2 dead-ends.

---

# Part 2 — the design

## EngineBridge — three tiers, adopted in cost order

Each tier is independently useful and ships on its own. **Stop at the first tier
that answers the question you have.** Do not build a lower tier speculatively.

| Tier | Mechanism | Cost | Failure mode |
|---|---|---|---|
| **0 — offset read** | Validated pointer-chain reads off pointers already held | ~1 session | A wrong number. Never a crash. |
| **1 — native call** | Locate and call `Object::GetPropertyTextByName` | ~3 sessions | **A crash.** Must be gated. |
| **2 — metadata walk** | `docs/proposals/ue2-reflection-bridge.md`, unchanged | 6+ sessions | Read-only; worst case logs nothing |

### Tier 0 — the offset read

Read a field the game already maintains, off a pointer the mod already holds,
having verified the object's identity first.

This is what the mod does today in `HandsProbe` and `ArmHide`, and the discipline
is already established: derive or compute the offset, verify object identity
before touching it, make the offset INI-overridable, fail closed on mismatch.
Tier 0 makes it a named layer instead of a habit.

**The first target is `PlayerController.myHUD.bHideHUD`** — an exact
cinematic-mode flag, two hops from the `eventPlayerCalcView` `pThis`
(finding 1 above).

Offsets are established two ways and **both must agree**:

1. **Predict** by declaration order from a measured anchor
   (`docs/UNREALSCRIPT.md`). Watch bool bitfield packing — consecutive `bool`s
   share a DWORD, and that is the trap that will cost you a cycle.
2. **Confirm** by a self-validating live read — a back-pointer that must point at
   the object you started from, a value that must change when you make it change,
   a pointer that must be non-null in play. The same three-stage positional
   identification `HandsProbe` uses.

A prediction alone is never enough to write through. This is the standard that
produced `AActor::Location = +0x1D8`.

### Tier 1 — the native call

`Core.Object` exposes `GetPropertyTextByName(name) -> string` as a native, and
retail script calls it on live instances (finding 2 above). Calling
it gives live reads of any named property without a metadata walk.

Two hard constraints, because this tier can crash:

- **Locate by pattern, never by RVA.** M3-S1 found a better anchor than
  `FindCalcView`'s FName scan — the `int<Class>exec<Func>` registration symbol,
  which points directly at the function's own row in the engine's native table.
  Three stages instead of six; `docs/modules/enginebridge.md`. The row's RVA
  differs between Steam and Epic, so the rule stands unchanged.
- **Validate on a property you can check by eye before anything trusts it.**
  Read `Health` and confirm it tracks damage. This is the discriminator between
  "the bridge is broken" and "the property is empty", and skipping it is how this
  costs three cycles instead of one. Default-off INI switch until it passes.

### Tier 2 — the metadata walk

`docs/proposals/ue2-reflection-bridge.md`, unchanged and unrejected. Six coupled
unknowns collapsed by a self-validating oracle (a layout is accepted only if it
reproduces `0x1D8` and `0x1E4`). Read-only, so its worst case is a log line
saying it found nothing.

**Parked.** Open it only if Tier 1 fails. It stopped being the first move when
finding 2 showed the engine already exposes the accessor it was going to rebuild.

---

## StateBus — one owner for game state

Today `g_cutscene` is written from three unrelated places — a demoted ViewTarget
path, non-latching pitch telemetry, and the reset paths — and consumers each
decide for themselves what "in a cutscene" means. That is why a real signal cannot
simply be dropped in.

StateBus is deliberately small:

- **One writer**, on the game thread, from `GameState_Observe`. That is already
  the correct place — the existing probes run there.
- **Published through the existing seqlock pattern.** Never a loose flag across
  threads (`docs/INVARIANTS.md` § *Threading and frames*).
- **One predicate per question**, and each predicate names its source so a log
  line can say *why* it is true.
- **Reset at every lifetime boundary** — pawn null, level load, save reload. Fix
  lifetime, not range.

### The signals, and that there are two

Finding 3 is load-bearing for the design: **cinematic mode and input context are
different signals.** The Little Sister rescue pushes
`NullInput` and never enters cinematic mode.

| Signal | Source | Covers |
|---|---|---|
| `Cinematic` | Tier 0, `myHUD.bHideHUD` | Anything a level script brackets with `ActionCinematicEnter`/`Exit` |
| `ScriptedInput` | Tier 1, the controller's `LastPlayerInputContext`, via the existing `kContexts` table | Rescues, radial, vending, resurrection, photo grading, bathysphere |
| `Paused` | `Level.Pauser` (`level+0x668`) | Already works; the only game-state signal in production today |

`GameState_Theater()` composes them. Consumers ask the question they actually
mean — a HUD gate and a comfort setting do not want the same answer, and the
existing `ContainerUIActive`-classified-as-menu bug is what happens when they
share one.

---

## Rejected alternatives

Recorded so they do not come back. Fuller list in `docs/INVARIANTS.md`.

| Approach | Why not |
|---|---|
| **Metadata walk first** | Six coupled unknowns as the opening move, when Tier 0 may make it unnecessary. Kept as Tier 2, not deleted. |
| **Hook `ProcessEvent` / `CallFunction`** | Failure mode is a crash, not a log line, and `ProcessEvent` is unlocated in this fork. Four fatal unknowns at once — the original proposal ruled it out and the research gives no reason to revisit. |
| **More heuristics** — timing, pitch rate, draw counts, view-target identity | This is the graveyard. Eight entries, each with the evidence that killed it. `docs/INVARIANTS.md` § *Cutscene detection*. |
| **Console `get` as a state source** | Proven to read the class default object. Keep the read channel for defaults and for confirming `set`; never build a detector on it. |
| **`CutsceneTheater=1` as a general setting** | Falsified — forces *every* cutscene onto the flat quad. Correct only when gated on a real signal, which is what M2 does. |
| **Draw signatures gating behaviour** | Structurally forbidden. Signatures may control cosmetic presentation only; a false-positive draw count once froze turning. |

---

## Where it lives

New: `Game/EngineBridge.cpp` / `.h` (Tier 0 accessors, then Tier 1) and the
StateBus in `Game/GameState.cpp`, which already owns state and already runs on the
game thread. No new threads, no new hooks for Tier 0.

Documented in `docs/modules/enginebridge.md` as it lands, and folded into
`docs/CODEMAP.md` by `/codemap`.
