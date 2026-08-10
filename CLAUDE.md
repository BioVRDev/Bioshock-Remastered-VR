# BioShock Remastered VR — working notes for Claude

Native VR mod for BioShock Remastered (`BioshockHD.exe`, 32-bit, D3D11, UE2.5
fork with no reflection system). Everything here was found by scanning and
measuring, not read out of an SDK.

## Read this before touching code

1. **`docs/CODEMAP.md`** — the index. Find the module you need, then load only
   its `docs/modules/*.md`. Do not read whole source files to orient yourself.
2. **`docs/INVARIANTS.md`** — things that are settled and things that are dead.
   Check it before proposing anything; several plausible ideas in this project
   have already been built, measured, and falsified.
3. **`.planning/STATE.md`** — what is actually true right now.
4. **`docs/STYLE.md`** — how this codebase is written. There is no
   auto-formatter, deliberately; do not bulk-reformat anything.

`docs/ENGINE-MAP.md` (memory offsets) only when touching engine memory.

## Build

Two projects, both **`Release | Win32`**. Win32 is mandatory — the game is 32-bit.

```bash
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" BioshockVR/BioshockVR.vcxproj -p:Configuration=Release -p:Platform=Win32 "-p:SolutionDir=C:\dev\Bioshock-Remastered-VR\" -v:minimal
```

**`-p:SolutionDir` is required** when building a `.vcxproj` directly. Both
projects resolve `third_party\` and `packages\` through `$(SolutionDir)`, which
is empty outside a solution build — the result is
`Cannot open include file: 'MinHook.h'`. The trailing backslash matters.

- `BioshockVR.vcxproj` → `Release/BioshockVR.dll`, the mod.
- `OpenXRShim/OpenXRShim.vcxproj` → `Release/openxr_loader.dll`, the SteamVR shim.
  Sources are in `OpenXRShim/src/`.
  **`Release|Win32` is the only config that produces a DLL.** The other three are
  `ConfigurationType=Application` and silently build an `.exe`.
- `/MT` runtime, so there is no VC++ redistributable dependency. Keep it.
- The shim must never be linked into `BioshockVR.dll`.

There is no deploy script. Claude copies the built DLL into the game folder
directly (see Test loop below) — it is writable without elevation. The DLL goes
**directly beside `BioshockHD.exe`**; there is no `BioPlugins` folder.

A third project, `dxgiproxy/dxgiproxy.vcxproj` → `Release/dxgi.dll`, is the
loader that pulls the mod in. It rarely changes. `Release|Win32` is the only
config with `ModuleDefinitionFile` set, so the other three build a DLL that
exports nothing the game looks for.

## Test loop

**Claude builds and deploys. The user only launches the game.**

1. Build both projects as above.
2. **Copy `Release/BioshockVR.dll` into the game folder yourself** — it is
   writable without elevation. Do not ask the user to run a deploy script.
   Back up the existing DLL first if the change is structural.
3. Tell the user what to do in the headset and what to look for.
4. **Read `…\Build\Final\logs\BioshockVR.log` directly** — do not ask them to
   paste it. The mod truncates it at startup, so it always contains one run.

The game must be closed before copying. There is no test suite and no way to
automate the verification step itself; a human has to put the headset on.

**Check the build stamp first, every time.** A stale DLL has invalidated seven
sessions of this project. `dllmain build: …` in the log must have advanced.

Then: `>>> XR: runtime =` correct · `EYEQ: depth min=1 max=1` · `hud: host found`
nonzero · `POLL:` showing `synth` high and `realpad 0`.

Full route when a change could touch presentation:
main menu → load → combat → crate → vending machine → **Little Sister rescue** →
pause. The rescue is the reproducible trigger for the HUD square.

## How to work here

- **Exact anchored edits.** Give a Ctrl+F target, then *add above*, *add below*,
  or *replace*. Never line numbers — this file moves constantly. Never "near the
  top" or "with the other externs".
- **One change per test cycle.** Build, deploy, run, read the log, then continue.
  Batched changes cannot be attributed when something breaks.
- **Say whether a change is diagnostic**, so the tester knows whether to expect a
  visible difference or only a log line.
- **Prefer an INI switch to a rebuild.** New behaviour ships default-off.
- **Measure before theorising.** Every long dead end in this project came from
  reasoning about what the code should do instead of logging what it did. The
  HUD square was solved in one run by a throttled log line after three sessions
  of theory.
- **Fail closed.** Verify a hook's prologue, an object's identity, a vtable's
  value before writing. A refused feature beats corrupted game state.
- **Never add a per-frame memory scan.** One-shot scans that stop after locking
  are fine; use backoff for retries.
- **Retire disproved theories explicitly** in `.planning/DECISIONS.md`, with the
  evidence that killed them. Otherwise they come back.

The user is not a programmer and applies edits by hand into Visual Studio. Vague
instructions cost a whole build-and-headset cycle.

## Config

~130 settings live in `BioshockVR.ini` next to the DLL. The shipping default is
`dist/BioshockVR.ini`. **The startup config echo in the log is the authority** —
not the file in the editor. Read-only files, wrong storefront profiles, and
VirtualStore redirection have all silently defeated edits here.
