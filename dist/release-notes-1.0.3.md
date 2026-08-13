BioShock Remastered VR v1.0.3
=============================

This is the hands release. Both of your hands are in the game now, weapons hold
still enough to actually aim down, and the scripted moments put you where they
were always meant to. Underneath all of it the codebase was rebuilt, so the next
release should not take as long as this one did.

Your other hand exists now
--------------------------

The hand that is not holding your weapon used to be hidden entirely. It now
appears, follows its own controller and turns with it, whether you are holding a
gun or a plasmid.

On the two handed weapons you can also reach across and take hold of the barrel.
The grab point is not a tuned constant, it is the place the game already draws
its own hand on the fore end, so the weapon lies along both of your hands the way
it looks like it should. Recoil no longer shakes you loose from it.

The gun holds still, so it can be aimed
---------------------------------------

The game animates the arm your weapon hangs off, so the gun drifted and breathed
in your hand. That made a calibrated crosshair impossible and iron sights
pointless. That animation is now frozen while you are holding a weapon.

Every weapon carries its own grip position, angle and crosshair. All three tune
live in the headset on the numpad and write themselves back into BioshockVR.ini
as you go, so a single session can calibrate the whole arsenal. The crossbow,
grenade launcher and chemical thrower shipped untuned in earlier releases and
are tuned now.

Scripted scenes go where they are supposed to
----------------------------------------------

Where a scripted scene walked you used to depend on which way you happened to be
looking or pointing at the instant it triggered. The balcony fall could put you
several metres from where it intended, differently on every run.

Scenes now land correctly and frame themselves correctly regardless of where you
were aiming. The right stick still turns you during one, and the scene spends
that offset down as it rotates so it still reaches its own framing. Your arms and
hands appear only while the game is genuinely animating them, instead of hanging
frozen in your view for the whole scene.

The interface stays in the room
-------------------------------

The map, the manual, the upgrade machine, the Gene Bank and the whole plasmid
and tonic flow are placed in the world instead of riding your head. That includes
clicking through from the first page of a machine screen to the second, which is
what broke most of them in earlier builds. The world holds still behind a screen
while you read it, and the gene machine text now lands where it belongs.

Movement, turning and aiming
----------------------------

*   **Walking goes where you asked.** The game applies its stick deadzone per
    axis, which bent your path by up to 11 degrees whenever the mod redirected
    the stick, and collapsed you into a pure sidestep near 90. That is undone
    before the value ever reaches the game.
*   **Turn speed is consistent.** The game's own response curve is nearly
    vertical at the top of the stick, which was the whole of "sometimes slow,
    sometimes fast". The same push now turns you at the same rate.
*   **Locomotion and aiming are fully decoupled.** Four movement modes decide
    what steers your walking, and how you aim is a separate setting, so any
    combination of the two works.
*   Snap turn with an adjustable step, and haptics on firing, on the grab zone
    and on the actions that earn them.

Per plasmid tuning
------------------

Every plasmid shared weapon slot 8, so tuning Electro Bolt moved Telekinesis
with it. Each plasmid now keeps its own position, angle and crosshair, and
switching away and back restores exactly what you set.

Setup and diagnostics
---------------------

*   `Setup.bat` now asks which headset you have and acts on the answer. On
    trackpad controllers the default d pad modifier landed where a thumb rests,
    and because the modifier is held rather than pulsed, those players could not
    walk at all. Setup moves it for them.
*   Native OpenXR is now the default runtime. The SteamVR shim remains the
    answer for headsets with no 32 bit OpenXR runtime registered, and Setup
    still picks it automatically when that is the case.
*   Setup no longer prints "Done" after a failure. Every error path now says
    what went wrong and what to do about it.
*   **`Uninstall.bat`** restores your game config, keeps your tuned
    BioshockVR.ini as a .bak rather than deleting it, and asks before removing
    dxgi.dll, which ReShade and Special K also use.
*   **`logs\CollectLogs.bat`** gathers every log and config into one zip on your
    Desktop, including the copies Windows can silently redirect your settings
    into. That redirection is the cause of most "I changed it and nothing
    happened" reports.
*   The mod's loader needed a Visual C++ redistributable that plenty of machines
    do not have. When it was missing nothing loaded and no log was written to
    explain why, because the component that writes the log was the one that
    failed. It is statically linked now.

Install
-------

1.  Extract everything into the folder containing BioshockHD.exe.
    *   Steam: `steamapps\common\BioShock Remastered\Build\Final`
    *   Epic: `Epic Games\BioshockRemastered\Build\FinalEpic`
2.  Right click `Setup.bat` and choose Run as administrator.
3.  Put your headset on and launch the game through your storefront, not the
    .exe directly.

**The game will not start in VR until you run Setup.** That is deliberate. The
package ships both OpenXR loaders under their own names, and Setup is what puts
your choice onto the name the mod actually loads. It also fixes the game's
resolution and FOV, which are wrong for VR until it does.

If you change headsets or switch OpenXR runtimes later, run `Setup.bat` again so
it can pick the right loader.

Performance
-----------

The default render resolution is high. If the framerate is poor, lower
`ResolutionX` and `ResolutionY` in BioshockVR.ini, run Setup again, and relaunch.
`GameFovDegrees` below 100 looks nearly identical in the headset and runs
noticeably better, because the game stops rendering side content that never
reaches the display.

Known issues
------------

*   Pre rendered cutscenes that play during the game are not anchored to the
    room yet, so they still follow your head.
*   Walking through water draws its effect as a square rather than filling your
    view.
*   The quest arrow drifts with the weapon before settling back into place.
*   Your arms sit slightly low and forward in some scripted scenes.
*   The desktop mirror runs at about half the headset's framerate. That is how
    the game submits frames to its own window rather than a performance problem.
    For recording, use SteamVR's display view.
*   `dxgi.dll` is also how ReShade, Special K and DXVK load, and only one file
    can own that name, so they cannot be used alongside this yet.
*   SteamVR performance can degrade after about 30 minutes with Virtual Desktop
    on Meta headsets. This is a Meta driver issue rather than something the mod
    can fix. [VirtualDesktopSwitcher](https://github.com/webhead2oo9/VirtualDesktopSwitcher)
    resolves it for some people.
*   Only Meta Touch controllers are tested. Index, Vive and WMR bindings exist
    and are built from each device's published layout, but I do not own that
    hardware. The log names which controller profile it matched and how many
    buttons bound, so a report is useful either way.

Reporting problems
------------------

Run `logs\CollectLogs.bat` and send the zip it puts on your Desktop. It asks two
questions and gathers everything needed, including the redirected copies
mentioned above.

If you would rather send one file it is `logs\BioshockVR.log`. The block at the
top lists every setting the mod actually read, which answers most reports
immediately. If the game folder is not writable that log goes to
`%LOCALAPPDATA%\BioshockVR\logs\` instead, and says so.

Credits
-------

*   **Eye-will** for playtesting every new build and for consistently sharp,
    genuinely useful feedback. A lot of what got fixed in this release got fixed
    because they took the time to describe exactly what they saw.
*   **GingasVR** for the OpenXR to OpenVR shim, the Lighthouse hardware testing,
    and the writeup that came with it.
*   **VOID**, developer of [mohamad-balouza/bioshock-vr](https://github.com/mohamad-balouza/bioshock-vr),
    for his massive contributions to this project. His work is MIT licensed and
    public, and he gave me explicit permission to use it however this mod needed.
    Several of the hardest parts here exist because of that:

    *   Disabling the game's 2D reticle through the engine's own console path,
        which is what let a proper VR crosshair replace it
    *   The hand rig's bone map: which bones are the wrists, which five per side
        are forearm, and which one the weapon hangs off. That is what makes it
        possible to hide the arms without losing the gun
    *   Capturing the whole interface in one render target pass, which is how
        the HUD gets lifted onto its own panel
    *   The animation preserving skeletal drive, which is the idea behind
        freezing the weapon hand. That is what killed weapon sway and made the
        crosshair calibratable at all
    *   Reading the barrel direction from the model's own bones instead of
        guessing at it with a tuned constant
    *   Locating the engine function that decides where a shot starts and which
        way it goes, and the technique for substituting our own values into it

    His research is public, and it saved me weeks.
*   Everyone who sent logs.
