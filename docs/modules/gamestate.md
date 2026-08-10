# Game state and cutscene detection

`Game/GameState.cpp` (1166) and `Game/EngineExec.cpp` (273).

**This is the project's longest-standing unsolved problem.** Everything
downstream of a cutscene signal is already built and waiting for an input.

## What works

**`GameState_Paused()`** reads `Level.Pauser` (`level+0x668`) — null during play,
an object while paused. This is the game's own pause test and it is reliable.
It is the only game-state signal currently in production use.

**`EngineExec_Run()`** runs console commands through `UGameEngine::Exec`. `set`
writes the *class default*, so it survives pawn respawn, level change and save
reload — which is why the reticle kill uses it instead of suppressing a draw
signature. Three absolute addresses, INI-overridable, vtable-verified before the
call, silently disabled on mismatch.

## The console read channel (new, and its limit)

`Exec` reports results through an `FOutputDevice`. The original stub was a no-op
that discarded everything, so `get` always appeared to return nothing.

Solving it took four cheap measurement rounds rather than reasoning:

1. Count the calls → the device *was* being called, exactly once per `get`.
2. Dump the arguments → same two values (`0x2F8`, `1`) regardless of the property.
   Invariant arguments mean a **query**, not a write.
3. One thunk per vtable slot → **slot 4**, consistently.
4. Return `1` instead of `0` → the channel opened. Slot 1 then arrives with a
   real, per-command text pointer.

```
get ShockPlayer bReticleDisabled    -> [True]
get ShockPlayer HudElementsDisabled -> [False]
```

**The limit, proven:** `get` returns the **class default object**, not live
instance state.

```
get ShockPlayer Health   -> 200.000000   (unchanged after taking damage)
get ShockPlayer Location -> (X=0,Y=0,Z=0) (unchanged after walking)
```

So it reads defaults usefully and runtime state not at all. `GETALL` is
unhandled on this build. Keep the channel — it is genuinely useful for defaults
and for confirming `set` — but do not build a detector on it.

## Why cutscene detection is stuck

`GameState_Cutscene()` can essentially never return true. `g_cutscene` is written
in only three places: the ViewTarget divergence path (demoted to diagnostic and
no longer writing), `GameState_PitchSample` (telemetry, no latch), and the reset
paths. `GameState_Theater()` therefore falls back to a name whitelist that
requires a context lock that has never happened.

**The context scan has never locked.** It brackets the window correctly
(`pawn+0x728..+0xA7C`, anchored on `FirstPersonHands.PlayerHands`), matches a wall
of known localized constants, and watches a long candidate list — then nothing.
Zero `>>> CONTEXT` lines in any session on record.

The decompile explains why the design *should* work:

```unrealscript
function BeginExorcisingGatherer(BaseShockAI theGatherer, bool PacifyHer)
{
    CurrentExorcismTarget = theGatherer;
    Level.GetFlashGUIController().HideMovie('HUD');
    Controller.ConsoleCommand("PUSHINPUTCONTEXT NullInput");
}
```

`NullInput` is already mapped to `CTX_SCRIPTED` in `kContexts`. The model is
correct; the value never arrives. `LastPlayerInputContext` is declared
`var private travel string` and nothing in the decompile writes it from script,
so it is presumably native — and possibly inert on this build.

Note `HideMovie('HUD')`: the real HUD is hidden for the whole rescue, so draws
reaching the capture during that window are not interface at all.

## Approaches that failed

See `docs/INVARIANTS.md` § *Cutscene detection* for the full list with evidence:
ViewActor divergence, pitch-rate latching, the pitch servo, S75/S78/S79 unwind,
cached view-target scans, and the input-ignored detector.

## The two live leads

**1. `CurrentExorcismTarget` — a pointer, not a string.** Set at the start of the
rescue and cleared at the end, bracketing the entire sequence including the
transformation. It is a member of `ShockPlayer`, the pawn already held. Finding
the offset is the standard differential trick: snapshot every pointer-shaped slot
on the pawn, run a rescue, log the `null → object` transitions.

A probe was built and produced 18 transitions, but the run contained no rescue
and `g_exPrev` was not reset on pawn change, so the pawn-lock burst polluted it.
Two candidates survived as plausible: `+0xEA4` (8.0-second span, complementary
with `+0x75C`, consistent across two sessions) and `+0xB58` (closest to the
reported start, but also moves during ordinary play). **Unresolved by probing.**

**There is now a cheaper route.** `research/uscript/ShockGame/Classes/ShockPlayer.uc`
declares it at line 348:

```unrealscript
var BaseShockAI CurrentExorcismTarget;
```

UE2 lays out properties in declaration order, so the offset can be *computed* by
anchoring on a measured field and walking the declarations forward, instead of
hunting for it. See `docs/UNREALSCRIPT.md`. Verify the result against a live read
before trusting it — and note it can also disambiguate `+0xEA4` vs `+0xB58`
rather than replacing that evidence.

This is narrow — it solves the rescue, not cutscenes generally.

**2. The UE2 reflection bridge** — `docs/proposals/ue2-reflection-bridge.md`.
Walk the `UClass` property chain to resolve any named property's offset at
runtime, validated against the known `Location = 0x1D8`. This is the general
answer and it is a project, not a session.

## Practical notes

- A marker key is invaluable: the logs are timestamped and the tester cannot see
  timestamps live. A key that logs `>>> ===== MARK =====` lets any visual event
  be pinned to a log line. `VK_END` is free.
- The `ContainerUIActive` context is deliberately mapped to `CTX_GAMEPLAY` — it
  was previously classified as a menu, which closed the HUD redirect merely on
  *approaching* a lootable crate.
