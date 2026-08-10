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

## Deploy

`deploy.bat` (gitignored — the user's copy has their machine path). The DLL goes
**directly beside `BioshockHD.exe`**; there is no `BioPlugins` folder.

```
C:\Program Files (x86)\Steam\steamapps\common\BioShock Remastered\Build\Final\
```

If the copy fails: the game is open, or it needs administrator rights.

The shim is deployed as a **pristine source** (`openxr_loader_steam.dll`), not
over the live `openxr_loader.dll` — `Setup.bat` selects the live loader by
copying. Overwriting the live file directly bypasses that selection.

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
