# Proposal: UE2 reflection bridge

**Status:** designed, not scheduled. Parked as the general answer to reading live
engine state.

Fallback route for reading **live** property values out of BioShock Remastered,
after `GET <class> <prop>` was proven — four probes, 93 s apart, while moving — to
return the **class default object** only:

```
get ShockPlayer Health   -> 200.000000    (unchanged after damage)
get ShockPlayer Location -> (X=0,Y=0,Z=0) (unchanged after walking)
```

Do this **only** if an executable string scan finds no cheaper door
(`PUSHINPUTCONTEXT`, `GETALL`, `EDITACTOR`).

---

## Why not ProcessEvent / GetPropertyText

The obvious route is `UObject::ProcessEvent` → `GetPropertyText`. It works in
principle, but it requires locating `ProcessEvent` in this UE2.5 fork, resolving
a `UFunction*` by name, building a parameter frame with the exact `FString`
layout, and getting return-value marshalling right. Every one of those fails by
**crashing**, not by logging. Four unknowns, all fatal, all at once.

**Read the metadata instead.** Unreal stores every property's byte offset in its
`UClass` chain. Walking that chain is pure pointer reading — every step guarded,
and a wrong guess yields a bad number rather than a crash. Same end result: a
runtime-resolved offset for any named property, valid on Steam and Epic without
separate values.

## The key advantage: we already know some answers

`HandsProbe` has measured, on this build:

```
Actor Location = +0x1D8
Actor Rotation = +0x1E4
```

So the layout solver does not have to be clever. It brute-forces candidate
layouts and **keeps only the one that reproduces those offsets**. That is a
self-validating search — it cannot silently settle on a wrong layout, because a
wrong layout will not produce a known-correct answer.

Run once, log the winning parameters, then hardcode them as INI-overridable
defaults exactly as `EnginePtrRva` already is.

## Structures being solved for

```
UObject
  +0x00              vtable
  +kObjClassOff      UClass*             <-- unknown 1

UClass (is-a UStruct, is-a UField)
  +kChildrenOff      UField* Children    <-- unknown 2  (head of property list)
  +kSuperOff         UStruct* SuperField <-- unknown 3  (parent class)

UField
  +kNextOff          UField* Next        <-- unknown 4  (linked list)
  +kNameOff          FName Name          <-- unknown 5

UProperty (is-a UField)
  +kPropOffsetOff    int32 Offset        <-- unknown 6  (the value we want)
```

Six unknowns, but not independent — the oracle collapses them.

## Stage 1 — the layout solver

New file `EngineRefl.cpp`. Runs **once**, on a key press, in gameplay, on the
game thread (from `GameState_Observe`, same as the existing probes). Read-only;
every dereference guarded. Worst case it finds nothing and logs that.

The search: for each candidate `UClass*` offset in the first `0x40` bytes of the
pawn, brute-force `children`, `next` and `propOffset`, walk the resulting chain,
and accept only a layout whose property `Offset` values **contain both `0x1D8`
and `0x1E4`** and yield at least 8 fields.

Cost: 16 × 32 × 16 × 24 ≈ 200k candidate walks worst case, each bailing on the
first bad pointer. One keypress; a visible hitch is acceptable.

**Note on `SuperField`:** `Location` is declared on `Actor`, not `ShockPlayer`, so
the chain may need to walk up the superclass list before it becomes visible. If
stage 1 reports "no layout", add `kSuperOff` to the brute force and follow
`SuperField` when `Children` runs out. That is unknown 3, deliberately deferred —
UE2 often flattens the property chain, so try without it first.

## Stage 2 — resolving a property by name

Stage 1 gives offsets; asking for `"LastPlayerInputContext"` by name needs each
field's `FName` turned into text. **The technique already exists in this
codebase**: `CameraHook.cpp`'s `FindCalcView()` stages 1–3 locate a wide string
in module memory and find the `FName` index global the engine stores for it.

1. Find the wide string `L"LastPlayerInputContext"` in module memory.
2. Find the `PUSH <addr>` xref, then the `MOV [imm32], ECX` that stores the
   resolved `FName` index.
3. Read that global — it is the name index.
4. Walk the property chain comparing `*(int32*)(field + L.name)` against it.
5. The matching field's `Offset` is the answer.

Solve `L.name` the same self-validating way: it is whichever field position makes
the property at `Offset == 0x1D8` match the name index for `L"Location"`.

## Stage 3 — reading the value

`LastPlayerInputContext` is `var private travel string`, i.e. an `FString`:

```cpp
struct FString { wchar_t* Data; int32 Count; int32 Max; };
```

Read `Data`, bounds-check `Count` (reject `<= 0` or `> 1024`), verify the buffer
is readable, and copy.

**Validate with a property you can verify by eye before trusting any of it**: read
`Health` through this path and confirm it tracks damage. If it does, the bridge is
real. This step is what separates "the bridge is broken" from "the property is
empty" — do not skip it.

## Stage 4 — wiring it

- Poll 2–10× per second from `GameState_Observe` (game thread, already correct).
- Publish through the existing atomic/seqlock pattern.
- Feed the existing `ClassifyContext` / `kContexts` table in `GameState.cpp`,
  which is already written and has never had a working input.
- `GameState_Theater()` then returns real answers, and the HUD gate, the cutscene
  anchor and the comfort work all light up with no further changes.

## Order of attack

1. **String-scan the exe first** — `PUSHINPUTCONTEXT`, `GETALL`, `EDITACTOR`. Free,
   no build, and any hit is far cheaper than this document.
2. Stage 1 solver. One keypress, one log line, decides everything.
3. Stages 2–4 only if stage 1 reports SOLVED.

Stop at the first thing that works.

## Honest risk assessment

- **Stage 1 is safe.** Read-only, guarded, worst case logs a failure.
- **Stage 2 is the real work.** The FName index hunt is fiddly, though
  `FindCalcView` proves the pattern works on this executable.
- **The whole thing may still dead-end** if `LastPlayerInputContext` is never
  written by this build — the memory scan already suggests it may be inert.
  Stage 3's `Health` validation is the discriminator.

This is a project, not a session.
