# BioShock Remastered VR — v1.0.0

Native VR for BioShock Remastered. Real stereo rendering, head tracking, motion
controllers and 6-DOF weapon holding — not a screen in a headset. This mod was made using AI.

---

## Install

1. Download `BioshockVR-v1.0.0.zip` below.
2. Extract all three files next to `BioshockHD.exe`:
   ```
   ...\BioShock Remastered\Build\Final\
       BioshockHD.exe
       dxgi.dll
       BioshockVR.dll
       BioshockVR.ini
   ```
3. **Open `BioshockVR.ini` and set `GameIniPath`.** This is required and the mod
   will not configure the game correctly without it.

   Press <kbd>Win</kbd>+<kbd>R</kbd>, paste `%APPDATA%\BioshockHD\Bioshock`, and
   press Enter. If `Bioshock.ini` is in the folder that opens, that is your
   path — put the full path to that file in the ini.

4. Put your headset on and launch the game normally.

No injector, no plugin folder, no launcher.

---

## Requirements

- BioShock Remastered on PC (developed and tested against the Steam build)
- Any OpenXR headset and runtime — Quest via Link/Air Link, SteamVR, WMR
- Windows 10/11

---

## What's in it

**Rendering**
- True stereo, one eye per frame, with both eyes locked to the same instant so
  near geometry fuses properly
- FOV reported to the runtime is kept in sync with what the game actually
  renders, so turning doesn't warp
- Automatic guard on the game's world FOV, which otherwise gets stuck wide after
  a Vita-Chamber respawn and narrow during scripted sequences

**Head and camera**
- Full head tracking, rotation and position
- Pitch decoupled from the game's camera — the horizon stays level
- Walking head bob removed
- Adjustable height and IPD

**Hands and weapons**
- 6-DOF weapon holding driven by the right controller
- Aim, crosshair and the actual shot all come from one value, so where the dot
  is is where the bullet goes
- Per-weapon grip position and angle, tunable live in-headset and saved
  automatically
- Forearms hidden while hands and weapon stay visible
- The game's flat 2D reticle removed and replaced with a proper VR dot

**Interface**
- The HUD is lifted off the game image onto its own layer, so it's readable and
  sits at a comfortable depth instead of being painted on the world
- Adjustable size and position, tunable live
- Menus and the map appear on a screen fixed in the room, not strapped to your
  face

**Cutscenes**
- Pre-rendered cutscenes play on a screen anchored in the room. You can look
  around it, and the camera no longer follows your head — a large comfort
  improvement in the opening sequence

**Controls**
- Touch/Index controllers mapped to a virtual gamepad
- Right stick click jumps (zoom is unusable in VR and is unbound)
- D-pad modifier on the right thumbrest

---

## Tuning it

Everything below can be changed while the game runs, and everything logs a line
you can paste back into the ini.

| Key | What it does |
|---|---|
| <kbd>F11</kbd> / <kbd>F12</kbd> | HUD panel smaller / larger |
| <kbd>Del</kbd> | cycle which HUD property F11/F12 edits |
| <kbd>Home</kbd> | toggle the HUD panel off, to compare |
| <kbd>Numpad 9</kbd> | cycle weapon grip edit mode: position / angle / aim |
| <kbd>Numpad 8 2 4 6 0 5</kbd> | adjust the current mode |
| <kbd>Numpad 7</kbd> | change step size |

**Weapon grip tuning saves itself to `BioshockVR.ini` as you go.** Edit the file
with the game closed, or your changes will be overwritten.

To get a weapon exactly right: set the angle first, then switch to aim mode and
fire at a flat wall, adjusting until the dot sits on the bullet hole.

---

## Known issues and limitations

**`dxgi.dll` conflicts with other graphics mods.** ReShade, DXVK, Special K and
most injectors also install as `dxgi.dll`, and only one file can have that name.
They can't currently be used together.

**Two features are build-specific.** The reticle removal and the arm hiding use
fixed addresses into the game executable. On a different build they detect the
mismatch and safely do nothing — the log will say so. Set `DisableReticle=0` and
`HideArmSleeves=0` to silence it. Everything else works on any build.

**Weapon grip offsets are tuned for 2750x2850 at FOV 100.** They do not carry
across resolutions by any reliable formula. If you change either, expect to
re-tune — it takes a few minutes with the numpad.

**In-engine cutscenes still track your head.** Only pre-rendered ones are
detected and moved to the flat screen. The opening is covered; some later
scripted sequences are not.

**Weapon idle sway remains.** The weapon hangs off a bone of the arm mesh, so it
inherits the authored idle animation. Hiding the arms doesn't stop it.

**Cutscene letterbox bars are still there**, now on the flat screen.

**Performance.** `ResolutionX` / `ResolutionY` in the ini are the main dial. FOV
below 100 looks nearly identical in the headset but runs noticeably better.

---

## Reporting a problem

Please include `BioshockVR.log`, which appears next to `BioshockVR.dll`. The
config block at the top of it shows every setting the mod actually read, and
answers most "it's not working" questions immediately.

Common ones:

- **Nothing happens at all** — check `BioshockVR_loader.log` exists next to the
  exe. If it's missing, `dxgi.dll` isn't being loaded.
- **`gameini: could not locate Bioshock.ini`** — `GameIniPath` isn't set, step 3
  above.
- **World feels too big or too small** — `EyeSeparation` is half your IPD in cm.
- **You feel too short or too tall** — `CameraHeightOffset`, in cm.

---

## Credits

Built on the OpenXR SDK and [MinHook](https://github.com/TsudaKageyu/minhook).

Several solutions here were derived from
[mohamad-balouza/bioshock-vr](https://github.com/mohamad-balouza/bioshock-vr),
whose published reverse-engineering made the reticle removal, the arm-bone
hiding and the HUD capture approach possible. Thanks for putting that work in
the open.
