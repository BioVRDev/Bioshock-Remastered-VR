# BioShock Remastered VR

Native VR for BioShock Remastered. Real stereo rendering, head tracking, motion
controllers and 6-DOF weapon holding.

Built by hooking the game directly: `IDXGISwapChain::Present` for the frame,
`APlayerController::eventPlayerCalcView` for the camera, and the D3D11 draw path
for the interface. The engine is a heavily modified Unreal 2.5 fork with no
reflection system, so everything here was found by scanning and measuring rather
than read out of an SDK dump.

---

## Install

1. Download the latest release.
2. Extract all four files next to `BioshockHD.exe`:

```
...\BioShock Remastered\Build\Final\
    BioshockHD.exe
    dxgi.dll
    BioshockVR.dll
    BioshockVR.ini
    FirstTimeSetup.bat
```

3. **Run `FirstTimeSetup.bat` once, with the game closed.**
4. Put your headset on and launch the game normally.

No injector, no plugin folder, no launcher.

### What the setup step does

The game reads its config at startup and rewrites it on exit with whatever it
actually ran at, which is why a fresh install starts fullscreen at your desktop
resolution and can never correct itself. `FirstTimeSetup.bat` sets the values
before the game ever starts, which breaks that loop for good:

- render resolution and FOV the mod needs
- windowed mode instead of exclusive fullscreen
- anisotropic filtering to x16

It finds `Bioshock.ini` on its own, backs it up first, and changes ten keys in
place using the same Windows API the game itself uses — it does not rewrite the
file. If anything looks wrong afterwards it restores the backup automatically.

Run it again any time you change `ResolutionX`, `ResolutionY` or
`GameFovDegrees` in `BioshockVR.ini`.

---

## Requirements

- BioShock Remastered on PC (developed and tested against the Steam build)
- Any OpenXR headset and runtime — Quest via Link or Air Link, SteamVR, WMR
- Windows 10 or 11

---

## What it does

**Rendering**
- True stereo, one eye per frame, both eyes locked to the same instant so near
  geometry fuses instead of shimmering
- The FOV reported to the runtime is kept in sync with what the game actually
  renders, so turning doesn't warp
- Automatic guard on the game's world FOV, which otherwise sticks wide after a
  Vita-Chamber respawn and narrow during scripted sequences

**Head and camera**
- Full head tracking, rotation and position
- Pitch decoupled from the game camera — the horizon stays level
- Walking head bob removed at the source
- Adjustable height and IPD

**Hands and weapons**
- 6-DOF weapon holding driven by the right controller
- Aim, crosshair and the actual shot all derive from one value, so where the dot
  is is where the bullet goes
- Per-weapon grip position and angle, tunable live in the headset and saved
  automatically
- Forearms hidden at the skeleton while the hands and weapon stay visible
- The game's flat 2D reticle disabled at the engine level and replaced with a
  proper VR dot

**Interface**
- The HUD is captured off the eye image and composited as its own layer, so it
  sits at a comfortable depth instead of being painted onto the world
- Adjustable size and position, tunable live
- Menus and the map appear on a screen fixed in the room

**Cutscenes**
- Pre-rendered cutscenes play on a screen anchored in the room. You can look
  around it and the camera no longer follows your head — a large comfort
  improvement in the opening sequence

**Controls**
- Touch and Index controllers mapped to a virtual gamepad
- Right stick click jumps; zoom is unbound because it breaks the weapon
  calibration
- D-pad modifier on the right thumbrest

---

## Tuning

Everything here changes live, and every change logs a line you can paste back
into `BioshockVR.ini`.

| Key | What it does |
|---|---|
| <kbd>F11</kbd> / <kbd>F12</kbd> | HUD panel smaller / larger |
| <kbd>Del</kbd> | cycle which HUD property F11/F12 edits |
| <kbd>Home</kbd> | toggle the HUD panel off, to compare |
| <kbd>Numpad 9</kbd> | cycle weapon grip mode: position / angle / aim |
| <kbd>Numpad 8 2 4 6 0 5</kbd> | adjust the current mode |
| <kbd>Numpad 7</kbd> | change step size |

**Weapon grip tuning writes itself to `BioshockVR.ini` as you go.** Edit that file
with the game closed, or your changes get overwritten.

To get a weapon exactly right: set the angle first, then switch to aim mode and
fire at a flat wall, adjusting until the dot sits on the bullet hole. The dot and
the shot come from the same value, so that test is exact rather than a judgement
call.

### Weapons still to tune

Tuned: **wrench, pistol, shotgun, machine gun, plasmids**.

Not yet tuned: **crossbow, grenade launcher, chemical thrower, research camera**.
Those fall back to a generic offset and will sit wrong in your hand. Perfectly
usable — they just want five minutes each with the numpad. If you tune one, the
values land in your ini, and a pull request or a paste in the issues is very
welcome.

---

## Known issues

**`dxgi.dll` conflicts with other graphics mods.** ReShade, DXVK, Special K and
most injectors install under the same filename, and only one file can own it.
They can't currently be used together.

**Two features are build-specific.** The reticle removal and the arm hiding use
fixed addresses into the game executable. On a different build they detect the
mismatch and safely do nothing — the log says so. Set `DisableReticle=0` and
`HideArmSleeves=0` to silence it. Everything else finds its targets by scanning
and works on any build.

**Grip offsets are tuned for 2750x2850 at FOV 100.** They don't carry across
resolutions by any reliable formula — this was measured and the scaling law was
ruled out. Change either and expect to re-tune.

**In-engine cutscenes still track your head.** Only pre-rendered ones are
detected and moved to the flat screen. The opening is covered; some later
scripted sequences aren't.

**Weapon idle sway remains.** The weapon hangs off a bone of the arm mesh, so it
inherits the authored idle animation. Hiding the arms doesn't stop it.

**Cutscene letterbox bars are still there**, now on the flat screen.

---

## Performance

`ResolutionX` and `ResolutionY` in `BioshockVR.ini` are the main dial — lower
them and re-run `FirstTimeSetup.bat`. FOV below 100 looks nearly identical in the
headset but runs noticeably better, because the game stops rendering side content
that never reaches the display.

---

## Reporting a problem

Open an issue and **attach `BioshockVR.log`**, which appears next to
`BioshockVR.dll`. The config block at the top shows every setting the mod
actually read, and answers most "it's not working" reports immediately.

Quick self-checks:

| Symptom | Cause |
|---|---|
| Nothing happens at all | Is `BioshockVR_loader.log` next to the exe? If not, `dxgi.dll` isn't loading — check all files are in the same folder |
| Wrong resolution, or fullscreen | Run `FirstTimeSetup.bat` with the game closed |
| `could not find Bioshock.ini` | Launch the game once so it creates one, quit, run setup again |
| World too big or too small | `EyeSeparation` is half your IPD in cm |
| You feel too short or too tall | `CameraHeightOffset`, in cm |

---

## Building from source

- Visual Studio 2022, **Win32 / x86** — the game is 32-bit and the mod must match
- Two projects: `BioshockVR` (the mod) and `dxgiproxy` (the loader, builds as
  `dxgi.dll`)
- Dependencies: OpenXR SDK, [MinHook](https://github.com/TsudaKageyu/minhook)

Build both, then copy `BioshockVR.dll`, `dxgi.dll`, `BioshockVR.ini` and
`FirstTimeSetup.bat` next to `BioshockHD.exe`.

If you get `unresolved external symbol` from `dxgi.def`, the proxy project is set
to x64. If `dumpbin /dependents dxgi.dll` lists `DXGI.dll`, you've linked
`dxgi.lib` and the proxy will try to load itself.

---

## Credits

Built on the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) and
[MinHook](https://github.com/TsudaKageyu/minhook).

Huge thanks to **VOID** and
[mohamad-balouza/bioshock-vr](https://github.com/mohamad-balouza/bioshock-vr).
Several of the hardest parts here — disabling the reticle through the engine's
own console path, the bone indices for hiding the arms without losing the weapon,
and the render-target approach to capturing the whole interface at once — were
possible because that work is public. Genuinely saved weeks.
