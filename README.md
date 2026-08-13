# BioShock Remastered VR

Native VR for BioShock Remastered. Real stereo rendering, head tracking, motion
controllers and 6-DOF weapon holding.

Built by hooking the game directly: `IDXGISwapChain::Present` for the frame,
`APlayerController::eventPlayerCalcView` for the camera, and the D3D11 draw path
for the interface. The engine is a heavily modified Unreal 2.5 fork with no
public SDK, so everything here was found by scanning and measuring rather than
read out of a dump.

I would highly recommend you install these two mods for the best experience:

HD Textures: https://www.nexusmods.com/bioshock/mods/54

Cutscene Black Bar Removal: https://www.nexusmods.com/bioshock/mods/81?tab=description

---

## Install

1. Download the latest release.
2. Extract everything next to `BioshockHD.exe`:

```
...\BioShock Remastered\Build\Final\
    BioshockHD.exe
    dxgi.dll                     loads the mod
    BioshockVR.dll               the mod
    BioshockVR.ini               settings
    openxr_loader_standard.dll   the normal OpenXR loader
    openxr_loader_steam.dll      the SteamVR one
    openvr_api.dll               needed by the SteamVR one
    Setup.bat
    Uninstall.bat
    README.txt
    changelog.txt
    logs\CollectLogs.bat
```

`Setup.bat` renames whichever loader you choose onto the name the mod actually
loads, `openxr_loader.dll`, so **that file appears after setup rather than in the
download — and the game will not start in VR until you run it.** `Setup.bat` also
writes its own copies of `Uninstall.bat` and `logs\CollectLogs.bat`, so a package
missing either one still ends up complete.

3. **Run `Setup.bat` once, with the game closed.** It asks which headset you
   have and which runtime to use, then installs the matching loader.
4. Put your headset on and launch the game normally.

No injector, no plugin folder, no launcher.

### What the setup step does

The game reads its config at startup and rewrites it on exit with whatever it
actually ran at, which is why a fresh install starts fullscreen at your desktop
resolution and can never correct itself. `Setup.bat` sets the values
before the game ever starts, which breaks that loop for good:

- render resolution and FOV the mod needs
- windowed mode instead of exclusive fullscreen
- anisotropic filtering to x16

It finds `Bioshock.ini` on its own, backs it up first, and changes fourteen
values across eleven settings in place, using the same Windows API the game
itself uses — it does not rewrite the file. If anything looks wrong afterwards it
restores the backup automatically.

It also asks which headset you have and which runtime to use, and writes two mod
settings from your answers: the d-pad modifier button, and whether holding X+Y
pauses the game (on for SteamVR, where the menu button is unreliable; off
otherwise).

Run it again any time you change `ResolutionX`, `ResolutionY` or
`GameFovDegrees` in `BioshockVR.ini`.

---

## Requirements

- BioShock Remastered on PC. **Steam and Epic are both supported and both
  tested** — Epic installs under `Build\FinalEpic` and `Setup.bat` finds it
- Any OpenXR headset. Quest and Pico work through the standard loader (Link,
  Air Link, or Virtual Desktop). **Lighthouse headsets — Index, Vive, Bigscreen
  Beyond, Varjo — work through a bundled SteamVR shim**, because SteamVR's own
  OpenXR runtime does not support 32-bit games and BioShock Remastered is
  32-bit. `Setup.bat` picks the right one for you; start SteamVR before the game
  if you choose that path.
- **Only Meta Touch controllers are tested.** Index, Vive wand and WMR/Reverb
  bindings exist and are built from each device's published layout, but I do not
  own that hardware, so they are best-effort. The log names the controller
  profile it matched and how many buttons bound — worth sending either way if you
  have one
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

**Moving and turning**
- `MovementMode` picks what steers your walking: nothing but the stick, where you
  point, where you look, or both. Aiming is a separate setting, so any way of
  walking works with either way of aiming
- Walking goes where you asked. The game applies its stick deadzone per axis,
  which bent your path by up to 11 degrees whenever the mod redirected the stick
  and snapped you to a pure sidestep near 90; that is undone before the value
  reaches the game
- Turn speed is capped so the same push always turns you at the same rate — the
  game's own response curve is nearly vertical at the top of the stick, which is
  the whole of "sometimes slow, sometimes fast"
- Snap turn, with an adjustable step

**Hands and weapons**
- 6-DOF weapon holding driven by the right controller
- The weapon holds still. The game animates the arm the gun hangs off, which made
  it drift and breathe in your hand and made a calibrated crosshair impossible;
  that idle animation is frozen while you hold it
- Two-handed grip — reach across with your free hand and take hold of the barrel
- The hand that is not holding your weapon appears and follows its own
  controller, on the weapons that hide it
- Haptics on firing and on the actions that earn them
- Aim, crosshair and the actual shot all derive from one value, so where the dot
  is is where the bullet goes
- Per-weapon grip position and angle, tunable live in the headset and saved
  automatically
- Forearms hidden at the skeleton while the hands and weapon stay visible
- The game's flat 2D reticle disabled at the engine level and replaced with a
  proper VR dot. It hides itself when your hands are empty and during scripted
  scenes, so it never floats over a cutscene

**Interface**
- The HUD is captured off the eye image and composited as its own layer, so it
  sits at a comfortable depth instead of being painted onto the world
- Adjustable size and position, tunable live
- Menus appear on a screen fixed in the room
- The map, the manual, the upgrade machine, the Gene Bank and the whole
  plasmid/tonic flow stay put in the room instead of riding your head, including
  when you click from one page of a machine screen through to the next
- Individual screens can be moved between four routes by name — `AnchorMovies`,
  `FollowMovies`, `SceneMovies` and `PanelMovies` in the ini decide whether a
  screen stays put in the room, follows your head, is left exactly where the game
  drew it, or is split with the interface flat and the world still in stereo. The
  ini lists the screen names and ships the tested set
- The quest arrow is placed in the world in front of you rather than riding your
  gun at eye level

**Cutscenes and scripted scenes**
- Pre-rendered cutscenes play on a screen anchored in the room. You can look
  around it and the camera no longer follows your head — a large comfort
  improvement in the opening sequence
- Scripted sequences land you where they intend and face you the way they
  intended, whichever way you were looking or pointing when they started. Neither
  head look nor the right stick pulls them off course
- The right stick still turns you during a scene, and `ScriptedRecentre` hands
  that back as the scene rotates so it still reaches its own framing
- Your arms and hands appear only while the scene is actually animating them

**Controls**
- Touch, Index, Vive wand and WMR controllers mapped to a virtual gamepad — see
  *Requirements* for which of those are actually tested
- Right stick click jumps; zoom is unbound because it breaks the weapon
  calibration
- D-pad modifier on the right thumbrest, with the button configurable — it lands
  where a thumb rests on trackpad headsets and the Rift CV1, and `Setup.bat` moves
  it for you when you say which headset you have
- Hold X+Y to pause, for runtimes where the menu button does not come through

---

## Tuning

Everything here changes live, and every change logs a line you can paste back
into `BioshockVR.ini`.

| Key | What it does |
|---|---|
| <kbd>F11</kbd> / <kbd>F12</kbd> | HUD panel smaller / larger |
| <kbd>Del</kbd> | cycle which HUD property F11/F12 edits |
| <kbd>Home</kbd> | toggle the HUD panel off, to compare |
| <kbd>Numpad 9</kbd> | cycle what the axis keys edit: gun position / gun angle / crosshair / off-hand position / off-hand angle / muzzle |
| <kbd>Numpad 8 2 4 6 0 5</kbd> | adjust the current mode |
| <kbd>Numpad 7</kbd> | change step size |

**Weapon grip tuning writes itself to `BioshockVR.ini` as you go.** Edit that file
with the game closed, or your changes get overwritten.

To get a weapon exactly right: set the angle first, then switch to aim mode and
fire at a flat wall, adjusting until the dot sits on the bullet hole. The dot and
the shot come from the same value, so that test is exact rather than a judgement
call.

### Weapons still to tune

Tuned: **wrench, pistol, shotgun, machine gun, crossbow, grenade launcher,
chemical thrower**, and every plasmid individually.

Not yet tuned: **the research camera**. It falls back to a generic offset and
will sit wrong in your hand. Perfectly usable — it just wants five minutes with
the numpad. If you tune it, the values land in your ini, and a pull request or a
paste in the issues is very welcome.

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

**Walking through water draws its effect as a square** rather than covering your
whole view.

**Pre-rendered cutscenes that play during the game are not anchored to the room
yet**, so they still follow your head. Scripted sequences themselves — where they
walk you, which way they turn you — are handled.

**The quest arrow drifts with the weapon** before settling back to where it
should be.

**Your arms sit slightly low and forward in some scripted scenes.**

**The desktop mirror runs at about half the headset's framerate.** That is how
the game submits frames to its own window, not a performance problem. For
recording, use SteamVR's display view instead.

**`dxgi.dll` and `winmm.dll` are shared filenames.** `Uninstall.bat` asks before
removing either, and keeps your tuned `BioshockVR.ini` as `BioshockVR.ini.bak` —
rename it back on a reinstall and every weapon stays calibrated.

**SteamVR performance can degrade after about 30 minutes** with Virtual Desktop
on Meta headsets. This is a Meta driver issue rather than something the mod can
fix; [VirtualDesktopSwitcher](https://github.com/webhead2oo9/VirtualDesktopSwitcher)
resolves it for some people.

---

## Performance

`ResolutionX` and `ResolutionY` in `BioshockVR.ini` are the main dial — lower
them and re-run `Setup.bat`. FOV below 100 looks nearly identical in the
headset but runs noticeably better, because the game stops rendering side content
that never reaches the display.

---

## Reporting a problem

**Run `logs\CollectLogs.bat`** and attach the zip it makes. It gathers every log
plus the config files, including the copies Windows can silently redirect your
settings into — which is the cause of most "I changed it and nothing happened"
reports.

If you would rather attach one file, it is **`logs\BioshockVR.log`**, in a `logs`
folder beside `BioshockHD.exe`. The config block at the top shows every setting
the mod actually read, and answers most "it's not working" reports immediately.

If the game folder is not writable, the mod falls back to
`%LOCALAPPDATA%\BioshockVR\logs\` instead and says so in the log.

Quick self-checks:

| Symptom | Cause |
|---|---|
| Nothing happens at all | Is there a `logs\BioshockVR_loader.log`? If not, `dxgi.dll` isn't loading — check all files are in the same folder. If there is, it names the reason, including which file is missing |
| Wrong resolution, or fullscreen | Run `Setup.bat` with the game closed |
| `could not find Bioshock.ini` | Launch the game once so it creates one, quit, run setup again |
| World too big or too small | `EyeSeparation` is half your IPD in cm |
| You feel too short or too tall | `CameraHeightOffset`, in cm |

---

## Building from source

- Visual Studio 2022, **Win32 / x86** — the game is 32-bit and the mod must match.
  `Release | Win32` is the only configuration that produces working output for
  any of the three
- Three projects: `BioshockVR` (the mod), `dxgiproxy` (the loader, builds as
  `dxgi.dll`), and `OpenXRShim` (the SteamVR shim, builds as `openxr_loader.dll`)
- Dependencies: OpenXR SDK, [MinHook](https://github.com/TsudaKageyu/minhook)

Build all three, then next to `BioshockHD.exe` put `BioshockVR.dll`, `dxgi.dll`,
`BioshockVR.ini`, `Setup.bat`, the shim renamed to `openxr_loader_steam.dll`, the
32-bit `openxr_loader_standard.dll`, and `openvr_api.dll` from Valve's OpenVR
SDK. Then run `Setup.bat`, which puts the loader you pick in place.

**Copying only the mod and the proxy gives you a package that cannot load** — the
mod statically imports `openxr_loader.dll`, so with no loader present Windows
refuses to load it, and the proxy reports it as a missing file.

If you get `unresolved external symbol` from `dxgi.def`, the proxy project is set
to x64. If `dumpbin /dependents dxgi.dll` lists `DXGI.dll`, you've linked
`dxgi.lib` and the proxy will try to load itself.

---

## Credits

Built on the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) and
[MinHook](https://github.com/TsudaKageyu/minhook).

Huge thanks to **Eye-will**, who playtested every new build and gave
consistently sharp, genuinely useful feedback. A lot of what got fixed this
release got fixed because they took the time to describe exactly what they saw.

Thanks also to **VOID**, and to the developer of the other BioShock VR project,
[mohamad-balouza/bioshock-vr](https://github.com/mohamad-balouza/bioshock-vr).
That project is MIT-licensed and its author has additionally given me explicit
permission to use the code however this one needs to. Several of the hardest
parts here exist because of it:

- **Disabling the game's flat 2D reticle through the engine's own console path**
  (`bReticleDisabled` via `Exec`), which is what let a proper VR crosshair
  replace it
- **The hand rig's bone map** — which bones are the wrists, which five per side
  are forearm, and which one the weapon hangs off. That is what makes it possible
  to hide the arms without losing the gun. Two independent efforts produced the
  same numbers, value for value, which is also what made them trustworthy
- **Capturing the whole interface in one render-target pass**, which is how the
  HUD gets lifted onto its own panel
- **The animation-preserving skeletal drive** — freeze the cluster's reference
  while driving it. That is the idea behind the weapon-hand freeze, and it is
  what killed weapon sway and made the crosshair calibratable at all
- **Bones 43 → 44 as the barrel axis**, reading the muzzle from the model instead
  of guessing at it with a tuned constant
- **`AWeapon::GetPerfectFireStart` at weapon vtable slot `+0x304`**, and the
  technique of substituting our own values into its out-parameters
- **`SetDrawScale` needing a revision-bump protocol**, which explained a dead end
  of ours: the offset was right all along, the bare poke was not

**This section names each system that came from there, and it gets a new line
every time another one does** — that is the deal, and it is worth more than a
licence header.
