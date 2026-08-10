# EngineBridge — reading live engine state

`Game/EngineBridge.cpp` / `.h`. Tier 1 of `docs/ARCHITECTURE.md`.

**Status: M3-S1 PASSED, 2026-08-10. Locate only — nothing here calls into the
engine yet.** The scan is read-only, one-shot and gates no behaviour. M3-S2 is
the session that dares to call.

## The measured result

Two launches, identical in every offset, on Steam:

| Native | RVA | Prologue |
|---|---|---|
| `execGetPropertyTextByName` | `0x7346E0` | `55 8B EC 83 E4 F8 83 EC` |
| `execGetPropertyText` | `0x734640` | `55 8B EC 6A FF 68 18 87` |
| `execSetPropertyText` | `0x734840` | `55 8B EC 6A FF 68 A0 87` |
| `execSetPropertyTextByName` | `0x734940` | `55 8B EC 6A FF 68 B8 87` |

`S3 stride 12 measured, 8 of 8 neighbours are rows` · `4 of 4 accessors located,
4 distinct addresses` · scan cost 0–15 ms, once.

Three things make this more than a lucky pattern match. The rows resolved to
**four neighbouring functions inside `0x300` bytes** — one translation unit, as
`UnObj.cpp` would be. The live row landed at `0x11BE684`, **exactly** what static
analysis predicted from the file. And the image was **relocated** at load (base
`0x0FBA0000` against a preferred `0x10900000`), so the scan resolved these in a
moved image rather than at the link-time base.

`slot1` held the pointer; `slot2` was null in every row. So the 12-byte row is
`{ const TCHAR* Name; Native Func; INT unused-or-not-yet-known }`.

**The function slots really are written at runtime** — they are zero on disk, and
that was the whole risk of the session. No backoff retry was needed; the first
attempt at ~1 s already had them.

## What it is for

The mod still cannot answer *"am I in a cutscene right now"*. Eight inferred
detectors are falsified (`docs/INVARIANTS.md` § *Cutscene detection*), and M1-S2
killed the ninth — `myHUD.bHideHUD` is written by the script corpus and never by
the shipped game. `docs/ARCHITECTURE.md` finding 2 is the next source: `Core.Object`
declares `GetPropertyTextByName(name) -> string` as a native and retail script
calls it **on live instances**. Console `get` returns the class default because
the console resolves a *class* — that measurement stands, the conclusion drawn
from it did not.

## The native lookup table — how the address is found

Grep the banner `HOW THE SCAN WORKS`.

BioshockHD.exe contains, in `.rdata`, the wide string

```
L"intUObjectexecGetPropertyTextByName"
```

exactly once. That is UE2's `IMPLEMENT_FUNCTION` registration symbol —
`int` + class + func — and **exactly one DWORD in the image points at it**. That
pointer is the first field of a 12-byte row in a table of natives whose
neighbouring rows name `execGetPropertyText`, `execSetPropertyText`,
`execSetPropertyTextByName`, `execGotoState`, `execEnable`, `execDisable` and
`execSaveConfig`. The four property accessors are four adjacent rows of one
table, and the name string points straight at the row that owns the function
pointer.

Three stages, not `FindCalcView`'s six:

| Stage | What it does | Fails if |
|---|---|---|
| 1 | Find the symbol string in `.rdata` | zero hits |
| 2 | Find the single DWORD pointing at it, in `.data` | zero or many rows |
| 3 | **Measure** the stride from neighbouring rows, then require 5 of 8 neighbours to be rows too | the row stands alone, or neighbours disagree |
| 4 | Score both trailing DWORDs; log both, ranked | both null, or both callable (ambiguous → fail closed) |

`FindCalcView` needs six stages because an FName literal only reaches a
cached-index global and it must bridge from there to a function body. Here the
anchor lands on the function's own row. **This is a deliberate deviation from
what `.planning/sessions/M3.md` § M3-S1 specified**, kept in the card's spirit:
locate by pattern never by RVA, staged, fail closed, log every near miss.

`FindCalcView` is untouched — it carries a "Do not touch it" banner and works on
all three storefronts. The region and scan helpers here are deliberate copies of
its shapes, not a refactor of them.

### The terminator is part of the needle

`intUObjectexecGetPropertyText` is a **prefix** of
`intUObjectexecGetPropertyTextByName`. Matching without the null terminator makes
the shorter name hit inside the longer one and hands S2 the wrong function. This
actually happened during the static analysis that preceded the file; it is not a
hypothetical.

## The backoff, and why it is still there

**The row's trailing DWORDs are zero on disk**, so the runtime read was the whole
question. It came back yes on the first attempt. The backoff (~1s, 4s, 10s, 20s,
40s) and the raw-row dump are kept anyway: they cost nothing when the first
attempt succeeds, and they are what would make a failure on another storefront
cost one cycle instead of two.

Only the cheap slot re-read repeats. The two scanning passes run once.

## Where it runs

`EngineBridge_Tick()` is called from `GameState_Observe` on the **game thread**,
beside `MyHudTick`/`CineTick`, and on the same reasoning: read-only, one-shot,
gating nothing, and a different feature from the (dead) context scan — so it does
not sit behind `EnableGameState`.

It takes no argument. The native table is process-static, so unlike the pawn and
controller pointers it has **no lifetime boundary to reset at**.

## Config

`EnableNativeScan`, **default 1**. That is a deliberate exception to "new
behaviour ships default-off": the scan writes nothing, hooks nothing and gates
nothing, its entire product *is* the log line, so shipping it off would waste the
headset cycle it exists to spend. The switch is there to kill it without a
rebuild.

**There is deliberately no INI override of the address.** That would be
hardcoding an RVA through the back door, which `docs/ENGINE-MAP.md` §
*Storefront divergence* exists to warn against.

## What S2 has to know before it calls

The symbol is `exec`GetPropertyTextByName. In UE2 that is

```cpp
void UObject::execGetPropertyTextByName(FFrame& Stack, RESULT_DECL);
```

a `__thiscall` member taking a **bytecode frame**, not a string. There is no
`obj->GetPropertyTextByName('Health')` entry point to call — the parameter has to
be supplied through a constructed `FFrame` whose `Code` yields the `name`. M3-S2
is written as though a direct call exists; it does not, and that is the first
thing that session has to deal with.
