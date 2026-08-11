# Engine memory map

Measured against the tested builds. **Validate object identity before every
write** — an offset that is right for Steam may be wrong or absent elsewhere.
Load this only when touching engine memory; see the `engine-offset` skill for
the procedure for adding a new one.

Units: one Unreal world unit = **1 cm**. `65536` rotator units = 360°, so
**1° ≈ 182.0444** units. World axes: forward `+X`, right `+Y`, up `+Z`. BioShock's
roll sign is opposite most external examples and is handled in the basis conversion.

---

## Engine-wide

| Fact | Value | Notes |
|---|---:|---|
| `AActor` base size | `0x450` | first subclass field lands here |
| `FName` | 8 bytes | `{Index, Number}` |
| `TArray` | 12 bytes | `{Data, Count, Max}` |
| `AActor::Location` | `+0x1D8` | canonical. `+0x1A0` was a bad early scan |
| `AActor::Rotation` | `+0x1E4` | three int32 rotator values |
| `AActor::DrawScale` | `+0x2AC` | uniform |
| `DrawScale3D X/Y/Z` | `+0x2B0/+0x2B4/+0x2B8` | per-axis |
| `Actor::Level` | `+0xF8` | LevelInfo is self-referential |
| `AController::Pawn` | `+0x450` | `+0x914` is an alias; **`+0x620` is `ViewTarget`** |
| Controller `Rotation` / aim field | `+0x1E4` | **see the warning below** |
| World FOV (`Controller.FovAngle`) | `controller+0x45C` | mirror at `+0x648` is `DesiredFOV` |
| Foreground FOV (`ForegroundFovAngle`) | `controller+0x460` | |
| **Input axis `aForward`** | `controller+0x5C0` | was "acceleration request X" — see below |
| **Input axis `aStrafe`** | `controller+0x5C8` | was "acceleration request Y" |
| Input axes `xForward`/`xStrafe` | `controller+0x5D4/+0x5D8` | was "acceleration mirror" |
| `PlayerController.ViewTarget` | `controller+0x620` | normally the pawn — hence grave 1 |
| **`PlayerController.myHUD`** | `controller+0x71C` | **measured M1-S1**, see below |
| Velocity | `controller+0x8F8`–`+0x900` | small residual jitter when blocked |
| `Level.Pauser` | `level+0x668` | null in play, object while paused |
| `Level.TimeDilation` | `level+0x5F8` | |
| `Level.TimeSeconds` | `level+0x5FC` | |
| `ShockPlayer.Hands` | `pawn+0x724` | string block starts ~`+0x728` |
| `AllPossibleWeaponClasses` | `pawn+0x750` | the real 8-entry list |
| `PreloadClasses` (decoy) | `pawn+0x998` | different ordering — **not** slot authority |
| `LastPlayerInputContext` | `pawn+0x934` (window `+0x728..+0xA7C`) | scan has never locked |
| **`ShockPlayerController.bIsForcingPlayerMove`** | `controller+0x9E0` | **measured M7-S5** — see below |
| Quest arrow actor | `pawn+0xAE4` | proximity probe result |

> ### ⚠ `Controller.Rotation` (`+0x1E4`) has three consumers
> The rendered view, the weapon trace, **and the direction the character walks**.
> Writing an aim direction into it therefore also steers locomotion — this is why
> motion aim walks you toward wherever the controller points, and why scripted
> walk sequences go off course. There is no arrangement of this one field that
> separates the three. See `docs/INVARIANTS.md` § *Aim and movement*.

### `+0x5C0` / `+0x5C8` — measured 2026-08, **identified M1-S1**

Three trials. Reads `0.000` standing still and `778`–`875` the instant the left
stick is held. The third trial was taken **pinned in a corner, provably not
moving**, and still read `~875`.

**These are `PlayerController.aForward` and `aStrafe` — the raw input axes**,
named by declaration-order arithmetic in M1-S1. That is not a correction to the
measurement, it is the explanation of it: the behaviour recorded above is
exactly what a raw input axis does, which is why a wall cannot cancel it. The
`+0x5D4`/`+0x5D8` "mirror" is the neighbouring `xForward`/`xStrafe` pair.

So it is the *input-requested* signal, not velocity. Walking into geometry
cannot false-fire it. When the game discards input (a pushed `NullInput`
context), the stick is deflected and this stays zero.

Its limit: it only answers when the player is asking to move. See `docs/modules/gamestate.md`.

### `myHUD` and the `FMatrix` alignment — measured M1-S1, 2026-08-09

**`PlayerController.myHUD` = `controller+0x71C`**, confirmed by a self-validating
live read on two different controllers (a menu one and an in-game one), stable
across ten one-second samples each.

Declaration-order arithmetic from `AActor` base `0x450` predicted `+0x710`. The
`+0xC` difference is **`FMatrix` being 16-byte aligned on this build** —
`PlayerController.RenderWorldToCamera` starts at `+0x680`, not `+0x674`. Every
field declared after `FixedRotation` therefore shifts `+0xC` from a naive walk:

```
+0x680  RenderWorldToCamera (FMatrix, 64)     +0x6F4  TargetViewRotation
+0x6C0  FlashScale                            +0x700  BlendedTargetViewRotation
+0x6CC  FlashFog                              +0x70C  TargetEyeHeight
+0x6D8  bManualFogUpdate (bool dword)         +0x710  TargetWeaponViewOffset
+0x6DC  LastDistanceFogColor                  +0x71C  myHUD
```

**Apply this to any future walk through `PlayerController`.** Everything up to
`DesiredFOV` (`+0x648`) is confirmed unshifted by seven independent anchors,
including both bool packs — 17 bools sharing one DWORD at `controller+0x468`,
38 sharing two at `+0x594`.

### `controller+0x9E0` — `bIsForcingPlayerMove`, measured M7-S5, 2026-08-10

Found by differential probe, not arithmetic — `ShockPlayerController`'s fields
begin at the end of `PlayerController` and **that size is unknown**, so it could
not be computed. Correlated across three events whose durations differ by 24x,
each matching the tester's independent report:

| Event | `+0x9E0` high for | Reported as |
|---|---|---|
| Scripted scene #1 | **1.0 s** | "went straight in" |
| Scripted scene #2 | **0.24 s** | instant |
| Bathysphere entry | **5.75 s** | "the slewing, not quite as long this time" |

The `SLEW` diagnostic fires inside those windows with the stick centred, and the
flag drops **0.09 s after** the scripted animation begins. That is the entry
stall: `StartForcePlayerMove` interpolates the player into position and heading
*before* the animation starts, and anything writing `Controller.Rotation` through
that window fights it.

**Verify by shape before trusting it:** it is a lone bool, so it reads exactly
`0` or `1`. Anything else means the pointer or the offset is wrong.

**Free consequence for M3-S3.** Walking `ShockPlayerController.uc` backwards from
line 47 to line 42 puts **`LastPlayerInputContext` at `+0x9C4`** (if interface
refs are 4 bytes) or **`+0x9C0`** (if 8). Two computed candidates for the field
whose *pawn* copy has never locked, instead of another blind scan.

## Pawn bool block — computed M7-S4, oracle-checked

`Pawn`'s own fields start at the `AActor` base `0x450`. `Pawn.uc` lines 13–44 are
**exactly 32 consecutive bools**, which fills one DWORD precisely and starts a
fresh one for the next three:

| Offset | Contents |
|---:|---|
| `+0x450` | `Controller` |
| `+0x454` | `NetRelevancyTime` |
| `+0x458` | `LastRealViewer` |
| `+0x45C` | `LastViewer` |
| `+0x460` | lines 13–44 — thirty-two bools, one full DWORD |
| **`+0x464`** | bit 0 `ShouldNotTakeDamageOnNextLanding` · **bit 1 `bCannotFall`** · bit 2 `bUseHavokRigidBodyCapsuleCollisions` |
| `+0x468` | `HavokRigidBodyCapsuleCollisionExtraRadius` (float — ends the bool run) |

**`bCannotFall` is the bathysphere signal — MEASURED LIVE, M7-S4, 2026-08-10.**
`ActionEnableBathysphereModeForPlayer` sets it `true` and clears
`bUseHavokRigidBodyCapsuleCollisions` **in the same call**, and `ShockPlayer`
defaults that one to `true` — so entering a ride must flip **bit 1 up and bit 2
down together**. The oracle passed exactly:

```
on foot       pawn+0x464 = 00000004   bCannotFall=0  capsule=1
bathysphere   pawn+0x464 = 00000002   bCannotFall=1  capsule=0
```

Two bits moving in opposite directions in one write is not something a wrong
offset produces by chance. The prediction came purely from declaration-order
arithmetic and was right first time — the payoff for anchoring on the `AActor`
base and counting the 32-bool run exactly.

## HUD actor — predicted and confirmed M1-S1

The whole head of the object was confirmed field-by-field against a live dump;
every slot matched its declared type and carried a plausible value.

| Offset | Field | Observed |
|---:|---|---|
| `+0x450` | `SmallFont` | font object |
| `+0x454`/`+0x458`/`+0x45C` | `MedFont`/`BigFont`/`LargeFont` | one shared font object |
| `+0x460` | `HUDConfigWindowType` | empty `FString` `{0,0,0}` |
| `+0x46C` | `nextHUD` | null |
| **`+0x470`** | **`PlayerOwner`** | **the PlayerController — the back-reference** |
| `+0x474` | `ProgressFontName` | live `FString`, `Count=Max=0x1B` |
| `+0x484` | `ProgressFadeTime` | `1.0` |
| `+0x488` | `MOTDColor` | `FFFFFFFF` |
| `+0x48C` | `ScoreBoard` | null (single-player) |
| **`+0x490`** | **bool DWORD** | `0x00000020` — see below |
| `+0x494` | `HudCanvasScale` | `0.95` |

The `+0x490` DWORD packs six bools in declaration order:

```
bit 0  bHideHUD            bit 3  bHideCenterMessages
bit 1  bShowScores         bit 4  bBadConnectionAlert
bit 2  bShowDebugInfo      bit 5  bMessageBeep
```

Observed `0x20` in ordinary play — only `bMessageBeep` set, and **`bHideHUD`
correctly clear**. `bHideHUD` is written from exactly two places in the whole
script corpus (`ActionCinematicEnter`/`Exit`), which is why it is the cinematic
flag; see `docs/ARCHITECTURE.md` finding 1.

---

## Hands actor

| Offset | Field |
|---:|---|
| `+0x450` | `PawnOwner` |
| `+0x454` | `CurrentAbility` |
| `+0x458` | `OldAbility` |
| `+0x45C` | `CurrentHoldable` — the weapon actor in the corrected layout |
| `+0x460` | `UseAbilityAnimation` FName |
| `+0x468` | `BioAmmoClass` |
| `+0x46C` | `PlayerViewOffset` (observed zero) |
| `+0x484` | `WeaponBobDamping` (0.5, behaviourally inert) |
| `+0x494` | bitfield including weapon-mode state (`Hands.uc` lines 35–38) |
| `+0x498` | `HandsOffscreenAnimationName` (`HandsDown`) — **the anchor** |
| `+0x4B8` | `InjectingEveAnimationName` (FName, measured idx `22336`) |
| `+0x4D8` | `ExorcisingGathererAnimationName` (FName, measured idx `22330`) |
| `+0x558` | `CurrentScriptedAnimationName` — **reads `None` even mid-sequence** |
| `+0x580` | `HandsAnimationHandle` — increments once per animation played |
| **`+0x594`** | **three-bool DWORD — see below** |

### `+0x594` — the scripted-animation flag, measured M7-S1, 2026-08-10

```
bit 0  bFinishedStateAnimations                      toggles with every anim cycle
bit 1  AbilityHasBeenReleased                        changes on ability use
bit 2  CurrentlyExecutingScriptedHandAnimationSequence
```

**Bit 2 is an exact scripted-sequence bracket.** Measured live: set `0.8 s` after
the tester marked the start of a scripted scene and cleared `0.75 s` before they
marked the end — both inside human reaction time on the marker key — and it
**fired exactly once in six minutes** covering weapon fire, plasmid fire, four
gene-machine opens, a Little Sister rescue and walking. Zero false positives.

**How the offset was derived, and why it is trusted.** Predicted by
declaration-order arithmetic (`Hands.uc` lines 39→82) from the `+0x494`/`+0x498`
anchor, which working shipping code already relies on (`IdleAnimMode=2`). UE2
packs consecutive bools, and lines 80–82 are three in a row, so bit 2 is the
target. **Four independent live confirmations**, which is what makes this a
measurement rather than a prediction:

1. `+0x498`, `+0x4B8`, `+0x4D8` all read as plausible FNames (non-zero index,
   `Number == 0`), and the two animation names sit 6 apart in the name table —
   consistent with being declared adjacently in one class.
2. `+0x580` increments like the animation handle it is predicted to be.
3. Bits 0 and 1 behave exactly as their names say — bit 0 toggles with animation
   cycles, bit 1 with ability use.
4. Bit 2 brackets a marked scripted scene and nothing else.

> **Bit 0 is NOT "an animation is playing" — falsified M7-S3.** It is
> `bFinishedStateAnimations`, and gating the arms on it failed in **opposite
> directions in two scenes**: the Little Sister crawl hid the arms throughout
> (including the bottle catch), while the plasmid balcony showed them and then
> left them frozen. `Hands.uc` `state PlayingScriptedHandAnimation` has an
> **empty body** and never touches the flag, so it keeps whatever the last
> weapon state left; `state InjectingEve` does set it. It tracks the Hands state
> machine's own animations only. `ScriptedHandsAnimationHandle` is no better —
> only ever assigned, never cleared. Use rig **motion** instead
> (`ArmHide_HandMotion`).

**What it does NOT cover.** The Little Sister *rescue* and the EVE *injection*
left bit 2 clear. Both are Hands **states** (`ExorcisingGatherer`, `InjectingEve`)
rather than scripted sequences — which is exactly why `ShockPlayer.uc:2091` tests
two separate conditions, `GetStateName() == 'ExorcisingGatherer' ||
GetStateName() == 'PlayingScriptedHandAnimation'`. Covering those needs the Hands
state, not this bit.

Ability mode tests `CurrentAbility != null && CurrentHoldable == null`, which
avoids flapping mid-equip.

## Holdable / weapon animation

| Offset | Field |
|---:|---|
| `+0x450` | back-pointer to Hands — use as a self-check |
| `+0x458` | `IdlingHandsAnim` `TArray<FName>` |
| `+0x470` | `IdlingAnim` |
| `+0x478` | `AdditiveHandBobAnim` (already `None`) |
| `+0x4A0` | hide bitfield |
| `+0x4A4` | attachment bone |
| `+0x6C4/+0x6C8` | zoomed world / foreground FOV |

## Weapon slots (`AllPossibleWeaponClasses`)

```
0 Wrench   1 Pistol   2 Shotgun    3 Crossbow      4 GrenadeLauncher
5 MachineGun   6 ChemicalThrower   7 ResearchCamera   8 Plasmid (mod-added, shared)
```

MachineGun placement is the disambiguator against the decoy array. **All plasmids
share slot 8**, so Electrobolt and Telekinesis overwrite each other's calibration.

## Skeleton

47 bones exactly, or `ArmHide` refuses to write. Ten sleeve bones collapse; 34
hand/finger bones and the weapon attachment are untouched by that pass. **Bone 43
is never scaled** — telekinesis release walks the attachment path through it,
and that path inverse-decomposes bone scale. It may be *moved*: `HideBone` pushes
it to `kFarBelow` and the cluster transform carries its position along.

| Offset | Field |
|---|---|
| `AHands +0x3FC` | `SkeletonInstance*` |
| `SkeletonInstance +0x04` | owning actor — proves the link before any write |
| `+0x48` | `hkQsTransform*` render bone array |
| `+0x4C` | bone count, expected 47 |
| `+0x88` | evaluate-if-dirty byte |

`hkQsTransform` is 48 bytes: `position[4]`, `rotation[4]` (quaternion), `scale[4]`.

| Cluster | Bones | Wrist | Sleeve |
|---|---|---|---|
| Left | 6–21 | 6 | 3, 4, 5, 22, 23 |
| Right | 27–44 | 27 | 24, 25, 26, 45, 46 |

### The array's space — MEASURED, M6-S1, 2026-08-10

A **common model space**, not parent-relative: bones sit tens of units apart in
one shared frame. Units are **centimetres**, and a rendered distance is model
units × `HandsScale`. The lane order reads as **(forward, right, up)** — the
actor's own axes — from a rest pose where the right wrist sits `+47` forward,
`+27` right, `-20` below the eye while the left hangs at `-3 / -31 / -82`. That
last part is anatomy rather than a measurement, which is why it lives in
`LeftHandAxisMap` and not in the source.

**The array keeps evaluating in ordinary play even while the dirty byte is
cleared every CalcView** — bone 27 moved between every pair of dumps with the
sleeve pass running. Writes stick because they land after evaluation. Do not
generalise that to scripted sequences; the M7-S4 latch was real.

The engine's own pose is what a reference capture must read, so every capture is
gated on `ScaleLooksNormal` — a cluster mid-hide reads back **our** zeroes.

### Confirmed by a second source

An independently developed mod against the same game records the *same* map,
value for value: wrists 6 and 27, clusters 6–21 and 27–44, both five-bone sleeve
sets, weapon attach 43, 47 bones, 48-byte transform, skeleton at `+0x3FC`.
Neither project copied the other.

### Bone 43 → 44 is the barrel axis — MEASURED 2026-08-11

Bone 43 is the weapon attach point and 44 is the tip of that chain, so **the gun
barrel and the wrench head are the same read**. That collapsed two planned
cycles into one.

Measured on the machine gun: the separation holds a rigid length of **24.40**
across every dump while the components rotate (`17.5, 12.9, 11.1` → `12.6, 7.7,
-19.4`), which is what one bone in a live array looks like.

> **This DISAGREES with the second source**, which recorded *"bone 44 muzzle-ish
> tip at x = +71"*. Ours is nowhere near 71 in any lane. Use the measurement,
> not the prediction — and treat the length, not the components, as the invariant.

### The bone-name map — offset confirmed, element type still open

`SkeletonInstance + 0x08` → `SharedSkeletonData`, `+0xAC` → an array, read as a
TArray `{data, count, max}`. Measured: `count 47, max 78`, and **47 is exactly
the bone count located by the independent `+0x4C` route** — that agreement is
what makes the offset trustworthy.

**The element type is not 4-byte `FName`s.** Read that way the entries come back
`-1, 26659, 0, 0 / -1, 35607, 0, 1 / -1, 17127, 0, 2` — a period-4 pattern whose
last lane counts up, i.e. ≥16-byte records with an index field, read four bytes
at a time. The probe now dumps raw bytes instead of assuming.

Resolving indices to *text* additionally needs the global name table, which is a
separate locate. The indices alone are enough for an identity check, since two
different skeletons cannot share all 47.

---

## Storefront divergence

| Fact | Steam | Epic | GOG |
|---|---:|---:|---:|
| Module size | `0x01677000` | `0x01676000` | `0x01716000` |
| `eventPlayerCalcView` RVA | `0x1BE7A0` | `0x1BE7A0` | `0x120A0` |
| Delta function | `0x53D850` | `0x53F7B0` | signature finds 0 matches |
| AHands vtable | `0xD8A28C` | `0xD8959C` | unknown |
| SkeletonInstance vtable | `0xE19ACC` | `0xE190EC` | unknown |
| `execGetPropertyTextByName` symbol | `0xE33F78` | `0xE33370` † | unchecked |
| its native-table row | `0x11BE684` | `0x11BD6B4` † | unchecked |
| `execGetPropertyTextByName` | `0x7346E0` | unchecked | unchecked |
| `execGetPropertyText` | `0x734640` | unchecked | unchecked |
| `execSetPropertyText` | `0x734840` | unchecked | unchecked |
| `execSetPropertyTextByName` | `0x734940` | unchecked | unchecked |

**Steam values measured live, M3-S1, 2026-08-10**, identical across two launches
and agreeing exactly with what static analysis predicted from the file. The
image is relocated at load — it ran at base `0x0FBA0000` against a preferred
`0x10900000` — so these RVAs were resolved in a moved image, not at the
link-time base.

† Epic is **file-derived only**: read out of the shipped executable, never seen
in a running process. GOG is not installed on the development machine. **Do not
paste any of these into code** — `docs/modules/enginebridge.md` explains the
scan that derives them, and the whole point of it is that the row moves between
storefronts.

Steam and Epic look similar but the data-section shift is **not constant**
(`0xCF0` for one vtable, `0x9E0` for the other) — no single value serves both.
GOG is a substantially different layout. The FName/string camera scan found the
function on all three; every hardcoded address did not.

Open on alternate builds: the GOG delta signature (needs shortening or
rederiving) and Steam-specific `EnginePtrRva`/`EngineVtableRva`, which is why the
engine-Exec reticle disable fails on Epic and GOG.

## The native lookup table

How UE2 registers its native functions on this build, and the anchor M3-S1 uses.
Full treatment in `docs/modules/enginebridge.md`.

`.rdata` holds a run of wide strings of the form `int<Class>exec<Func>` —
UE2's `IMPLEMENT_FUNCTION` registration symbol. `.data` holds a table of
**12-byte rows**; the first DWORD of a row points at one of those strings, and
the two trailing DWORDs are **zero in the file on disk**, so whatever the row
carries beyond the name is written at runtime.

```
.rdata   L"intUObjectexecGetPropertyTextByName"   <- exactly one copy
.data    [ nameptr | dw1 | dw2 ]                  <- exactly one row points at it
```

Rows observed adjacent to the property accessors, in order: `execGotoState`,
`execEnable`, `execDisable`, `execGetPropertyText`,
**`execGetPropertyTextByName`**, `execSetPropertyText`,
`execSetPropertyTextByName`, `execSaveConfig`, `execStaticSaveConfig`,
`execAssertScriptAndNativeBuildConfigsMatchNative`.

This is a **general** mechanism, not a one-off: any native the engine registers
can be reached the same way, by its `int<Class>exec<Func>` name. Locate by that
string, never by the row's RVA — the row moves between storefronts (table above).

### The class prefix is part of the needle — `A` for actors, `U` for objects

`UObject` subclasses register as `intU<Class>exec<Func>`; **`AActor` subclasses
register as `intA<Class>exec<Func>`**. `LevelInfo` is an actor, so its natives
are `intALevelInfoexec…`. Two rows were first written with `U`, and both came
back `NOT FOUND — no symbol string` — a *stage 1* miss, so the four property
accessors located normally in the same run and nothing looked broken.

### Validate a needle offline before spending a headset cycle on it

The whole table is enumerable from the shipped executable: **1,823**
`int…exec…` UTF-16 strings in `.rdata`. Scanning for one takes seconds and
answers exactly the question a failed run would. Measured in
`Build/Final/BioshockHD.exe` (Steam, 21,214,720 bytes):

| File offset | Symbol |
|---|---|
| `0x00E06638` | `intALevelInfoexecGetFlashGUIController` |
| `0x00E064E0` | `intALevelInfoexecFlashGUIControllerExists` |

### The Flash interface API

`UFlashGUIController` registers **51** natives. Located and confirmed live:

| Native | RVA (Steam) |
|---|---|
| `GetFlashGUIController` (on `ALevelInfo`) | `0x3A4B30` |
| `FlashGUIControllerExists` (on `ALevelInfo`) | `0x3A4B80` |
| `GetTopPlayingMovie` | `0x43E270` |
| `HideMovie` | `0x43DAC0` |
| `IsPausedInterfaceActive` | `0x43B200` |

**Neither getter needs calling.** Both are four-instruction field walks, decoded
from their own code bytes — see `docs/modules/gamestate.md` and the
`M6-S5: WHICH INTERFACE SCREEN IS UP` banner in `Game/GameState.cpp`:

```
GetFlashGUIController, this = LevelInfo:
  [this+0xFC] -> +0x5C -> +0x4C -> (bail if [+0x48] == 0) -> +0x44 -> deref -> +0x7C

GetTopPlayingMovie, this = FlashGUIController:
  count = [this+0x15C]   data = [this+0x158]   top = data[count-1]
```

`Actor::Level` is `+0xF8`, self-referential — see *Pause / full-menu detection*.

### `AWeapon::GetPerfectFireStart` — vtable slot `+0x304`

Returns the shot origin and rotation, which **answers M4-S3 by asking instead of
inferring**. Read off the held weapon's vtable, so no hardcoded RVA and no
storefront exposure.

Measured 2026-08-11: slot `+0x304` → `0x0FB96840`, module base `0x0F970000`, so
**rva `0x226840`** — *exactly* the figure an independently developed mod recorded
for the same function. Two projects, no shared code, same address. Valid MSVC
prologue `55 8B EC 83 EC 18 53 56`.

**Located, never called.** A `__thiscall` returning a struct by hidden pointer is
the shape that unbalanced the stack in that other project.

## CalcView call sites

Multiple sites call the function. The **render-view site is the one that keeps
running while stationary** — identify it at runtime, never hardcode. Writing head
rotation into a movement or aim consumer silently couples head motion to locomotion.

| Site | Return address | Behaviour |
|---|---|---|
| site0 | `module+0x4CCF62` | every frame, moving or not — **the leader** |
| site3 | `module+0x491C86` | every frame while moving; freezes stationary |
| site2 | `module+0x2BA912` | ~8/s while moving |
| site4 | `module+0x2A8848` | ~1/s while moving |
| site1 | `module+0x4CB8DD` | spawn-time one-shot |

## Calling convention

```cpp
struct FVector  { float x, y, z; };
struct FRotator { int32_t pitch, yaw, roll; };

typedef void (__fastcall* CalcViewFn)(
    void* pThis, void* edx_unused,
    void** ViewActor, FVector* CameraLocation, FRotator* CameraRotation);
```

The real function is `__thiscall`; the detour uses `__fastcall` plus a dummy EDX
argument to reproduce the x86 register and stack layout.
