# BioShock Remastered VR — HANDOFF (rev 4)

**This is the only always-on doc. It is deliberately short. Three companion docs
live on my disk, NOT in project knowledge — see §8. ASK ME FOR THEM when you need
them; I'll drag them in. Don't guess at their contents.**

---

## 0. THE PROJECT

Native VR mod for **BioShock Remastered**. Real stereo, engine-native, submitted
to a **Quest 3** over **Virtual Desktop** via **OpenXR**. Not vorpX, not a
wrapper. C++, **32-bit (x86) DLL**, loaded by SnowTempest's BioPlugins loader.

**Released anonymously.** Never use a real name. Never hardcode a path containing
a Windows username. Assume the DLL and repo become public.

**Status: Phases 0–8 DONE.** Real stereo depth works, scaffolding is stripped to
one submit path. 236 Present/s, 118 unique frames/eye, 4.2ms inter-eye disparity,
headset at 120Hz. **No head tracking yet** — image welded to the skull. **16:9
image in a near-square headset FOV** — black bars top/bottom (Phase 9 fixes this).

## 1. CURRENT PHASE — Phase 9: SQUARE RENDER

**Zero mod code changes.** This is a config change plus verification, per
`ROADMAP.md`. No files need uploading unless the test fails.

**Why FOV can't fix it:** `Bioshock.ini` has `HorizontalFOVLock=True` — the
slider sets horizontal FOV only; vertical is derived from aspect ratio. At 16:9
the vertical is permanently squashed. The Quest 3 eye is ~94°×99° — nearly
square. Feeding it 16:9 IS the bars.

**The fix:** force the game to **1440×1440** via `Bioshock.ini` (not the mod's
ini). At slider=100 that gives 100°h × 100°v — overfills the Quest both axes.
**Free** — 1920×1080 and 1440×1440 are both 2.07MP, same GPU cost, just moving
pixels from the (wasted) sides to the (empty) top/bottom.

`XR_SetGameFov` already derives vertical from measured `g_bbW/g_bbH`, so it
adapts automatically — no code change needed there.

**Test:** log reads `backbuffer : 1440 x 1440` and `GAME FOV = 100.0 h / 100.0
v` (adjust the 100 if `GameFovDegrees` in `BioshockVR.ini` differs from the
in-game slider — they must match, see §9). Bars gone top and bottom.

**Watch for:** Scaleform HUD is laid out for 16:9 and may look odd at 1440×1440.
Expected — suppressed in Phase 13. Not a bug to chase now.

### Phase 8 (scaffolding strip) — DONE, COMMITTED, PUSHED.
`XRSession.cpp` now has one submit path: `XR_SubmitPair(image, eye)` — eye 0
stashes into `g_stageL`, eye 1 calls the `SubmitPair()` helper with (stashed L,
this R). `XRMode` and all dead cadence paths (`XR_SubmitMono`, `XR_SubmitAER`,
`XR_SubmitAERStable`) are gone, recoverable via git history only. Verified in
headset + log: 236 Present/s, 118 submit/s, EYEQ 1/1, writes L≈R.

**Carried forward:** mono-fallback for loading screens (§9 open item) still
exists as `SubmitPair(image, image)` — not wired up, three lines away. Eye-tag
FIFO and both timing heartbeats untouched — still load-bearing for Phase 10+,
do not remove.

## 2. WHO I AM

Comfortable in **C++**. **NOT** fluent in **Git/GitHub** or **Visual Studio
project configuration** — walk me through those click by click, exact menu names
and paths.

**I push back when your explanation doesn't match what I see. Take it seriously.**
In Phase 6/7 the assistant proposed three wrong causes for a framerate bug in a
row; the real cause was only found because I refused to accept them. **When I say
"that doesn't sound right," stop theorising and measure.**

**I am burning tokens fast.** Be concise. Don't re-explain settled things. Don't
restate this doc back at me. Prefer **precisely anchored edits** over full-file
dumps unless we've genuinely diverged.

## 3. NON-NEGOTIABLE INVARIANTS

Violating any of these silently corrupts the mod. They are all MEASURED.

- **1 unit = 1 cm.** half-IPD = **3.2 units**, literally, no calibration. (Classic
  BioShock was ~2cm — do NOT carry that over.)
- **Write ONLY to site0** (`mod+0x4CCF62`) — the render view. `eventPlayerCalcView`
  has **5 call sites**; the other four *consume* the view. Writing to them lets
  head-look steer the character. **Auto-detected by call count, never hardcoded.**
- **Backbuffer is DXGI 28** (`R8G8B8A8_UNORM`). **XR swapchain is DXGI 29**
  (`R8G8B8A8_UNORM_SRGB`). Same typeless family → `CopyResource` is legal.
  **NEVER search for a BGRA format** — illegal cross-family copy, black headset,
  no error. Dead end, already removed.
- **Call the ORIGINAL first**, then overwrite. It rebuilds the out-params from
  scratch each call, so they arrive clean. Absolute offset, no accumulator.
- **Out-pointers are the caller's STACK LOCALS.** Not stable across frames. Never
  cache on pointer identity.
- **We do NOT feed the Quest's canted per-eye FOV to the game.** The game renders
  one centred symmetric view; we report a symmetric FOV to the compositor. That's
  what made the eyes fuse. Eye separation is a *position* offset, not an FOV change.
- **If ANY stage of the camera search fails, install NO hook.** A wrong hook
  corrupts the stack and kills the game instantly.

## 4. THE EYE-TAG FIFO — cross-thread, and it matters

**MEASURED: `CalcView` runs on the GAME thread; `Present` runs on the RENDER
thread.** Different thread IDs, confirmed in the log. There is a pipeline between
them.

**So Present CANNOT flip a shared flag and expect the next CalcView to see it.**
The eye tag **travels with the frame**: the camera hook (producer) tags every site0
view with the eye it applied and pushes to a FIFO; Present (consumer) pops in
order. A one-frame slip can never accumulate.

Measured queue depth: **rock-steady 1.** `writes L` ≈ `writes R`.

**Phase 12 will read this FIFO for a second purpose** (zeroing the tick delta on
the second Present of each pair) — do not refactor it away or assume it's only
for eye selection.

## 5. PERFORMANCE MODEL — the one-paragraph version

**Virtual Desktop silently drops the Windows desktop from 240Hz to 120Hz while
streaming** (I have now disabled that VD setting). **And the game's swapchain is
blt-model, which DWM throttles to the desktop composition rate regardless of
`SyncInterval`.** Therefore **Present rate == desktop refresh rate, always.**

- Game renders a frame in **~1.5ms** (~690fps capable). **The game has NEVER been
  the bottleneck.**
- The pair cadence needs **2 Presents per compositor frame** → **desktop must be
  ≥2× headset refresh.** 240/120 on my machine today. **This hardware dependency
  must not survive to release — Phase 10 (mirror throttle) removes it** by
  starving DWM of most Presents so it can never throttle us.

**DEAD THEORIES — all proposed, all disproven by measurement, DO NOT RESURRECT:**
vsync · `xrEndFrame` · `xrWaitFrame` · GPU/encoder load · "the game is too slow" ·
"move the XR loop to its own thread" · hooking swapchain creation to force
flip-model + `ALLOW_TEARING` (rejected — see `PERF.md` §4). **Full graveyard with
the evidence that killed each one is in `PERF.md`. Ask for it before theorising
about any framerate number.**

## 6. ENVIRONMENT

```
C:\Program Files (x86)\Steam\steamapps\common\BioShock Remastered\Build\Final\
├── BioshockHD.exe
├── dxgi.dll            ← SnowTempest's loader (regular BSHD-PluginLoader 1.0.2,
│                          NOT the ReShade/version.dll variant)
├── openxr_loader.dll   ← NuGet, x86. MUST sit next to the EXE, not in BioPlugins\
└── BioPlugins\
    ├── BioshockVR.dll  ← MY MOD
    ├── BioshockVR.log  ← my runtime log (truncated each launch)
    └── BioshockVR.ini  ← my config
```
Game config INIs (`Bioshock.ini`, `User.ini`) live in **AppData**. Phase 9 edits
`Bioshock.ini` directly — that's a game config file, not the mod's.
Repo: `C:\dev\Bioshock-Remastered-VR\`. Steam AppID **409710**.

- Runtime is **`VirtualDesktopXR`** (VDXR), 32-bit. **Do NOT route me to SteamVR
  or OpenComposite.**
- **NuGet `OpenXR.Loader` ALONE is sufficient** (ships headers AND .lib). Do NOT
  add `OpenXR.Headers`.
- **Launch order is mandatory:** VD Streamer → Quest connected → THEN the game.
- Backbuffer currently **1920×1080** (moving to **1440×1440** this phase),
  windowed, no MSAA, BufferCount 1. Feature level 11_0.
- **Build config:** Release / **Win32** (Win32 == x86 — do NOT create a separate
  "x86" platform). Dynamic Library. Runtime Library **/MT**. Include dir
  `$(SolutionDir)third_party\minhook\include`. Linker Additional Options
  **`/PDBALTPATH:%_PDB%`** (keeps the username out of the shipped DLL).
- **Never committed:** `Release/`, `third_party/`, `deploy.bat`, the ini, `.pdb`.

`BioshockVR.ini` (current, post Phase 8 — `XRMode` removed):
```ini
[VR]
GameFovDegrees=105    ; MUST match the in-game slider or turn-warp returns.
                      ; ini currently says 105 — confirm slider matches (§9).
EnableCameraHook=1    ; kill switch: don't install the hook at all
EnableCameraWrite=1   ; kill switch: hook logs but doesn't modify the camera
EyeSeparation=3.2
SwapEyes=0            ; if depth ever looks inside-out (hollow-mask)
DisableVSync=1
```
Read once at init. **We never WRITE it** — Program Files is UAC-protected and a
write gets silently redirected to VirtualStore.

## 7. HOW WE WORK

```
1. I state the phase goal.
2. You give me code (anchored edit, or full file if we've diverged).
3. I paste into VS, build, deploy (deploy.bat as Admin), AND RUN THE GAME.
4. I report what I saw + paste the log.
5. You read the log, correlate, iterate.
6. It works → YOU TELL ME TO COMMIT (commit, THEN push — two separate steps).
```

- **I run the game. You can't. Compiling is NOT working.** After handing me
  something, STOP and tell me exactly what to run and what to look for.
- **Design experiments I can see in seconds or read in the log.** Log aggressively
  — the log is your only window into runtime. **Throttle it**: heartbeat once a
  second, never per-frame at 118fps.
- **Do NOT stack an unverified change on top of another unverified change.**
- **Always name the file. Always give an exact anchor** ("replace the whole
  `hkPresent`"), never "add this somewhere."
- **Track your own edits within a chat.** The uploaded files are my LAST COMMIT,
  not a live mirror.
- **DIVERGENCE IS THE DANGER.** If a bug doesn't match your mental model, suspect
  my disk drifted from your picture BEFORE you suspect the logic. Ask me to paste
  the file. **If I say "I haven't applied your last message yet," RE-SYNC WITH
  FULL FILES** — do not hand me anchored edits assuming code I never pasted.
- **WHEN UNSURE: MEASURE, DON'T THEORISE.** Phase 6/7 lost four hours to three
  consecutive plausible, wrong theories. The answer came from a 30-second run with
  the suspect feature **turned off entirely**. The best experiments DISABLE
  something. Put timers around individual API calls.
- If you'd be **guessing at something that could write wrong data into the game**,
  STOP and give me a short numbered list of questions.
- **One phase, one chat.** At the boundary I'll ask for a new handoff.

**Known traps:**
- **VD CHANGES YOUR DESKTOP REFRESH RATE.** If Present/s ever halves for no
  reason, check Windows display settings BEFORE debugging code.
- **UAC VirtualStore.** If a log looks STALE (right content, old timestamp),
  Windows redirected it to `%LOCALAPPDATA%\VirtualStore\...`. Check there first.
  Also why `deploy.bat` needs Admin.
- **The game lies in its own INI.** Menu says vsync off; `Bioshock.ini` says
  `UseVSync=True`, and the DX11 renderer reads the latter. `UseMultithreadedRendering=0`
  while demonstrably running two threads. **Never trust a game setting — measure
  what the game DOES.**
- **DUPLICATE SOURCE FILES.** All source lives ONLY in
  `C:\dev\Bioshock-Remastered-VR\BioshockVR\`. VS *Add → Existing Item* references
  files in place — copy into the project folder FIRST, then add.
- **Known high-FPS engine issues** (ragdoll bugs, animation stutter above 60fps).
  We run at 236fps — but so did the game before the mod. **Do NOT uncap further.**
- **Antivirus** may quarantine hook DLLs. No log at all → check Defender.

## 8. COMPANION DOCS — ASK FOR THEM, DON'T GUESS

These are on my disk, **not in project knowledge**, to keep your per-turn context
small. **When you need one, ASK and I'll attach it.** Never invent their contents.

| File | Contains | Ask for it when |
|---|---|---|
| **`SPEC.md`** | The 6-stage FName search; `__fastcall`+dummy-EDX calling convention; rotator↔basis math; the 5 call sites table; HUD vertex counts | Touching `CameraHook.cpp`, head tracking (Phase 11), or HUD (Phase 13) |
| **`PERF.md`** | Full performance model; the dead-theory graveyard with the evidence that killed each; the DWM/blt-model explanation; the flip-model rejection (§4) | ANY framerate number looks wrong, or before Phase 10 |
| **`ROADMAP.md`** | Phases 9–13 in detail, plus backlog | At each phase boundary (already loaded for Phase 9) |

**Likewise, ask for source files you need.** I only upload what the current phase
touches. Phase 9 needs **none** unless the test fails.

## 9. OPEN ITEMS
- Verify the in-game FOV slider matches `GameFovDegrees` (ini currently 105 —
  confirm, or turn-warp returns and stereo judgment is corrupted). **Do this
  before or during Phase 9**, since the FOV number is central to the test.
- Verify no `.pdb` ever reached the repo (`git add .` was used once).
- Close-object misalignment — not characterised. **Don't chase it until head
  tracking lands (Phase 11)**; a head-locked image makes it impossible to judge.
- Loading screens/movies run ~15fps in-headset (30fps source ÷ pair cadence).
  Fix: fall back to mono-every-Present when the camera hook isn't ticking (we
  already detect this — `EYEQ underruns`). Backlog, not yet scheduled.
- Window centering on ultrawide launches top-left. Cosmetic, backlog.
