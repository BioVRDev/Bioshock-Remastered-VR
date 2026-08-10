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
| `AController::Pawn` | `+0x450` | aliases also seen at `+0x620`, `+0x914` |
| Controller `Rotation` / aim field | `+0x1E4` | **see the warning below** |
| World FOV | `controller+0x45C` | mirror at `+0x648` |
| Foreground FOV | `controller+0x460` | |
| **Acceleration request** | `controller+0x5C0` (X), `+0x5C8` (Y) | see below |
| Acceleration mirror | `controller+0x5D4/+0x5D8` | second copy the engine keeps |
| Velocity | `controller+0x8F8`–`+0x900` | small residual jitter when blocked |
| `Level.Pauser` | `level+0x668` | null in play, object while paused |
| `Level.TimeDilation` | `level+0x5F8` | |
| `Level.TimeSeconds` | `level+0x5FC` | |
| `ShockPlayer.Hands` | `pawn+0x724` | string block starts ~`+0x728` |
| `AllPossibleWeaponClasses` | `pawn+0x750` | the real 8-entry list |
| `PreloadClasses` (decoy) | `pawn+0x998` | different ordering — **not** slot authority |
| `LastPlayerInputContext` | `pawn+0x934` (window `+0x728..+0xA7C`) | scan has never locked |
| Quest arrow actor | `pawn+0xAE4` | proximity probe result |

> ### ⚠ `Controller.Rotation` (`+0x1E4`) has three consumers
> The rendered view, the weapon trace, **and the direction the character walks**.
> Writing an aim direction into it therefore also steers locomotion — this is why
> motion aim walks you toward wherever the controller points, and why scripted
> walk sequences go off course. There is no arrangement of this one field that
> separates the three. See `docs/INVARIANTS.md` § *Aim and movement*.

### Acceleration `+0x5C0` / `+0x5C8` — measured 2026-08

Three trials. Reads `0.000` standing still and `778`–`875` the instant the left
stick is held. The third trial was taken **pinned in a corner, provably not
moving**, and still read `~875`.

That makes it the *input-accepted* signal, not velocity: the engine writes the
acceleration request from the stick, and the wall merely cancels the motion.
Walking into geometry therefore cannot false-fire it. When the game discards
input (a pushed `NullInput` context), the stick is deflected and this stays zero.

Its limit: it only answers when the player is asking to move. See `docs/modules/gamestate.md`.

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
| `+0x494` | bitfield including weapon-mode state |
| `+0x498` | `HandsOffscreenAnimationName` (`HandsDown`) |

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
hand/finger bones and the weapon attachment are untouched. **Bone 43 is never
modified** — telekinesis release walks the attachment path through it.

---

## Storefront divergence

| Fact | Steam | Epic | GOG |
|---|---:|---:|---:|
| Module size | `0x01677000` | `0x01676000` | `0x01716000` |
| `eventPlayerCalcView` RVA | `0x1BE7A0` | `0x1BE7A0` | `0x120A0` |
| Delta function | `0x53D850` | `0x53F7B0` | signature finds 0 matches |
| AHands vtable | `0xD8A28C` | `0xD8959C` | unknown |
| SkeletonInstance vtable | `0xE19ACC` | `0xE190EC` | unknown |

Steam and Epic look similar but the data-section shift is **not constant**
(`0xCF0` for one vtable, `0x9E0` for the other) — no single value serves both.
GOG is a substantially different layout. The FName/string camera scan found the
function on all three; every hardcoded address did not.

Open on alternate builds: the GOG delta signature (needs shortening or
rederiving) and Steam-specific `EnginePtrRva`/`EngineVtableRva`, which is why the
engine-Exec reticle disable fails on Epic and GOG.

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
