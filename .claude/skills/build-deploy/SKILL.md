---
name: build-deploy
description: Build BioshockVR.dll and/or the OpenXR shim, deploy to the game folder, and verify the build stamp actually advanced. Use whenever the user asks to build, rebuild, compile, or deploy the mod, or before any headset test.
---

# Build and deploy

## Build

Both projects are **`Release | Win32`**. Win32 is mandatory — the game is 32-bit.

```bash
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" BioshockVR/BioshockVR.vcxproj -p:Configuration=Release -p:Platform=Win32 "-p:SolutionDir=C:\dev\Bioshock-Remastered-VR\" -v:minimal
```

```bash
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" OpenXRShim/OpenXRShim.vcxproj -p:Configuration=Release -p:Platform=Win32 "-p:SolutionDir=C:\dev\Bioshock-Remastered-VR\" -v:minimal
```

Outputs land in `Release/`: `BioshockVR.dll` and `openxr_loader.dll`.

⚠ **`-p:SolutionDir` is required when building a `.vcxproj` directly.** Both
projects resolve `third_party\minhook\include` and `packages\OpenXR.*` through
`$(SolutionDir)`, which is empty outside a solution build. The symptom is
`error C1083: Cannot open include file: 'MinHook.h'` — it looks like a missing
dependency but the file is right there. The trailing backslash matters.

Visual Studio itself sets this automatically, so this only bites command-line
builds.

⚠ **`Release|Win32` is the only shim configuration that produces a DLL.** The
other three are `ConfigurationType=Application` and quietly build an `.exe`. If
you get `openxr_loader.exe`, you built the wrong config.

Report the warning count. New warnings in a behaviour-preserving change are a
finding, not noise.

## Deploy — do this yourself, do not ask the user

The game folder is **writable without elevation**. Copy the DLL directly; the
user should never have to run a deploy script.

```
C:\Program Files (x86)\Steam\steamapps\common\BioShock Remastered\Build\Final\
```

The DLL goes **directly beside `BioshockHD.exe`** — there is no `BioPlugins`
folder, despite what the old `deploy.bat` did (it has been removed).

The loader, `dxgiproxy/` → `Release/dxgi.dll`, is a THIRD project. It rarely
needs rebuilding, and replacing it is riskier than the mod DLL: get it wrong and
the game loads nothing at all. Back up the existing one first, and verify with
`dumpbin /EXPORTS` that the five game-facing names (`CreateDXGIFactory`, ...)
are present -- `ModuleDefinitionFile` is set only on `Release|Win32`.

1. Check the game is closed (`tasklist | grep -i bioshock`). A running game locks
   the DLL and the copy fails.
2. For a structural change, back up the existing DLL first
   (`BioshockVR.dll.pre-refactor-backup` or similar) so the user has a one-copy
   rollback that does not need a rebuild.
3. Copy `Release/BioshockVR.dll` → `<game>/BioshockVR.dll`.

The shim is deployed as a **pristine source** (`openxr_loader_steam.dll`), never
over the live `openxr_loader.dll` — `Setup.bat` selects the live loader by
copying, and overwriting it directly bypasses that selection. Leave a working
live loader alone unless the shim itself changed.

Note that two builds of identical shim source produce **different bytes** — MSVC
embeds a build timestamp in the PE header. Do not read a hash mismatch as a code
change; compare the sources instead.

## Verify — do not skip this

**A stale DLL has invalidated seven sessions of this project.** After every
deploy, confirm the build stamp advanced:

```
dllmain build: <date/time>
BioshockVR version: 1.0.3
```

If the stamp did not move, the game loaded an old binary and every observation
from that run is worthless.

Then the four startup health lines:

| Line | Healthy |
|---|---|
| `>>> XR: runtime =` | the runtime you expect (native vs shim) |
| `EYEQ: depth min=1 max=1` | min == max |
| `hud: host found …` | nonzero |
| `POLL:` | `synth` high, `realpad 0` |

## Then

There is no test suite and no way to automate verification — a human has to put
the headset on. Tell the user exactly what to look for and whether the change is
diagnostic (log line only) or should produce a visible difference.

If the change could touch presentation, ask for the full route:
main menu → load → combat → crate → vending machine → **Little Sister rescue** →
pause. The rescue is the reproducible trigger for the HUD square.
