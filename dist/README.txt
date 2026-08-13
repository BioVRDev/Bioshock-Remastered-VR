================================================================================
 BioShock Remastered VR  |  1.0.3
================================================================================

Native VR for BioShock Remastered. Real stereo rendering, head tracking, motion
controllers and 6-DOF weapon holding.

No injector, no plugin folder, no launcher. See changelog.txt for what is new.


--------------------------------------------------------------------------------
 INSTALL
--------------------------------------------------------------------------------

1. Close the game.

2. Extract every file in this zip next to BioshockHD.exe:

     Steam   ...\BioShock Remastered\Build\Final\
     Epic    ...\Epic Games\BioshockRemastered\Build\FinalEpic\

   You should end up with:

     BioshockHD.exe                the game
     dxgi.dll                      loads the mod
     BioshockVR.dll                the mod
     BioshockVR.ini                settings
     openxr_loader_standard.dll    the normal OpenXR loader
     openxr_loader_steam.dll       the SteamVR one
     openvr_api.dll                needed by the SteamVR one
     Setup.bat
     Uninstall.bat
     README.txt, changelog.txt
     logs\CollectLogs.bat

3. Run Setup.bat, with the game still closed. It asks which headset you have
   and which runtime to use, then installs the matching loader.

4. Put your headset on and launch the game normally.

THE GAME WILL NOT START IN VR UNTIL YOU RUN SETUP. That is deliberate. The mod
loads a file called openxr_loader.dll, and Setup is what puts your choice of the
two loaders above onto that name. It also fixes the game's resolution and FOV,
which are wrong for VR until it does.

If it reports that it cannot write, close the game, right-click Setup.bat and
choose "Run as administrator".


--------------------------------------------------------------------------------
 WHAT SETUP DOES
--------------------------------------------------------------------------------

The game reads its config at startup and rewrites it on exit with whatever it
actually ran at, which is why a fresh install starts fullscreen at your desktop
resolution and can never correct itself. Setup writes the values before the game
starts, which breaks that loop for good:

  * render resolution and FOV
  * windowed mode instead of exclusive fullscreen
  * anisotropic filtering to x16

It finds Bioshock.ini on its own, backs it up first, and changes those values in
place using the same Windows call the game itself uses. It does not rewrite the
file. If anything looks wrong afterwards it restores the backup automatically.

It also writes two control settings from your answers: which button is the d-pad
modifier, and whether holding X+Y pauses the game.

Run it again any time you change ResolutionX, ResolutionY or GameFovDegrees in
BioshockVR.ini.

Everything Setup did is recorded in logs\setup.log.


--------------------------------------------------------------------------------
 REQUIREMENTS
--------------------------------------------------------------------------------

  * BioShock Remastered on PC. Steam and Epic are both supported and tested
  * Windows 10 or 11
  * Any OpenXR headset

    Quest, Pico and Rift work through the normal loader (Link, Air Link or
    Virtual Desktop).

    Index, Vive, Bigscreen Beyond and Varjo work through the bundled SteamVR
    shim, because SteamVR's own OpenXR runtime does not support 32-bit games and
    BioShock Remastered is 32-bit. Setup picks the right one for you. If you use
    that path, start SteamVR before launching the game.

  * Only Meta Touch controllers are tested. Index, Vive wand and WMR or Reverb
    bindings exist and are built from each device's published layout, but I do
    not own that hardware. The log names which controller profile it matched and
    how many buttons bound, so it is worth sending either way if you have one.


--------------------------------------------------------------------------------
 TUNING IN THE HEADSET
--------------------------------------------------------------------------------

Everything below changes live, and every change writes itself into
BioshockVR.ini as you go. Edit that file with the game CLOSED, or your changes
get overwritten.

  F11 / F12        HUD panel smaller / larger
  Del              cycle which HUD property F11/F12 edits
  Home             toggle the HUD panel off, to compare

  Numpad 9         cycle what the axis keys edit:
                     gun position, gun angle, crosshair,
                     off-hand position, off-hand angle, muzzle
  Numpad 8 2       forward / back      (or pitch)
  Numpad 6 4       right / left        (or yaw)
  Numpad 0 5       up / down           (or roll)
  Numpad 7         change step size

To get a weapon exactly right: set the angle first, then switch to crosshair
mode, fire at a flat wall, and adjust until the dot sits on the bullet hole. The
dot and the shot come from the same value, so that test is exact rather than a
judgement call.

Tuned already: wrench, pistol, shotgun, machine gun, crossbow, grenade launcher,
chemical thrower, and every plasmid. The research camera is not, so it falls back
to a generic offset and will sit wrong in your hand. Five minutes with the numpad
fixes it, and if you do, the values land in your ini and are very welcome in the
issues.


--------------------------------------------------------------------------------
 PERFORMANCE
--------------------------------------------------------------------------------

ResolutionX and ResolutionY in BioshockVR.ini are the main dial. Lower them and
re-run Setup.bat.

GameFovDegrees below 100 looks nearly identical in the headset but runs
noticeably better, because the game stops rendering side content that never
reaches the display.


--------------------------------------------------------------------------------
 IF SOMETHING GOES WRONG
--------------------------------------------------------------------------------

RUN logs\CollectLogs.bat AND SEND THE ZIP IT PUTS ON YOUR DESKTOP. It asks two
questions, then gathers every log and config, including the copies Windows can
silently redirect your settings into, which is the cause of most "I changed it
and nothing happened" reports.

If you would rather send one file, it is logs\BioshockVR.log. The block at the
top lists every setting the mod actually read, which answers most reports
immediately. If the game folder is not writable, that log goes to
%LOCALAPPDATA%\BioshockVR\logs\ instead and says so.

  Nothing happens at all
      Look for logs\BioshockVR_loader.log. If it is not there, dxgi.dll is not
      loading, so check every file went into the same folder as BioshockHD.exe.
      If it is there, it names the reason.

  Wrong resolution, or it starts fullscreen
      Run Setup.bat with the game closed.

  "could not find Bioshock.ini"
      Launch the game once so it creates one, quit, then run Setup again.

  Flat 2D image, no VR
      Setup was not run, or the runtime choice was wrong. Run it again and pick
      the other option. One of them will be right.

  World feels too big or too small
      EyeSeparation is half your IPD, in cm.

  You feel too short or too tall
      CameraHeightOffset, in cm.


--------------------------------------------------------------------------------
 UNINSTALL
--------------------------------------------------------------------------------

Run Uninstall.bat from the game folder. It restores your original Bioshock.ini
from the backup Setup made, removes the mod's files, and deletes itself.

Your tuning is kept. BioshockVR.ini is renamed to BioshockVR.ini.bak rather than
deleted, so a reinstall can restore every weapon you calibrated. It also asks
before removing dxgi.dll, because ReShade, Special K and DXVK use that same
filename.

Save games and original game files are never touched.


--------------------------------------------------------------------------------
 KNOWN ISSUES
--------------------------------------------------------------------------------

  * Pre-rendered cutscenes that play during the game are not anchored to the
    room yet, so they still follow your head
  * Walking through water draws its effect as a square rather than filling your
    view
  * The quest arrow drifts with the weapon before settling back
  * Your arms sit slightly low and forward in some scripted scenes
  * The desktop mirror runs at about half the headset's framerate. That is how
    the game submits frames to its own window, not a performance problem
  * dxgi.dll is also how ReShade, Special K and DXVK load, so they cannot be
    used alongside this yet


--------------------------------------------------------------------------------
 RECOMMENDED ALONGSIDE
--------------------------------------------------------------------------------

  HD Textures                 https://www.nexusmods.com/bioshock/mods/54
  Cutscene Black Bar Removal  https://www.nexusmods.com/bioshock/mods/81


--------------------------------------------------------------------------------
 CREDITS
--------------------------------------------------------------------------------

Built on the Khronos OpenXR SDK and MinHook.

A huge thank you to Eye-will, who playtested every new build and gave me
consistently sharp, genuinely useful feedback. A lot of what got fixed this
release got fixed because they took the time to describe exactly what they saw.

And to VOID, developer of github.com/mohamad-balouza/bioshock-vr, for his
massive contributions to this project. His work is MIT licensed and he gave me
explicit permission to use it however this mod needed. Several of the hardest
parts here exist because of that:

  * Disabling the game's flat 2D reticle through the engine's own console path,
    which is what let a proper VR crosshair replace it
  * The hand rig's bone map: which bones are the wrists, which five per side are
    forearm, and which one the weapon hangs off. That is what makes it possible
    to hide the arms without losing the gun
  * Capturing the whole interface in one render target pass, which is how the
    HUD gets lifted onto its own panel
  * The animation-preserving skeletal drive, which is the idea behind freezing
    the weapon hand. That is what killed weapon sway and made aiming calibratable
  * Reading the barrel direction from the model's own bones rather than guessing
    at it with a tuned constant
  * Locating the engine function that decides where a shot starts and which way
    it goes, and the technique for substituting our own values into it

His research is public, and it saved me weeks.
