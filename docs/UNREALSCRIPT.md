# The decompiled UnrealScript corpus

**1,765 classes**, decompiled from the game's own script packages. This is the
primary research resource for anything involving engine state, and it has
repeatedly answered in minutes what memory scanning could not answer in sessions.

## Where it is

```
research/uscript/<Package>/Classes/*.uc
```

**`research/` is gitignored and must stay that way.** This is 2K's copyrighted
content — a local research aid, never committed, never published, never quoted
at length in a public issue or release note.

| Package | Classes | What is in it |
|---|---:|---|
| `ShockGame` | 654 | **the important one** — `ShockPlayer`, `Hands`, weapons, plasmids, HUD, actions |
| `ShockAI` | 540 | AI, Big Daddies, Little Sisters / gatherers |
| `Engine` | 320 | `Actor`, `Pawn`, `PlayerController`, `LevelInfo` |
| `Scripting` | 99 | the scripted-sequence system |
| `VengeanceShared` | 80 | shared base types |
| `Tyrion` | 27 | the gene/tonic system |
| `Core` | 10 | `Object`, `Commandlet` |
| `IG*`, `FMODAudio` | 35 | effects and audio subsystems |

## Regenerating it

UE Explorer (portable) ships a CLI:

```bash
"<ue-explorer>/UEExplorer.exe" "<path>/<Package>.U" -console -export=classes -silent
```

Two things that will waste your time if you do not know them:

1. **Output does not go next to the package.** It goes to
   `%APPDATA%\EliotVU\UE Explorer\<version>\Exported\<Package>\Classes\`.
   The command exits 0 and appears to have done nothing.
2. **Running it with no file path launches the GUI and blocks.** There is no
   usable `-help`; always pass a package path.

Packages live in `…\Build\Final\BakedScripts\pc\`.

## What is reliable and what is not

The decompiler is not a perfect fit for this engine — BioShock is a heavily
modified UE2.5 fork, and UE Explorer ships no native-function table for it. The
ShockGame export logged ~2,300 deserialization exceptions and ~900 range errors.

| Reliable | Unreliable |
|---|---|
| **Variable declarations**, types and order | Bytecode inside some function bodies |
| Function signatures | Native function calls (no NTL for this game) |
| `defaultproperties` | Anything the log flagged as `Trailing data` |
| Class hierarchy, states, enums | Complex expression reconstruction |

Treat function bodies as a strong hint and declarations as near-certain. Where a
body matters, cross-check against behaviour or a memory read.

## The technique this unlocks

> **UE2 lays out properties in declaration order.** A `.uc` file therefore gives
> you the *sequence* of an object's fields, which is exactly the information a
> blind memory scan lacks.

This turns "find the offset of field X" from a differential-probe hunt into
arithmetic: anchor on a field whose offset is already measured, then walk the
declarations forward, adding each type's size.

Worked anchors from `docs/ENGINE-MAP.md`: `AActor::Location = +0x1D8`,
`Rotation = +0x1E4`, `DrawScale = +0x2AC`, `ShockPlayer.Hands = pawn+0x724`,
`AllPossibleWeaponClasses = pawn+0x750`.

Sizes: `float`/`int`/pointer/object-ref = 4 · `FName` = 8 · `FVector` = 12 ·
`FRotator` = 12 · `FString`/`TArray` = 12 · `bool` = **packed into a bitfield**,
which is the one that will trip you up — consecutive `bool`s share a DWORD.

**Always verify the computed offset against a live read before writing to it.**
Declaration order is a strong prediction, not a measurement — the same standard
that produced `AActor::Location`, which was predicted from script and then
confirmed in one comparison.

### Open lead this applies to

`ShockPlayer.CurrentExorcismTarget` (`ShockPlayer.uc:348`) brackets the entire
Little Sister rescue — set in `BeginExorcisingGatherer`, cleared in
`OnGathererInteractionCompleted`. `.planning/STATE.md` records it as a live
cutscene-detection lead whose offset was never pinned down by probing. Counting
declarations from a known anchor is now the cheaper route.

## Useful entry points

> **The `Scripting` package is the scripted-sequence system** — `Action*`,
> `Trigger*`, `Watcher*`, `Script`, `Variable*`, `Message*`. It is what the level
> designers actually built cutscenes and scripted moments *with*, so it is the
> authority on how any scripted moment is constructed. Start there, not in
> `ShockGame`, for anything cutscene-shaped. It is where `docs/ARCHITECTURE.md`
> findings 1 and 2 came from.

### The native property accessors

`Core/Classes/Object.uc` declares `GetPropertyText`, **`GetPropertyTextByName`**,
`SetPropertyText` and `SetPropertyTextByName` as natives, and retail script calls
them **on live instances** (`Scripting/Classes/Script.uc`,
`Scripting/Classes/ActionGetProperty.uc`). This is why the "no reflection system"
premise is wrong, and it is Tier 1 of `docs/ARCHITECTURE.md`. The console `get`
returning class defaults is a property of the *console command*, not of the engine.

| Question | Start at |
|---|---|
| Scripted sequences, forced camera | **`Scripting/Classes/`**, `ShockGame/Classes/Action*.uc` |
| Cutscene enter/exit | `Scripting/Classes/ActionCinematicEnter.uc` / `ActionCinematicExit.uc` |
| Reading a live property at all | `Core/Classes/Object.uc` (the natives above) |
| Input contexts (`NullInput` etc.) | `ShockPlayer.uc`, `ShockPlayerController.uc` |
| Weapons, plasmids, hands | `ShockGame/Classes/Hands.uc`, `Weapon*.uc`, `Ability*.uc` |
| Little Sisters / gatherers | `ShockAI/Classes/`, `ShockPlayer.uc:2415` |
| HUD / Scaleform | `ShockGame/Classes/*GUI*.uc`, `*Hud*.uc` |
