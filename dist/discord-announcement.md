# BioShock Remastered VR — 1.0.3 is out

**The hands release.** Both of your hands are in the game now, weapons finally
hold still enough to aim down, and the scripted moments put you where they were
always meant to.

---

**Your other hand exists**
The hand that isn't holding your weapon used to just be hidden. Now it appears
and follows your controller, with a gun or a plasmid. On the two-handed weapons
you can reach across and grab the barrel, and the gun points along both hands.
Recoil doesn't shake you off it anymore.

**The gun holds still, so you can actually aim**
The game was animating the arm your weapon hangs off, so it drifted and breathed
in your hand and a calibrated crosshair was impossible. That's frozen now. Every
weapon gets its own grip, angle and crosshair, and you tune them live in the
headset on the numpad while it saves itself to your ini. The crossbow, grenade
launcher and chemical thrower are tuned for the first time.

**Scripted scenes go where they're supposed to**
Where a scene walked you used to depend on which way you happened to be looking
when it triggered. The balcony fall could drop you metres off target. That's
fixed. You can still turn with the stick during a scene and it reclaims its own
framing as it rotates, and your arms only show up while the game is genuinely
animating them.

**The interface stays in the room**
The map, the manual, the upgrade machine, the Gene Bank and the whole
plasmid/tonic flow are placed in the world instead of riding your head. That
includes clicking through to a second page, which is what broke most of them
before. The gene machine text is fixed too, and the world holds still behind a
screen while you read it.

**Walking and turning feel right**
Walking a straight line while pointing somewhere else used to bend your path by
up to 11 degrees, and collapse into a pure sidestep near 90. That's gone. Turn
speed no longer changes with how hard you push the stick. And movement is now
fully decoupled from aiming: four modes decide what steers your walking, and
your aim is a separate setting, so any combination works.

**The HUD sits at a comfortable depth**
Health, EVE and ammo are lifted off the game image onto their own panel instead
of being painted flat onto the world, with size and position tunable live. The
game's 2D reticle is disabled at the engine level and replaced with a real VR
dot that hides itself when your hands are empty or a scene is playing.

**Per-plasmid tuning**
Every plasmid shared one set of numbers, so tuning Electro Bolt moved
Telekinesis with it. They each keep their own now.

**Installing it is a lot less painful**
Setup asks which headset and runtime you have and configures the controls to
match, including moving the d-pad modifier off the thumb rest on trackpad
controllers, where it was silently stopping people from walking at all. Native
OpenXR is the default now, with the SteamVR shim as the fallback. Epic support
is properly verified. There's an uninstaller that keeps your tuning, and a
one-click log collector for bug reports. One nasty one also got caught on the
way out: the mod's loader needed a Visual C++ redistributable that plenty of
machines don't have, and when it was missing nothing loaded and no log was
written to explain why.

---

**Also in here:** snap turn, haptics, manual wrench swing, Index/Vive/WMR
bindings, head bob removed at the source, the horizon stays level, the quest
arrow no longer rides your gun at eye level, loading screens back to full
framerate, around 130 documented settings, and a full codebase refactor so the
next release lands faster.

**Run `Setup.bat` before you launch.** It picks your runtime, installs the
matching loader, and fixes the game's resolution and FOV. The game won't start
in VR until you do.

Full changelog and known issues are in the download. If something breaks, run
`logs\CollectLogs.bat` and send me the zip — it grabs everything I need.

---

Big thanks to **Eye-will**, who playtested every new build and gave me
consistently sharp, genuinely useful feedback. A lot of what got fixed this
release got fixed because they took the time to describe exactly what they saw.

And to **VOID**, dev of the other BioShock VR project
(<https://github.com/mohamad-balouza/bioshock-vr>), for letting me use his work.
The reticle removal, the hand rig's bone map, the interface capture and the
skeletal drive that killed weapon sway all trace back to his research. Saved me
weeks, and the full credit list is in the readme.
