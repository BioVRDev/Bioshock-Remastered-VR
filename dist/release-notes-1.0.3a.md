BioShock Remastered VR v1.0.3a
==============================

A hotfix for 1.0.3. **If Setup.bat closed instantly when you ran it, this is the
release you need.** The mod itself is unchanged: same DLL, same settings, same
tuning. Everything here is the installer and what it configures for you.

Setup no longer closes on itself
--------------------------------

On a machine whose only OpenXR runtime is SteamVR, `Setup.bat` died with
`set was unexpected at this time` and the window shut before it ever asked which
runtime you wanted. That is Index, Vive and PSVR2, which is precisely the group
that needs the bundled SteamVR shim and could not reach the prompt that installs
it.

The cause was a line that checked whether a value existed before using it. The
check was real but it ran too late: the command interpreter resolves everything
on a line before it runs any of it, so the value was already being read when the
guard was still waiting its turn. Reproduced here by forcing the same empty
registry state, and fixed the same way.

If you hit this and worked around it by renaming the loader by hand, you did
exactly what Setup would have done. Nothing to undo.

Your controls, spelled out at the end of Setup
----------------------------------------------

Two of the mod's controls cannot be guessed, and until now nothing told you what
they were. Setup now finishes by printing them for the setup you just chose:

```
    Pause            the Menu button on the LEFT controller

    Modifier         right thumbrest (rest your thumb on it)
                     Hold it and the left stick stops moving you and
                     becomes a D-pad for the HUD.

    Map              hold the Modifier, then Pause, for half a second
    Alt Menu Button  hold the Modifier and tap Pause
```

The pause control depends on which runtime you picked and the modifier depends
on which headset you picked, so the combination could not be worked out from
either answer alone. It is written to `logs\setup.log` too, so a bug report
carries the exact layout the reporter was using.

Controls configured per headset
-------------------------------

Read out of the mod's own binding tables and checked against each device's
OpenXR profile:

*   **Vive wands and WMR were being given a pause chord they cannot press.**
    Neither device binds the two face buttons the chord needs, so it was inert,
    and the summary above would have told those players to press buttons that do
    not exist on their controllers. Pause stays on the menu button for both.
*   **Vive wands and WMR had no map at all.** The default d-pad modifier sits on
    the thumbrest, which neither profile binds, so the modifier never engaged and
    the map and alt menu button were unreachable. Both now get the modifier on
    the right stick click.
*   **WMR had no jump button.** Jump lives on a face button that the WMR profile
    does not bind. Setup now writes a different button layout for WMR only, which
    gives you jump, hack and use, and moves the med hypo to the radial where it
    is reachable anyway.
*   **Vive wands are told the truth.** That controller binds one face action,
    Use. Med hypo, hack/reload and jump have nowhere to go and no setting
    recovers them. Setup says so rather than configuring around a gap it cannot
    close. If your headset came with Index controllers, choose Index.
*   **Index, Beyond, Varjo and Somnium** get a note about the analog grip reading
    as held by a resting hand, and the one-line fix if it happens to you.
*   **Pico** gets a note explaining that the mod ships no Pico bindings and
    relies on the runtime reporting Touch compatibility, with the log line that
    names what actually matched.

The buttons are named the way your controller names them
--------------------------------------------------------

The summary calls each control what it is on the hardware you said you have. Two
of those are not what anyone would assume: Touch has exactly one menu button and
it is on the **left** controller, and **the Index has no menu button at all** in
OpenXR, so the mod binds a firm press on the left trackpad. An Index player
looking for a menu button would never have found one.

PSVR2 is on the headset list now, and its pause chord is named as Triangle and
Square, which is where those buttons are on a Sense controller.

Also in this release
--------------------

*   `Uninstall.bat` keeps your tuned `BioshockVR.ini` as a `.bak` instead of
    deleting it, and asks before removing `dxgi.dll`, which ReShade and Special K
    also use.
*   `CollectLogs.bat` cleans up the copies it gathers, so a second report cannot
    carry a previous run's evidence with a fresh timestamp on it.
*   Both scripts stopped using a Windows tool Microsoft is removing. Where it was
    already gone, the machine details in a bug report came through blank and
    every log bundle was named the same thing.
*   Mouse and keyboard is documented in the ini and both readmes. It is rough and
    labelled as such, and a gamepad is rougher; neither is offered as a setup
    choice yet.

Install
-------

Same as 1.0.3. Extract everything into the folder containing BioshockHD.exe,
right click `Setup.bat` and choose Run as administrator, then launch through your
storefront.

If you already have 1.0.3 working, you can extract this over it and re-run
`Setup.bat`. Your `BioshockVR.ini` is not overwritten by extracting, so any
tuning you have done is kept.
