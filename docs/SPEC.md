# SPEC.md — Technical Spec (load on demand)

**Companion to `HANDOFF.md`. Ask for this when touching `CameraHook.cpp`, head
tracking, or the HUD. Everything here is MEASURED and confirmed on real hardware —
do not re-derive it.**

---

## 1. The camera function — FOUND

Target: `APlayerController::eventPlayerCalcView` — the UE2.5 event thunk the engine
calls every frame to compute the render view.

Located by an **FName chain anchored on a string literal** — survives patches, no
hardcoded offsets. **Works: 31ms, six stages, in `CameraHook.cpp::FindCalcView`.**

For the current build it resolves to `BioshockHD.exe + 0x1BE7A0`, prologue bytes
`55 8B EC 83 E4 F8 83 EC`. **That offset is auto-detected and MUST stay that way** —
it is recorded here only as a sanity anchor.

### The six stages (all logging)
1. Find wide string `"PlayerCalcView"` (UTF-16LE) in module memory.
2. Find the `PUSH imm32` (`68`) xref to it, in executable memory.
3. Forward ≤96 bytes from the xref: next `E8` (CALL), then next `89 0D imm32`
   (`MOV [imm32], ECX`). That imm32 is `NAME_PlayerCalcView.Index`.
4. Xrefs to that global, **skipping any within 200 bytes of the string xref** (the
   name table is one unrolled init loop).
5. From a surviving xref, walk **back ≤512 bytes** for the MSVC prologue
   `CC CC CC 55 8B EC`.
6. That address **+3** (the `push ebp`) is the function.

**If ANY stage fails, install NO hook.** A wrong hook corrupts the stack and kills
the game instantly. `IsMemoryValid(addr,size)` (VirtualQuery → MEM_COMMIT, sane
protection, range fits) guards every read. Module range via
`GetModuleHandleA(NULL)` + `GetModuleInformation`.

**Scan on the FIRST `Present`, not at DllMain** — the EXE may still be
packed/encrypted that early and `.text` would be garbage. By first Present (~14s
in) it's unpacked.

---

## 2. Signature — the calling-convention trick that WORKS

```cpp
struct FVector  { float   x, y, z; };            // 1 unit == 1 CENTIMETRE (MEASURED)
struct FRotator { int32_t pitch, yaw, roll; };   // low 16 bits: 65536 == 360 deg

// MSVC __thiscall on x86: `this` in ECX, stack args right-to-left, callee cleans.
// You cannot DEFINE a free function as __thiscall, so use __fastcall with a dummy
// EDX parameter: identical register + stack layout, also callee-cleans.
// CONFIRMED WORKING.
typedef void (__fastcall* CalcViewFn)(
    void* pThis,        // APlayerController*  (ECX)
    void* edx_unused,   //                     (EDX, never read)
    void** ViewActor,   // AActor**
    FVector*  CameraLocation,    // OUT
    FRotator* CameraRotation);   // OUT
```

**1 unit = 1 cm — MEASURED, not assumed.** Phase 5 crouch dropped eye height
640.4 → 587.2 = **53.2 units**; a jump was +87 units. At 2cm/unit those become a
106cm crouch and a 1.74m vertical leap — impossible. So **half-IPD = 3.2 units
literally, no calibration.** (Classic BioShock was ~2cm — do NOT carry that over.)

**Rotator decode** — reinterpret the low 16 bits as signed for −180..+180:
```
deg = (double)(int16_t)(u & 0xFFFF) * (360.0/65536.0)
```

---

## 3. The detour pattern

```
1. Call the ORIGINAL first. UnrealScript fills *CameraRotation with the game's
   intended view (mouse-driven, from Controller.Rotation).
2. Snapshot that CLEAN rotation.
3. Overwrite *CameraRotation / *CameraLocation with your values.
```

- **Absolute offset, not a delta.** The original rebuilds the buffer from scratch
  each call, so it arrives clean every frame. No accumulator, no drift; toggling
  off instantly restores the mouse view.
- **Aim stays decoupled for free** — gameplay reads `APlayerController.Rotation`,
  which we never touch. Head-look won't move where the gun shoots. This is the
  foundation for motion controls later. **But this promise depends on writing the
  RIGHT call site — see §4.**

**~~Pointer-cache the validity check on `CameraLocation`~~ — DELETED, DO NOT DO
THIS.** The original spec claimed `CameraLocation` is "the same pointer every
frame." **FALSE — measured.** The out-pointers are the *caller's stack locals*
(`0x014FF364`, `0x014FE8FC`, …) and are NOT stable. Caching produced 7,600 lines
of spurious "level transition?" spam. Validate cheaply each call; never cache on
pointer identity.

---

## 4. FIVE call sites — write to ONE

`eventPlayerCalcView` is called from **5 distinct return addresses**. When the
player is stationary they all report identical pos/rot. The discriminator is
**which sites keep ticking when you stand still:**

| Site | Return addr | Moving | Stationary | What it is |
|---|---|---|---|---|
| **site0** | `mod+0x4CCF62` | every frame | **every frame** | **THE RENDER VIEW** |
| site3 | `mod+0x491C86` | every frame | **frozen** | movement-gated |
| site2 | `mod+0x2BA912` | ~8/sec | frozen | movement-gated |
| site4 | `mod+0x2A8848` | ~1/sec | frozen | movement-gated |
| site1 | `mod+0x4CB8DD` | once ever | frozen | spawn-time |

**Write ONLY to site0.** Sites 2/3/4 are movement/physics/AI code *consuming* the
view result. Stuffing head rotation into them could **steer your character when you
turn your head** — silently breaking the aim/view decoupling that motion controls
depend on. That failure would look like "movement feels weird sometimes" and be
hell to diagnose.

**Auto-detect, don't hardcode `0x4CCF62`:** bucket calls by `_ReturnAddress()`,
target the site with the highest call count. site0 leads from the first call and
can't be overtaken — it's the only one that ticks while standing still.
Self-correcting across patches. **Implemented and working.**

---

## 5. Rotator ↔ basis math (needed for head tracking)

UE convention: **forward = +X world, right = +Y world, up = +Z.**

```
rotator_to_basis(FRotator r):
    pitch,yaw,roll = radians(units_to_deg(each))
    cp,sp=cos/sin(pitch); cy,sy=cos/sin(yaw); cr,sr=cos/sin(roll)
    forward = ( cp*cy,  cp*sy,  sp )
    right0  = ( -sy,     cy,    0  )
    up0     = ( -sp*cy, -sp*sy, cp )
    right = right0*cr + up0*(-sr)
    up    = right0*sr + up0*cr

basis_to_rotator(Basis b):
    pitch = asin(clamp(b.forward.z,-1,1))
    yaw   = atan2(b.forward.y, b.forward.x)
    cp = cos(pitch)
    if |cp|>1e-6: right0=(-sin yaw, cos yaw, 0)
                  up0   =(-sin p*cos y, -sin p*sin y, cp)
    else:         right0=(b.right.x, b.right.y, 0)
                  up0   =(0,0,sign(cp))            # gimbal pole
    roll = atan2(-dot(b.right,up0), dot(b.right,right0))
    return FRotator(deg_to_units of each)
```

World-space yaw is the default and is what VR wants:
```
apply_world_space_yaw(clean, yaw, pitch, roll):
    yawed     = rotate_basis_about_WORLD_Z(rotator_to_basis(clean), radians(yaw))
    pitchroll = rotator_to_basis(FRotator(pitch, 0, -roll))
    return basis_to_rotator(mul_basis(yawed, pitchroll))

mul_basis(a,b)   = Basis(transform_vec(a,b.forward),
                         transform_vec(a,b.right),
                         transform_vec(a,b.up))
transform_vec(b,v) = b.forward*v.x + b.right*v.y + b.up*v.z
```

**Roll is INVERTED** vs most sources: BioShock's `FRotator.Roll` increases
clockwise around the view axis. **Note the `-roll`.**

**We do NOT pass the Quest's canted per-eye FOV into the game.** The game renders
one centred symmetric view; we report a symmetric FOV to the compositor and that is
what made the eyes fuse. Eye separation is a *position* offset, not an FOV change.

---

## 6. Stereo — the whole thing (WORKING)

```cpp
// In the detour on site0, after calling original and snapshotting `clean`:
FRotator final = ApplyWorldSpaceYaw(clean, hmdYawDeg, hmdPitchDeg, hmdRollDeg);
*CameraRotation = final;                  // head tracking; today final == clean

Basis b = RotatorToBasis(final);
float s = g_eyeSign * 3.2f;               // half-IPD, cm == units (MEASURED)
CameraLocation->x += b.right.x * s;
CameraLocation->y += b.right.y * s;
CameraLocation->z += b.right.z * s;
```

The *engine* renders each eye with a camera it believes, so **culling, gun,
shadows, water, and post-fx are all correct for free.** No matrix interception.

**Eye offset goes along the FINAL (head-rotated) right vector**, not the clean one.
(itsloopyo uses clean yaw for positional lean — right for leaning, wrong for eye
separation.)

### MANDATORY AER RULE for head tracking
**Sample the HMD pose ONCE per eye-pair and use it for BOTH eyes.** If you
re-sample per game frame, a head turn between the two frames renders the eyes from
*different head rotations* — a real stereo mismatch your brain fights.

**This applies to the pose written INTO THE GAME CAMERA — not the pose on the
composition layer.** The layer pose should stay **fresh on every submit** (that's
what keeps timewarp smooth). The *camera* pose must be **held constant across both
frames of a pair.** Easy to conflate. Get it right.

---

## 7. Quest 3 per-eye frustums (measured, logged once at session start)

```
eye 0 fov  L-54.0  R40.0  U44.0  D-55.0 deg
eye 1 fov  L-40.0  R54.0  U44.0  D-55.0 deg
```
Each eye is **~94° horizontal × ~99° vertical — near-square, and canted** (the
asymmetry between eyes is the toe-in). We do NOT feed these to the game.

---

## 8. FOV — and the black bars

- **Stock FOV is 100° horizontal.** The live in-game slider value **cannot be read
  from memory** (itsloopyo hit the same wall). So the user declares it in
  `BioshockVR.ini` → `GameFovDegrees`. **Slider and ini MUST match** — the moment
  they diverge, the turn-warp returns.
- We report the GAME's true FOV to OpenXR as a **symmetric `XrFovf`** (horizontal
  from the ini, vertical derived from the backbuffer aspect). This killed the
  turn-warp and made the eyes fuse. **Working.**

### THE BLACK BARS ARE AN ASPECT-RATIO PROBLEM, NOT A FOV PROBLEM
Raising the FOV slider will **never** fix them.

`Bioshock.ini` has **`HorizontalFOVLock=True`**. The slider sets HORIZONTAL fov;
vertical is *derived from the aspect ratio*:
```
vFov = 2 * atan( tan(hFov/2) * h/w )
```
At 16:9 the vertical is permanently squashed:
```
GameFovDegrees=100 → 100.0h /  67.7v    (bars top & bottom)
GameFovDegrees=105 → 105.0h /  72.5v    (still bars; just a wider image)
```
The Quest 3 eye is **~94° × ~99° — nearly square.** Feeding it 16:9 **is** the bars.

**FIX: render at a SQUARE resolution.** At **1440×1440** with the slider at 100:
```
vFov = 2*atan(tan(50°) * 1.0) = 100°     →  100°h × 100°v
```
Overfills the Quest in **both** axes. **Bars gone.**

**And it's free:** 1920×1080 = 2.07 MP; 1440×1440 = 2.07 MP. **Identical pixel
count** — you're moving pixels from the sides (wasted) to the top and bottom
(currently empty). Same GPU cost.

**No code changes needed** — `XR_SetGameFov` already derives vertical from `h/w`,
and `g_bbW/g_bbH` are measured from the real swapchain. It adapts automatically.
Force the resolution via `Bioshock.ini` (the menu won't offer 1:1).

---

## 9. D3D11 hooks — vtable indices

```
IDXGISwapChain      vtable[8]  = Present       ← HOOKED, working
ID3D11DeviceContext vtable[12] = DrawIndexed   ← HUD phase
ID3D11DeviceContext vtable[13] = Draw          ← HUD phase
```
Vtable grabbed from a throwaway 100×100 device+swapchain at init; all instances
share the vtable. MinHook (`MH_Initialize/CreateHook/EnableHook`). **Done, stable.**

---

## 10. HUD suppression (future phase)

Scaleform HUD identified by draw-call vertex counts:
```
DrawIndexed(234) = compass
Draw(11)         = health / EVE
Draw(9)          = gun reticle
Draw(21)         = plasmid reticle
```
**Gate the suppression:** only drop `Draw(9)` / `Draw(21)` when a HUD-active flag
is set (raised by a `234` or `11` draw, cleared each Present) — otherwise you eat
world particles that happen to have the same vertex counts.
