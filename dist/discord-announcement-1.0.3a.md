**1.0.3a is up**

Quick hotfix. If you ran Setup.bat and the window just closed on you, that is fixed. It was hitting everyone whose only runtime is SteamVR, so mostly Index, Vive and PSVR2, which is exactly the group that needs the SteamVR shim the installer puts in place. Sorry to anyone who lost time on it.

The mod itself has not changed. Same DLL, same settings, your tuning is untouched. If 1.0.3 installed fine for you this one is optional, though the control fixes below are worth having.

**What else is in it**

Setup now prints your actual controls when it finishes, based on the headset and runtime you picked. Pause, the modifier, the map, all named for the buttons your controller really has. It goes into setup.log as well, so it shows up in bug reports.

Per headset fixes, all from reading what the mod actually binds:

- Vive wands and WMR were being handed a pause chord their controllers cannot press, and the default modifier sits on a thumbrest neither one has, so the map was unreachable. Both fixed.
- WMR had no jump button at all. It gets a different button layout now.
- Vive wands only have one usable face button, so Setup tells you that up front instead of pretending otherwise.
- PSVR2 is on the headset list.

Uninstall keeps your tuned ini instead of deleting it now, and asks before it touches dxgi.dll, since ReShade and Special K use that same filename.

Mouse and keyboard is documented in the ini if you want to mess with it, but it is rough and a gamepad is rougher. Not a supported mode yet.

Download: <https://github.com/BioVRDev/Bioshock-Remastered-VR/releases/tag/v1.0.3a>

Full notes are on the release page. Shout if anything is still broken.
