# Packaging, installation, and logs

`GameIni.cpp` (218), `dist/*.bat`, `dxgiproxy/`.

## Install layout

```
...\BioShock Remastered\Build\Final\
    BioshockHD.exe
    dxgi.dll                      plugin loader
    BioshockVR.dll                the mod          ← directly here
    BioshockVR.ini                config
    openxr_loader.dll             the LIVE loader (shim OR Khronos)
    openxr_loader_standard.dll    the parked one, when SteamVR is active
      -- or --
    openxr_loader_steam.dll       the parked one, when native OpenXR is active
    Setup.bat  Uninstall.bat
    openvr_input\                 shim-generated SteamVR bindings
    logs\                         BioshockVR.log, openxr_shim.log, setup.log
```

> **There is no `BioPlugins` subfolder in a current install.** The mod DLL sits
> directly beside the executable. `deploy.bat` copied into `BioPlugins\` and
> silently failed; `deploy.bat.example` has the corrected path.

Steam AppID `409710`. The intended loader is SnowTempest's
`BSHD-PluginLoader 1.0.2` `dxgi.dll`, not the ReShade-oriented `version.dll`.

## Loader selection — a two-file swap

> **There are always exactly TWO loader files. Three is a fault state.**

`Setup.bat` swaps by **renaming** (`move /y`), not copying. The live loader is
always `openxr_loader.dll`; the *inactive* one is parked under its own name:

| Active runtime | `openxr_loader.dll` | parked file |
|---|---|---|
| SteamVR shim | the shim | `openxr_loader_standard.dll` (Khronos) |
| Native OpenXR | the Khronos loader | `openxr_loader_steam.dll` (shim) |

So the *absence* of `openxr_loader_steam.dll` is not drift — it is exactly what a
correctly-configured SteamVR install looks like. Do not "restore" it.

The script is well defended, and reading it is worth more than trusting the
strategy notes in the old handoffs:

- already-active state is detected and becomes a no-op,
- a missing live file with both sources present is recovered,
- **three valid files** are treated as "a previous COPY-based setup left a
  duplicate": it byte-compares (`fc /b`) the live loader against each source and
  deletes the redundant one. If the live loader matches *neither* — e.g. someone
  dropped in a freshly-built shim, which differs from the deployed one only by
  its PE build timestamp — it bails to `:loaderambiguous` rather than guessing.

That last branch is why an extra `openxr_loader_steam.dll` breaks runtime
switching with a confusing error rather than destroying anything.

The earlier handoffs record "copy, never rename" as the intended strategy, on the
grounds that renaming consumes the source. The shipped script does the opposite
and handles it correctly. **The implementation is the authority here**; if the
strategy is ever revisited, that is a decision to make deliberately, not a bug
to fix.

`openxr_loader_steamvr.dll` is a legacy alias — uninstall should clean it up, but
it is not a current source name.

Headset choice and runtime choice are **separate questions** in both the installer
and the logs. A 32-bit runtime registration at
`HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` says which runtime is
active, not which headset is being worn — so detection may recommend, never
silently override.

| Configuration | Path |
|---|---|
| Quest/Rift via Meta/Oculus OpenXR | Standard loader |
| Quest/Pico via Virtual Desktop / VDXR | Standard loader |
| Pimax, Reverb G2 / WMR with a working x86 runtime | Standard loader |
| Index, Vive, Vive Pro, Bigscreen Beyond, Varjo, Somnium via SteamVR | **Shim** |
| Pico Streaming Assistant without an x86 runtime | Shim, or recommend VD/VDXR |

Reverify before a release; vendor runtime support changes.

## Game INI synchronisation

`SyncGameIni()` pushes the VR-relevant values into the game's `Bioshock.ini` so
the mod's reported FOV and the rendered image cannot drift apart — a 10° drift was
the original turn-warp bug. It writes exactly five keys — `HorizontalFOV`,
`bHorizontalFOVLock`, `HorizontalFOVLock`, `WindowedViewportX/Y` — using
`WritePrivateProfileString`, the same API the game uses, updating keys in place
rather than rewriting the file. Everything else is left strictly alone.

The game reads its config at startup and rewrites it on exit with whatever it
actually ran at, so a fresh install starts fullscreen at desktop resolution and
can never correct itself. Setting the values *before* the game starts breaks that
loop; that is what `Setup.bat` is for.

The game uses **separate windowed and fullscreen viewport dimensions** — write
both. A read-only `Bioshock.ini` silently blocked synchronisation for a long
period, so always check write-failure logs and readback.

## Configuration authority

`BioshockVR.ini` lives beside the DLL; the mod finds it from its own module path,
so a copy anywhere else is silently ignored. `dist/BioshockVR.ini` is the
shipping default.

**The startup config echo in `BioshockVR.log` is the authority, not the editor.**
Read-only files, wrong storefront profiles and VirtualStore redirection have all
silently defeated edits.

Six keys appear twice in the shipped INI — `ControllerDeadzone`, `DisableHeadBob`,
`GripHysteresis`, `GripThreshold`, `HeadRelativeMove`, `SwingEnabled`. **Both
occurrences of each carry identical values.** They are deliberate documentation
repeats between the "start here" block and the numbered reference section, not a
hazard. Windows reads the first occurrence. Verified 2026-08 — do not "fix" them.

INI formatting rules that bite: comments must be on their own line (a trailing
comment is read as data), `;` comments the whole line out, environment variables
are **not** expanded, and unknown keys are ignored silently — so a typo looks
exactly like a working setting.

## Logs

| File | Location |
|---|---|
| `BioshockVR.log` | beside the DLL |
| `openxr_shim.log` | **`logs\`**, not beside the DLL — moved; verify `CollectLogs.bat` |
| `setup.log` | `logs\` |
| `Bioshock.log` | game profile folder — **only opened at shutdown**, so it can never contain anything from a live session |

Support bundle: `CollectLogs.bat` should gather mod, shim, installer, INI and
runtime logs into one folder, and detect VirtualStore redirection.

## Known documentation drift

- `README.md` tells users to run `FirstTimeSetup.bat`; the shipped file is
  `Setup.bat`.
- `README.md` lists four install files; the real package is larger.

## Build identity

Visual Studio **Release | Win32**, `/MT` runtime (no VC++ redistributable
dependency), MinHook + D3D11 + OpenXR headers. `/PDBALTPATH:%_PDB%` avoids
embedding a local username in the shipped DLL. The shim is a separate project
and must not be linked into `BioshockVR.dll`.
