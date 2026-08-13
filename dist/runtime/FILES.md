# Every file the release ships, and what breaks without it

Written because `openvr_api.dll` was **required at runtime, listed in no manifest,
absent from the repo, and mentioned only by `Uninstall.bat` — which deleted it.**
A package assembled by hand from memory is a package that eventually ships broken.

---

## The file set

| File | Where it comes from | Setup renames it? | Without it |
|---|---|---|---|
| `BioshockHD.exe` | the game | no | — |
| **`dxgi.dll`** | `dxgiproxy.vcxproj` → `Release/dxgi.dll` | no | **nothing loads at all.** This is the injection point: the game loads it as a system DLL and it pulls the mod in |
| **`BioshockVR.dll`** | `BioshockVR.vcxproj` → `Release/BioshockVR.dll` | no | the proxy logs `FAIL:` with a `GetLastError` code and the game runs flat |
| **`openxr_loader.dll`** | **not a file you ship** — the *active slot*. Setup copies one of the two candidates below onto this name | **yes, this is the rename target** | the mod cannot load at all: it statically imports this name |
| `openxr_loader_standard.dll` | the official Khronos **32-bit** OpenXR loader | copied to `openxr_loader.dll` when the user picks OpenXR — **the default** | the OpenXR path is unavailable |
| `openxr_loader_steam.dll` | `OpenXRShim.vcxproj` → `Release/openxr_loader.dll`, renamed on packaging | copied to `openxr_loader.dll` when the user picks SteamVR | the SteamVR path is unavailable |
| **`openvr_api.dll`** | Valve's OpenVR SDK, `bin/win32/` | no | **the SteamVR path fails and Setup used to report success anyway.** The shim loads it by name at runtime |
| `BioshockVR.ini` | `dist/BioshockVR.ini` | no | the mod runs on built-in defaults, which are not the shipping defaults |
| `Setup.bat` | `dist/Setup.bat` | no | no installer |
| `Uninstall.bat` | written by Setup from its own embedded copy | no | no uninstaller |
| `logs\CollectLogs.bat` | written by Setup from its own embedded copy | no | no support bundles |

## There are two loaders, not three

This is the part that reads as three files and is really two choices and one slot:

```
openxr_loader_standard.dll  ─┐
                             ├─ Setup copies ONE onto ─→  openxr_loader.dll
openxr_loader_steam.dll     ─┘                            (what the mod loads)
```

**`openxr_loader.dll` is never shipped as itself.** It is whichever candidate the
user selected. `Setup.bat` owns that choice and the rename logic already existed;
the only change for this release is which one is the default.

**The default is `openxr_loader_standard.dll`** — the real OpenXR loader, which
routes to whatever OpenXR runtime the system has active (Meta, SteamVR, VDXR,
WMR, Varjo…). The SteamVR shim is the fallback for setups where no **32-bit**
OpenXR runtime is registered, which is the case that used to send people to the
shim by default.

> **32-bit matters and is easy to miss.** BioShock Remastered is a 32-bit game, so
> it needs a 32-bit OpenXR runtime. A headset whose runtime ships x64 only cannot
> serve it through the standard loader however well it works elsewhere — that is
> what the shim exists for.

## Why the shim discards suggested bindings

Worth knowing before anyone debugs a controller. The shim implements
`xrSuggestInteractionProfileBindings` as a log line and a success return — it
authors its own SteamVR manifests instead, and binds by **action name**.

So the mod's binding tables in `Input/InputHook.cpp` matter **only on the standard
loader**, and the shim's manifests in `OpenXRShim/src/shim_input.cpp` matter
**only on the shim**. Both need to be edited when a control changes, and the log
line `>>> INPUT: bound profile (…) =` is what tells you which one was in force.

## The import contract, which has broken the build twice

`BioshockVR.dll` **statically imports** `openxr_loader.dll`. Calling any `xr*`
function the active loader does not export makes Windows refuse to load the mod
entirely, and the proxy reports it as a missing file.

**Check before every release:**

```bash
dumpbin /imports BioshockVR.dll    # vs    dumpbin /exports openxr_loader_steam.dll
```

Every `xr*` import must appear in the shim's exports. Anything outside that set
must be resolved at runtime through `xrGetInstanceProcAddr` instead — a null
return is harmless, a missing static import is fatal. `Input_Pulse`
(`xrApplyHapticFeedback`) and the binding census (`xrGetCurrentInteractionProfile`,
`xrPathToString`) are the three that do this today.

## Assembling a release

1. Build all three projects `Release | Win32` — `BioshockVR.dll`, `dxgi.dll`, and
   the shim.
2. Rename the shim's output to `openxr_loader_steam.dll`.
3. Take `openxr_loader_standard.dll` and `openvr_api.dll` from this folder.
4. Run the import contract check above.
5. Package with `BioshockVR.ini`, `Setup.bat` and `changelog.txt`.
   `Uninstall.bat` and `CollectLogs.bat` are written by Setup and are **not**
   packaged separately.
