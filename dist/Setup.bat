@echo off
rem ===========================================================================
rem  BioShock Remastered VR -- Setup
rem
rem  Run with the game CLOSED. If it reports a write failure, close the game,
rem  right-click this file and choose "Run as administrator", and try again.
rem
rem  WHAT IT DOES
rem    1. Finds BioshockVR.ini and works out which store this install is.
rem    2. Finds Bioshock.ini and writes resolution / FOV / anisotropy into it,
rem       backing it up first and restoring the backup if anything looks wrong.
rem    3. Records where Bioshock.ini lives back into BioshockVR.ini.
rem    4. Asks which headset you have (recorded only).
rem    5. Asks which runtime to use and installs the matching loader.
rem
rem  Everything is written to logs\setup.log. If you report a problem, that
rem  file tells us your whole configuration without anyone having to ask.
rem
rem  This does NOT rewrite Bioshock.ini wholesale. It calls
rem  WritePrivateProfileString, the same Windows API the game itself uses,
rem  which updates one key in place and leaves every other line untouched.
rem ===========================================================================

setlocal enabledelayedexpansion

set "BUILD=2026-08-13-d"
set "LDR_LIVE=openxr_loader.dll"
set "LDR_SHIM=openxr_loader_steam.dll"
set "LDR_STD=openxr_loader_standard.dll"
set "LOGDIR=%~dp0logs"
set "SETUPLOG=%LOGDIR%\setup.log"
if not exist "%LOGDIR%" mkdir "%LOGDIR%" >nul 2>&1

call :log ""
call :log "==========================================================="
call :log "SETUP RUN  %DATE% %TIME%  build %BUILD%"
call :log "==========================================================="

echo.
echo   BioShock Remastered VR -- Setup
echo   -----------------------------------------
echo   Build: %BUILD%
echo.

rem ---- machine, for the log only ---------------------------------------------
rem  NO wmic. It is a Feature-on-Demand that Microsoft is removing from Windows
rem  11, and on a machine without it every line here logged BLANK -- which reads
rem  in a bug report as "the reporter has no GPU" rather than "the tool is gone".
rem  Get-CimInstance is the supported replacement, is present on every Windows 10
rem  and 11, and is the same call the performance block in CollectLogs already
rem  uses. One idiom for machine facts, in both scripts.
rem
rem  Driver version is in here because it was not before, and a GPU driver is the
rem  single most common cause of "black in the headset, perfect in the log".
call :log "--- machine ---"
for /f "usebackq delims=" %%A in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $o=Get-CimInstance Win32_OperatingSystem; $c=Get-CimInstance Win32_Processor; 'os : '+$o.Caption+' build '+$o.BuildNumber; 'cpu: '+($c.Name -join ', '); 'ram: '+[math]::Round($o.TotalVisibleMemorySize/1MB,1)+' GB'; foreach($g in (Get-CimInstance Win32_VideoController)){'gpu: '+$g.Name+'  driver '+$g.DriverVersion} } catch { 'machine details unavailable' }" 2^>nul`) do call :log "%%A"

rem ---- find BioshockVR.ini ---------------------------------------------------
set "MODINI=%~dp0BioshockVR.ini"
rem  BOTH Epic folder names are probed. Our own files disagreed about whether
rem  Epic ships Build\FinalEpic or Build\Final, and nobody here owns an Epic
rem  install to settle it -- so stop guessing and look for both. A name that
rem  does not exist simply fails its `if exist` and costs nothing.
if not exist "%MODINI%" if exist "C:\Program Files\Epic Games\BioshockRemastered\Build\FinalEpic\BioshockVR.ini" set "MODINI=C:\Program Files\Epic Games\BioshockRemastered\Build\FinalEpic\BioshockVR.ini"
if not exist "%MODINI%" if exist "C:\Program Files\Epic Games\BioshockRemastered\Build\Final\BioshockVR.ini" set "MODINI=C:\Program Files\Epic Games\BioshockRemastered\Build\Final\BioshockVR.ini"
if not exist "%MODINI%" if exist "C:\Program Files (x86)\Epic Games\BioshockRemastered\Build\FinalEpic\BioshockVR.ini" set "MODINI=C:\Program Files (x86)\Epic Games\BioshockRemastered\Build\FinalEpic\BioshockVR.ini"
if not exist "%MODINI%" if exist "C:\Program Files (x86)\Epic Games\BioshockRemastered\Build\Final\BioshockVR.ini" set "MODINI=C:\Program Files (x86)\Epic Games\BioshockRemastered\Build\Final\BioshockVR.ini"
if not exist "%MODINI%" if exist "C:\Program Files (x86)\Steam\steamapps\common\BioShock Remastered\Build\Final\BioshockVR.ini" set "MODINI=C:\Program Files (x86)\Steam\steamapps\common\BioShock Remastered\Build\Final\BioshockVR.ini"
if exist "%MODINI%" goto :gotmodini

echo   BioshockVR.ini was not found next to this file, or in the usual Steam
echo   and Epic install folders.
echo.
echo   Paste the folder that contains BioshockHD.exe.
echo.
echo   HOW TO COPY A FOLDER PATH:
echo     1. Open that folder in File Explorer.
echo     2. Click the address bar at the top. The path turns into blue text.
echo     3. Press Ctrl+C.
echo     4. Click back on THIS window, press Ctrl+V, then Enter.
echo.
echo   It usually looks like one of these:
echo     C:\Program Files ^(x86^)\Steam\steamapps\common\BioShock Remastered\Build\Final
echo     C:\Program Files\Epic Games\BioshockRemastered\Build\FinalEpic
echo     C:\Program Files\Epic Games\BioshockRemastered\Build\Final
echo.

:askmodini
set "USERDIR="
set /p "USERDIR=  Folder (or press Enter to give up): "
if not defined USERDIR goto :end
set "USERDIR=%USERDIR:"=%"
if "%USERDIR:~-1%"=="\" set "USERDIR=%USERDIR:~0,-1%"
if exist "%USERDIR%\BioshockVR.ini" (
    set "MODINI=%USERDIR%\BioshockVR.ini"
    goto :gotmodini
)
echo.
echo   No BioshockVR.ini in that folder. Check you copied the folder with
echo   BioshockHD.exe in it, not the game's top-level folder.
echo.
goto :askmodini

:gotmodini
for %%F in ("%MODINI%") do set "GAMEDIR=%%~dpF"
echo   Mod config:  %MODINI%
call :log "--- paths ---"
call :log "mod ini : %MODINI%"
call :log "game dir: %GAMEDIR%"
call :cleanupbrokenlogfiles

rem ---- can we write to the game folder? --------------------------------------
set "WRITEOK=1"
break > "%GAMEDIR%vrwritetest.tmp" 2>nul
if not exist "%GAMEDIR%vrwritetest.tmp" set "WRITEOK=0"
del "%GAMEDIR%vrwritetest.tmp" >nul 2>&1
call :log "game dir writable: %WRITEOK%"
if "%WRITEOK%"=="0" (
    echo.
    echo   WARNING: this folder is not writable without admin rights.
    echo            Logs and saved settings will not land here.
    echo            Close this, right-click Setup.bat, pick
    echo            "Run as administrator", and run it again.
    echo.
)

rem ---- values we want, read from BioshockVR.ini ------------------------------
set "RESX=2750"
set "RESY=2850"
set "FOV=100"
set "ANISO=16"
set "GIP="
rem  DPADCUR and DPADFLIP are read only so the controls summary at the end can
rem  state what is actually configured rather than what Setup happens to write.
rem  Setup writes the modifier for some headsets and leaves it alone for the
rem  rest, so without reading it the summary would be guessing for most users.
set "DPADCUR=1"
set "DPADFLIP=0"
for /f "usebackq eol=; tokens=1,* delims==" %%A in ("%MODINI%") do (
    if /i "%%A"=="ResolutionX"    set "RESX=%%B"
    if /i "%%A"=="ResolutionY"    set "RESY=%%B"
    if /i "%%A"=="GameFovDegrees" set "FOV=%%B"
    if /i "%%A"=="Anisotropy"     set "ANISO=%%B"
    if /i "%%A"=="GameIniPath"    set "GIP=%%B"
    if /i "%%A"=="ControllerDpadModifier" set "DPADCUR=%%B"
    if /i "%%A"=="ControllerDpadFlip"     set "DPADFLIP=%%B"
)
for /f "tokens=1 delims=." %%X in ("%FOV%") do set "FOV=%%X"

echo   Wanted:      %RESX% x %RESY%, FOV %FOV%, anisotropy x%ANISO%
call :log "wanted: %RESX%x%RESY% fov %FOV% aniso %ANISO%"

rem ---- which store is THIS install? ------------------------------------------
rem  Anyone who owns both stores, or who copied BioshockVR.ini between them,
rem  ends up with a GameIniPath pointing at the wrong store's config.
rem
rem  DETECTED FROM "Epic Games", NOT FROM THE FOLDER NAME. Our own files
rem  disagreed about Epic's folder: Setup said Build\FinalEpic, the ini and the
rem  README said Build\Final. Whichever is right, an Epic install lives under an
rem  "Epic Games" parent folder and a Steam one does not -- so ask the question
rem  that has an answer we can actually check. The old FinalEpic test is kept as
rem  a second signal, because it costs one line and cannot make things worse.
set "STORE=STEAM"
if not "%GAMEDIR:Epic Games=%"=="%GAMEDIR%" set "STORE=EPIC"
if not "%GAMEDIR:FinalEpic=%"=="%GAMEDIR%" set "STORE=EPIC"
echo   Store:       %STORE%   (from the folder name)
call :log "store: %STORE%"

rem ---- find EVERY Bioshock.ini on this machine -------------------------------
rem  Someone can own the game on both stores, and the two keep SEPARATE configs
rem  in separate profile folders. Writing only the one matching this install's
rem  folder name left the other store untouched -- so launching the other copy
rem  gave stock resolution and FOV with the mod still loaded, which reads to the
rem  user as "the mod broke my game".
rem
rem  So: find them ALL, write to ALL of them. Writing settings into a config for
rem  a store you do not have installed is impossible, because the file only
rem  exists if that game has been launched at least once.
rem
rem  PRIMARY is the one matching THIS install, and is what gets recorded as
rem  GameIniPath for the mod to read at runtime.
set "NINI=0"
set "PRIMARY="

call :addini "%APPDATA%\My Games\Bioshock Epic HD\Bioshock\Bioshock.ini"        EPIC
call :addini "%APPDATA%\Bioshock Epic HD\Bioshock\Bioshock.ini"                 EPIC
call :addini "%USERPROFILE%\Documents\My Games\Bioshock Epic HD\Bioshock\Bioshock.ini" EPIC
call :addini "%APPDATA%\My Games\BioshockHD\Bioshock\Bioshock.ini"              STEAM
call :addini "%APPDATA%\BioshockHD\Bioshock\Bioshock.ini"                       STEAM
call :addini "%USERPROFILE%\Documents\My Games\BioshockHD\Bioshock\Bioshock.ini" STEAM
rem  DELIBERATELY NOT SEARCHED:
rem    %APPDATA%\Bioshock\            the ORIGINAL 2007 BioShock
rem    %APPDATA%\My Games\Bioshock\   same game, other layout
rem  Those belong to a different game that this mod does not touch. Writing our
rem  resolution and FOV into them would break someone's classic BioShock while
rem  doing nothing at all for Remastered. Remastered only ever uses BioshockHD
rem  (Steam) or Bioshock Epic HD (Epic).

if defined GIP if exist "%GIP%" call :addini "%GIP%" ANY

if "%NINI%"=="0" goto :nogameini

echo   Found %NINI% game config file(s):
call :log "--- game configs ---"
for /l %%I in (1,1,%NINI%) do (
    call :say "               !INI%%I!"
    call :log "found: !INI%%I!"
)
if not defined PRIMARY (
    rem  Nothing matched this install's store, so the first hit is as good a
    rem  guess as any for GameIniPath -- but every file still gets written.
    set "PRIMARY=!INI1!"
    call :log "WARNING: no config matched store %STORE%, using the first as primary"
)
call :say "  Primary:     !PRIMARY!"
call :log "primary: !PRIMARY!"
goto :gotgameini

:nogameini
echo.
echo   Could not find Bioshock.ini. Looked under AppData\Roaming and Documents,
echo   with and without a "My Games" folder, for both stores.
echo.
echo   If you have never launched the game, do that once, quit, and run this
echo   again -- the game creates the file on first launch.
echo.
echo   Otherwise paste either the FOLDER containing Bioshock.ini, or the full
echo   path to Bioshock.ini itself.
echo.
echo   HOW TO FIND IT:
echo     1. Press Windows+R, type   %%APPDATA%%   and press Enter.
echo     2. Look for Bioshock Epic HD, BioshockHD, or Bioshock.
echo        Bioshock.ini is usually in a Bioshock subfolder of that.
echo     3. Click the address bar, Ctrl+C, then Ctrl+V into this window.
echo.
call :log "ERROR: no Bioshock.ini found automatically"

:askgameini
set "USERINI="
set /p "USERINI=  Path (or press Enter to give up): "
if not defined USERINI goto :end
set "USERINI=%USERINI:"=%"
if "%USERINI:~-1%"=="\" set "USERINI=%USERINI:~0,-1%"
if exist "%USERINI%\Bioshock.ini" (
    call :addini "%USERINI%\Bioshock.ini" ANY
    set "PRIMARY=%USERINI%\Bioshock.ini"
    goto :gotgameini
)
if exist "%USERINI%" (
    call :addini "%USERINI%" ANY
    set "PRIMARY=%USERINI%"
    goto :gotgameini
)
echo.
echo   Nothing found at that path.
echo.
goto :askgameini

:gotgameini

rem ---- back up and write EVERY config we found -------------------------------
for /l %%I in (1,1,%NINI%) do call :writeone "!INI%%I!"

rem ---- hand the answer to the mod --------------------------------------------
echo.
echo   Recording the config location in BioshockVR.ini...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d='[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string a, string b, string c, string d);'; Add-Type -MemberDefinition $d -Name Ini -Namespace W32 | Out-Null; [void][W32.Ini]::WritePrivateProfileString('VR','GameIniPath','!PRIMARY!','%MODINI%'); [void][W32.Ini]::WritePrivateProfileString($null,$null,$null,'%MODINI%')"

set "GIPCHECK="
for /f "usebackq eol=; tokens=1,* delims==" %%A in ("%MODINI%") do (
    if /i "%%A"=="GameIniPath" set "GIPCHECK=%%B"
)
if not defined GIPCHECK (
    echo   WARNING: could not save GameIniPath into BioshockVR.ini.
    echo            Re-run as administrator. Everything above still applied.
    call :log "WARNING: GameIniPath writeback failed"
) else (
    echo   Saved:       GameIniPath=!GIPCHECK!
    call :log "GameIniPath written: !GIPCHECK!"
)

rem ===========================================================================
rem  HEADSET -- recorded, AND now acted on for one setting.
rem
rem  We cannot support every headset, but we can know which ones people have
rem  and which ones keep turning up in bug reports. Kept separate from the
rem  runtime question so a headset nobody has heard of still records itself
rem  and still gets a working runtime.
rem
rem  THIS USED TO BE PURELY DECORATIVE, and one default made that expensive.
rem  ControllerDpadModifier=1 means "right thumbrest", which on a TRACKPAD
rem  headset (Index, Beyond, Varjo, Somnium) resolves to trackpad TOUCH -- and
rem  the modifier is HELD, not pulsed. A thumb resting where the hardware
rem  expects it to rest therefore suppresses all movement: the player cannot
rem  walk, at default settings, with nothing in the log explaining why.
rem
rem  The Rift CV1 has the same problem for the opposite reason -- no thumbrest
rem  sensor at all, so the modifier can never fire and the d-pad is unreachable.
rem
rem  Quest 2 and later DO have a thumbrest and keep the default.
rem
rem  Writing an ini key is the safest possible place to fix a hardware defect:
rem  it changes no code, it is visible in the startup echo, and the user can
rem  override it afterwards.
rem ===========================================================================
echo.
echo   -----------------------------------------
echo    Which headset do you have?
echo   -----------------------------------------
echo.
echo     1  Meta Quest 3 / 3S          10  Pimax Crystal / Light
echo     2  Meta Quest Pro             11  Pimax 5K / 8K
echo     3  Meta Quest 2               12  Reverb G2 / other WMR
echo     4  Meta Quest 1               13  Varjo Aero / XR-3
echo     5  Meta Rift S / Rift CV1     14  Pico 4 / 4 Ultra
echo     6  Valve Index                15  Somnium VR1
echo     7  HTC Vive / Vive Pro        16  PSVR2
echo     8  Vive Pro 2 / XR Elite      17  Something else
echo     9  Bigscreen Beyond 1 / 2
echo.
set "HMD="
set /p "HMD=  Headset number: "

set "HMDNAME=unspecified"
if "%HMD%"=="1"  set "HMDNAME=Meta Quest 3 / 3S"
if "%HMD%"=="2"  set "HMDNAME=Meta Quest Pro"
if "%HMD%"=="3"  set "HMDNAME=Meta Quest 2"
if "%HMD%"=="4"  set "HMDNAME=Meta Quest 1"
if "%HMD%"=="5"  set "HMDNAME=Meta Rift S / CV1"
if "%HMD%"=="6"  set "HMDNAME=Valve Index"
if "%HMD%"=="7"  set "HMDNAME=HTC Vive / Vive Pro"
if "%HMD%"=="8"  set "HMDNAME=Vive Pro 2 / XR Elite"
if "%HMD%"=="9"  set "HMDNAME=Bigscreen Beyond"
if "%HMD%"=="10" set "HMDNAME=Pimax Crystal"
if "%HMD%"=="11" set "HMDNAME=Pimax 5K / 8K"
if "%HMD%"=="12" set "HMDNAME=Reverb G2 / WMR"
if "%HMD%"=="13" set "HMDNAME=Varjo"
if "%HMD%"=="14" set "HMDNAME=Pico 4"
if "%HMD%"=="15" set "HMDNAME=Somnium VR1"
if "%HMD%"=="16" set "HMDNAME=PSVR2"
if "%HMD%"=="17" (
    set "HMDNAME="
    set /p "HMDNAME=  Type the headset name: "
    if not defined HMDNAME set "HMDNAME=unspecified (other)"
)

rem ---- the one setting the answer decides ----------------------------------
rem  6 Index, 9 Beyond, 13 Varjo, 15 Somnium -- trackpad controllers, where the
rem  default modifier lands on the thumb's resting place and stops you walking.
rem  5 Rift S / CV1 -- the CV1 has no thumbrest sensor, so mode 1 never fires.
rem  16 PSVR2 -- REPORTED, not measured: a PSVR2 user reached the map only with
rem  mode 2, which reads as mode 1's thumbrest touch not binding through the
rem  shim. One report, so it is a better default than the current one rather
rem  than a proven fact; the ini comment says how to change it back.
rem
rem  7, 8 Vive and 12 WMR -- READ OUT OF THE MOD'S OWN BINDING TABLES, not
rem  guessed. Neither profile binds rest_l or rest_r at all (WMR leaves them out
rem  deliberately, to keep the modifier off the trackpad the face buttons live
rem  on), so the DEFAULT modifier is inert on both and those users have no map
rem  and no context help whatsoever. Mode 2 is the only one their hardware can
rem  reach.
set "DPADFIX="
if "%HMD%"=="5"  set "DPADFIX=2"
if "%HMD%"=="6"  set "DPADFIX=2"
if "%HMD%"=="7"  set "DPADFIX=2"
if "%HMD%"=="8"  set "DPADFIX=2"
if "%HMD%"=="9"  set "DPADFIX=2"
if "%HMD%"=="12" set "DPADFIX=2"
if "%HMD%"=="13" set "DPADFIX=2"
if "%HMD%"=="15" set "DPADFIX=2"
if "%HMD%"=="16" set "DPADFIX=2"

rem ---- WMR gets a different button layout, and only WMR ---------------------
rem  The chain is: physical button -> the mod's A/B/X/Y action -> an XInput
rem  button -> a game function, where A is Use, B is Med hypo, X is Hack/Reload
rem  and Y is Jump. The shipped ControllerLayout=0 sends action Y to Jump, and
rem  THE WMR PROFILE BINDS NO Y AT ALL, so WMR players have no jump button.
rem
rem  Layout 1 rotates the same four physical buttons onto Jump, Hack, Use and
rem  Med hypo. WMR then gets Jump, Hack and Use and loses only the hypo, which
rem  is reachable from the radial anyway. Trading the hypo for jump is the easy
rem  call when jump gates progression.
set "LAYOUTFIX="
if "%HMD%"=="12" set "LAYOUTFIX=1"
if defined LAYOUTFIX (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$src='using System;using System.Runtime.InteropServices;namespace W32{public class Ini{[DllImport(\"kernel32\",CharSet=CharSet.Unicode)]public static extern bool WritePrivateProfileString(string a,string b,string c,string d);}}'; Add-Type -TypeDefinition $src -ErrorAction SilentlyContinue; [void][W32.Ini]::WritePrivateProfileString('VR','ControllerLayout','%LAYOUTFIX%','%MODINI%')" 2>nul
    echo.
    echo   Controls:    set ControllerLayout=%LAYOUTFIX% so you have a jump button.
    echo                Your controllers bind no Y, and Y is jump on the default
    echo                layout. This trades the med hypo button for jump; the
    echo                hypo is still on the radial.
    call :log "headset fix: ControllerLayout=%LAYOUTFIX% for !HMDNAME! (no Y bound)"
)

if defined DPADFIX (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$src='using System;using System.Runtime.InteropServices;namespace W32{public class Ini{[DllImport(\"kernel32\",CharSet=CharSet.Unicode)]public static extern bool WritePrivateProfileString(string a,string b,string c,string d);}}'; Add-Type -TypeDefinition $src -ErrorAction SilentlyContinue; [void][W32.Ini]::WritePrivateProfileString('VR','ControllerDpadModifier','%DPADFIX%','%MODINI%')" 2>nul
    echo.
    echo   Controls:    set ControllerDpadModifier=%DPADFIX% for your headset.
    echo                Your controllers put the d-pad modifier somewhere your
    echo                thumb rests, which would have stopped you walking.
    call :log "headset fix: ControllerDpadModifier=%DPADFIX% for !HMDNAME!"
)

if "%HMD%"=="12" (
    echo.
    echo   Note:        Reverb / WMR controllers have a DIGITAL grip. If the
    echo                plasmid wheel does not open, that is the known cause --
    echo                say so in a bug report and include your logs.
    call :log "headset note: WMR digital grip caveat shown"
)

rem  Vive wands are the most constrained device the mod supports, and it is not
rem  close. The profile binds ONE face action, Use, on the right menu button;
rem  Med hypo, Hack/Reload and Jump have no button to go to. Jump could come
rem  from the trackpad click, but that click is the only workable d-pad
rem  modifier and the mod resolves the conflict in the modifier's favour. There
rem  is no configuration that fixes this, so say so instead of implying one.
if "%HMD%"=="7"  set "VIVEWARN=1"
if "%HMD%"=="8"  set "VIVEWARN=1"
if defined VIVEWARN (
    echo.
    echo   Note:        Vive wands have no A/B/X/Y buttons, so only USE is bound
    echo                to a button. Med hypo, hack/reload and jump have nowhere
    echo                to go on this controller. Everything else works. If your
    echo                headset came with Index controllers instead, pick 6.
    call :log "headset note: Vive wand missing-action warning shown"
)

rem  Index-family analog grips read high from a resting hand. The ini documents
rem  0.90 as the remedy but words it as conditional, and it is: it depends on
rem  hand size and how you hold them. Said, not written.
if "%HMD%"=="6"  set "GRIPNOTE=1"
if "%HMD%"=="9"  set "GRIPNOTE=1"
if "%HMD%"=="13" set "GRIPNOTE=1"
if "%HMD%"=="15" set "GRIPNOTE=1"
if defined GRIPNOTE (
    echo.
    echo   Note:        Index-style grips can read as held by a resting hand,
    echo                which eats your face buttons. If that happens, set
    echo                GripThreshold=0.90 in BioshockVR.ini.
    call :log "headset note: Index grip threshold hint shown"
)

rem  There is no Pico interaction profile in the mod. Pico's runtime usually
rem  advertises the Touch profile and everything works; if it does not, the
rem  session falls back to the simple controller, which reaches menus and
rem  nothing else. The log names whichever one actually matched, so point at it
rem  rather than guessing here.
if "%HMD%"=="14" (
    echo.
    echo   Note:        Pico is not a profile the mod ships bindings for, so it
    echo                relies on your runtime reporting Touch compatibility.
    echo                If the sticks and buttons do nothing in game, send your
    echo                log: the line ">>> INPUT: bound profile" names what
    echo                actually matched.
    call :log "headset note: Pico profile caveat shown"
)

rem  PSVR2 puts Options on the RIGHT controller, unlike Touch. Without the chord
rem  the modifier and the pause button are both right-handed and the map takes
rem  two hands on one controller. The chord is on the LEFT controller's two face
rem  buttons, so it frees the right thumb. Choosing SteamVR below already turns
rem  it on -- this only says so, because a control you do not know about is the
rem  same as one that does not exist.
if "%HMD%"=="16" (
    echo.
    echo   Note:        On PSVR2, pause is both face buttons on the LEFT
    echo                controller pressed together. SteamVR decides which
    echo                physical buttons those are, and its own controller
    echo                binding screen can move them if it picked badly.
    call :log "headset note: PSVR2 left-hand pause chord explained"
)
call :log "--- headset ---"
call :log "user reported: !HMDNAME!"

rem ===========================================================================
rem  RUNTIME -- this one is acted on.
rem
rem  BioshockHD.exe is 32-bit. A 32-bit process reads its OpenXR runtime from
rem  the WOW6432Node view of the registry, so that is the key we check.
rem
rem  Three ways a 32-bit app ends up with XR_ERROR_FILE_ACCESS_ERROR (-32):
rem  no runtime registered; a registered runtime whose manifest file is gone;
rem  or the key pointing at SteamVR, which has never shipped a 32-bit runtime.
rem  All three mean the real loader cannot work here.
rem ===========================================================================
set "XRRUNTIME="
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1" /v ActiveRuntime 2^>nul ^| findstr /i "ActiveRuntime"') do set "XRRUNTIME=%%B"

rem ---- THESE TESTS MUST USE ! AND NOT %, AND IT IS NOT A STYLE CHOICE -------
rem
rem  REPORTED 1.0.3, and it stopped Setup dead: "set was unexpected at this
rem  time", window closes, no runtime prompt, no log line. It happened to every
rem  user whose ONLY runtime is SteamVR -- Index, Vive, PSVR2 -- which is
rem  precisely the group that needs the bundled shim and could least afford the
rem  installer to die before offering it.
rem
rem  cmd expands every %VAR% on a line BEFORE it executes any of that line, so
rem  `if defined XRRUNTIME` guards the execution and does nothing whatsoever
rem  about the expansion beside it. With no 32-bit runtime registered the
rem  variable does not exist, the %XRRUNTIME:steamxr=% substitution cannot be
rem  resolved, and cmd reports the next token it can see -- `set`.
rem
rem  !VAR! is resolved at EXECUTION time, after the guard has been evaluated, so
rem  an undefined variable is simply empty and the comparison is ""=="".
rem  Delayed expansion is already on (line 24) and !RTNAME! below already relies
rem  on it. Do not "tidy" these back into % form.
set "HAS32=1"
if not defined XRRUNTIME set "HAS32=0"
if defined XRRUNTIME if not exist "!XRRUNTIME!" set "HAS32=0"
if defined XRRUNTIME if not "!XRRUNTIME:steamxr=!"=="!XRRUNTIME!" set "HAS32=0"

set "RTNAME=none"
if "%HAS32%"=="1" (
    set "RTNAME=unrecognised"
    rem  NO PIPES IN HERE. An echo piped into find, inside a parenthesised
    rem  block, makes cmd spawn child cmd.exe processes and re-parse the block,
    rem  which is a well-known way to get lines -- including a following set /p
    rem  prompt -- executed more than once. The substitution test below is
    rem  case-insensitive, needs no child process, and is the same idiom the
    rem  steamxr check above already uses.
    rem  ! not %, for the reason spelled out above the HAS32 tests. Inside a
    rem  parenthesised block the hazard is worse, not better: the WHOLE block is
    rem  parsed in one go, so these four expand before the `if` above them has
    rem  decided anything at all.
    if not "!XRRUNTIME:virtualdesktop=!"=="!XRRUNTIME!" set "RTNAME=VDXR"
    if not "!XRRUNTIME:oculus=!"=="!XRRUNTIME!"         set "RTNAME=Oculus OpenXR"
    if not "!XRRUNTIME:pimax=!"=="!XRRUNTIME!"          set "RTNAME=Pimax-OpenXR"
    if not "!XRRUNTIME:mixedreality=!"=="!XRRUNTIME!"   set "RTNAME=WMR OpenXR"
)

call :log "--- openxr ---"
call :log "ActiveRuntime json: %XRRUNTIME%"
call :log "usable 32-bit     : %HAS32%"
call :log "identified as     : !RTNAME!"

echo.
echo   -----------------------------------------
echo    Which runtime should the game use?
echo   -----------------------------------------
echo.
if "%HAS32%"=="1" (
    echo     Detected: !RTNAME!
) else (
    echo     Detected: no usable 32-bit OpenXR runtime
)
:askruntime
echo.
echo     1  Native OpenXR     Oculus, VDXR, Pimax, WMR      [recommended]
echo     2  SteamVR           via the OpenVR shim
echo     3  Unknown           use only if 1 and 2 both fail
echo     0  Cancel
echo.
echo     Press Enter to take the recommended option.
echo.
set "PICK="
set /p "PICK=  Runtime [1]: "

rem  NATIVE OPENXR IS THE DEFAULT. It routes to whatever runtime the system has
rem  active, so it works on Meta, VDXR, WMR, Varjo and Pimax without the shim in
rem  the way -- and it is the path the mod's own controller bindings apply to.
rem  The shim remains the answer when no 32-BIT OpenXR runtime is registered,
rem  which is the case this used to default to for everyone.
if not defined PICK set "PICK=1"

set "MODE="
if "%PICK%"=="1" set "MODE=STD"
if "%PICK%"=="2" set "MODE=SHIM"
if "%PICK%"=="0" goto :end
if "%PICK%"=="3" (
    if "%HAS32%"=="1" ( set "MODE=STD" ) else ( set "MODE=SHIM" )
    echo.
    echo   Guessing based on what is registered. If VR does not start, run this
    echo   again and pick 1 or 2 explicitly -- one of them will be right.
    call :log "user picked UNKNOWN, guessed !MODE!"
)

rem  Ask again rather than giving up. If this prompt ever repeats, it is
rem  because the answer was not 0, 1, 2 or 3 -- and the line below says so,
rem  which is the difference between a bug and a typo.
if not defined MODE (
    echo.
    echo   "%PICK%" is not one of the choices. Type 0, 1, 2 or 3.
    call :log "invalid runtime choice '%PICK%' -- re-asking"
    goto :askruntime
)

if "%MODE%"=="STD" if "%HAS32%"=="0" (
    echo.
    echo   WARNING: no usable 32-bit OpenXR runtime is registered. Native
    echo            OpenXR will most likely find no headset and run flat.
    call :log "WARNING: native chosen with no usable 32-bit runtime"
)

rem ---- pause chord: the RUNTIME asks for it, the HEADSET can refuse ---------
rem  Hold X+Y to pause. On the SHIM the menu button is not reliably delivered,
rem  so the chord is how you reach the pause menu at all. On native OpenXR the
rem  menu button works and the chord only costs you two buttons that can fire it
rem  by accident, so it is off there.
rem
rem  BUT IT IS A HARDWARE CAPABILITY BEFORE IT IS A RUNTIME PREFERENCE, and this
rem  used to be decided on the runtime alone. Read out of the mod's binding
rem  tables: the Vive wand profile binds NO face buttons at all, and the WMR
rem  profile binds X but no Y. There is no chord to press on either. Turning it
rem  on for them was inert, and worse, the controls summary would have told them
rem  to press two buttons their controllers do not have. Both profiles bind a
rem  left menu button, so pause stays there and works.
rem
rem  WRITTEN BOTH WAYS, not just when switching on: someone who runs this again
rem  and changes runtime must get the matching value, and a one-way write would
rem  leave the old one in place.
set "CHORD=0"
if "%MODE%"=="SHIM" set "CHORD=1"
set "NOCHORD="
if "%HMD%"=="7"  set "NOCHORD=1"
if "%HMD%"=="8"  set "NOCHORD=1"
if "%HMD%"=="12" set "NOCHORD=1"
if defined NOCHORD set "CHORD=0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$src='using System;using System.Runtime.InteropServices;namespace W32{public class Ini{[DllImport(\"kernel32\",CharSet=CharSet.Unicode)]public static extern bool WritePrivateProfileString(string a,string b,string c,string d);}}'; Add-Type -TypeDefinition $src -ErrorAction SilentlyContinue; [void][W32.Ini]::WritePrivateProfileString('VR','ControllerPauseChord','%CHORD%','%MODINI%')" 2>nul
if "%CHORD%"=="1" (
    echo   Controls:    hold X+Y to pause, enabled for SteamVR.
) else (
    if defined NOCHORD (
        echo   Controls:    pause is the menu button. Your controllers have no
        echo                second face button, so the X+Y chord is off.
    ) else (
        echo   Controls:    pause is the menu button; the X+Y chord is off.
    )
)
call :log "pause chord: ControllerPauseChord=%CHORD% for MODE=%MODE% hmd=%HMD%"

rem ---- install the loader ----------------------------------------------------
rem  These are RENAMES, not copies. Exactly two loader files should exist:
rem
rem    Native active:  openxr_loader.dll + openxr_loader_steam.dll
rem    Steam active:   openxr_loader.dll + openxr_loader_standard.dll
rem
rem  The inactive loader changes name. No duplicate source DLL is intentionally
rem  left behind. Every two-step rename has a rollback if the second move fails.
call :log "--- loader ---"
call :log "mode        : %MODE%"
call :log "game dir    : %GAMEDIR%"
call :loaderinventory

rem  Refuse tiny/non-DLL-looking files. The old unsafe logger could overwrite a
rem  DLL with one line of text, which Explorer rounded up and displayed as 1 KB.
if defined LIVE_EXISTS if "!LIVE_OK!"=="0" goto :loadercorrupt
if defined SHIM_EXISTS if "!SHIM_OK!"=="0" goto :loadercorrupt
if defined STD_EXISTS  if "!STD_OK!"=="0"  goto :loadercorrupt

if "%MODE%"=="SHIM" goto :selectsteam
goto :selectstandard

rem ============================== STEAM SHIM ==================================
:selectsteam
rem  Recover a clean state where the active name is temporarily missing.
if not defined LIVE_EXISTS if defined SHIM_EXISTS if defined STD_EXISTS (
    move /y "%GAMEDIR%%LDR_SHIM%" "%GAMEDIR%%LDR_LIVE%" >nul 2>&1
    if errorlevel 1 goto :loaderwritefail
    echo.
    echo   Loader:      SteamVR shim recovered and activated.
    call :log "loader: recovered steam shim as live loader"
    goto :loaderdone
)

rem  Normal already-active Steam state: live + standard, no _steam file.
if defined LIVE_EXISTS if defined STD_EXISTS if not defined SHIM_EXISTS (
    echo.
    echo   Loader:      SteamVR shim -- already active.
    call :log "loader: Steam state already active"
    goto :loaderdone
)

rem  Normal native state: live standard + _steam. Rename both.
if defined LIVE_EXISTS if defined SHIM_EXISTS if not defined STD_EXISTS goto :swap_to_steam

rem  Three valid files means a previous COPY-based setup left a duplicate.
if defined LIVE_EXISTS if defined SHIM_EXISTS if defined STD_EXISTS (
    set "LIVE_IS_SHIM=0"
    set "LIVE_IS_STD=0"
    call :sameas "%GAMEDIR%%LDR_LIVE%" "%GAMEDIR%%LDR_SHIM%"
    if "!ISSHIM!"=="1" set "LIVE_IS_SHIM=1"
    call :sameas "%GAMEDIR%%LDR_LIVE%" "%GAMEDIR%%LDR_STD%"
    if "!ISSHIM!"=="1" set "LIVE_IS_STD=1"

    if "!LIVE_IS_SHIM!"=="1" (
        del /f /q "%GAMEDIR%%LDR_SHIM%" >nul 2>&1
        if exist "%GAMEDIR%%LDR_SHIM%" goto :loaderwritefail
        echo.
        echo   Loader:      SteamVR shim -- already active; duplicate removed.
        call :log "loader: removed duplicate openxr_loader_steam.dll"
        goto :loaderdone
    )

    if "!LIVE_IS_STD!"=="1" (
        del /f /q "%GAMEDIR%%LDR_STD%" >nul 2>&1
        if exist "%GAMEDIR%%LDR_STD%" goto :loaderwritefail
        call :log "loader: removed duplicate openxr_loader_standard.dll before swap"
        goto :swap_to_steam
    )

    goto :loaderambiguous
)

goto :loadermissing

:swap_to_steam
move /y "%GAMEDIR%%LDR_LIVE%" "%GAMEDIR%%LDR_STD%" >nul 2>&1
if errorlevel 1 goto :loaderwritefail
move /y "%GAMEDIR%%LDR_SHIM%" "%GAMEDIR%%LDR_LIVE%" >nul 2>&1
if errorlevel 1 (
    move /y "%GAMEDIR%%LDR_STD%" "%GAMEDIR%%LDR_LIVE%" >nul 2>&1
    call :log "ERROR: Steam swap second rename failed; first rename rolled back"
    goto :loaderwritefail
)
if exist "%GAMEDIR%%LDR_SHIM%" goto :loaderverifyfail
if not exist "%GAMEDIR%%LDR_LIVE%" goto :loaderverifyfail
if not exist "%GAMEDIR%%LDR_STD%" goto :loaderverifyfail
rem  THE SETUPFAIL PAIR THAT USED TO SIT HERE BELONGED AT :loaderverifyfail, and
rem  here it fired on the SUCCESS path -- so every SteamVR install renamed both
rem  loaders correctly and then printed "SETUP DID NOT COMPLETE". Caught by the
rem  packaging dry run, which is the only reason it was ever seen: the three
rem  gotos above are what report a real verification failure, and reaching this
rem  line means all three passed.
echo.
echo   Loader:      SteamVR shim activated.
echo                Start SteamVR BEFORE launching the game.
call :log "loader: renamed live standard to openxr_loader_standard.dll"
call :log "loader: renamed openxr_loader_steam.dll to openxr_loader.dll"
goto :loaderdone

rem ============================= NATIVE OPENXR ================================
:selectstandard
rem  Recover a clean state where the active name is temporarily missing.
if not defined LIVE_EXISTS if defined SHIM_EXISTS if defined STD_EXISTS (
    move /y "%GAMEDIR%%LDR_STD%" "%GAMEDIR%%LDR_LIVE%" >nul 2>&1
    if errorlevel 1 goto :loaderwritefail
    echo.
    echo   Loader:      standard OpenXR loader recovered and activated.
    call :log "loader: recovered standard loader as live loader"
    goto :loaderdone
)

rem  Normal already-active native state: live + _steam, no _standard file.
if defined LIVE_EXISTS if defined SHIM_EXISTS if not defined STD_EXISTS (
    echo.
    echo   Loader:      standard OpenXR loader -- already active.
    call :log "loader: native state already active"
    goto :loaderdone
)

rem  Normal Steam state: live shim + _standard. Rename both.
if defined LIVE_EXISTS if defined STD_EXISTS if not defined SHIM_EXISTS goto :swap_to_standard

rem  Three valid files means a previous COPY-based setup left a duplicate.
if defined LIVE_EXISTS if defined SHIM_EXISTS if defined STD_EXISTS (
    set "LIVE_IS_SHIM=0"
    set "LIVE_IS_STD=0"
    call :sameas "%GAMEDIR%%LDR_LIVE%" "%GAMEDIR%%LDR_SHIM%"
    if "!ISSHIM!"=="1" set "LIVE_IS_SHIM=1"
    call :sameas "%GAMEDIR%%LDR_LIVE%" "%GAMEDIR%%LDR_STD%"
    if "!ISSHIM!"=="1" set "LIVE_IS_STD=1"

    if "!LIVE_IS_STD!"=="1" (
        del /f /q "%GAMEDIR%%LDR_STD%" >nul 2>&1
        if exist "%GAMEDIR%%LDR_STD%" goto :loaderwritefail
        echo.
        echo   Loader:      standard OpenXR loader -- already active; duplicate removed.
        call :log "loader: removed duplicate openxr_loader_standard.dll"
        goto :loaderdone
    )

    if "!LIVE_IS_SHIM!"=="1" (
        del /f /q "%GAMEDIR%%LDR_SHIM%" >nul 2>&1
        if exist "%GAMEDIR%%LDR_SHIM%" goto :loaderwritefail
        call :log "loader: removed duplicate openxr_loader_steam.dll before swap"
        goto :swap_to_standard
    )

    goto :loaderambiguous
)

goto :loadermissing

:swap_to_standard
move /y "%GAMEDIR%%LDR_LIVE%" "%GAMEDIR%%LDR_SHIM%" >nul 2>&1
if errorlevel 1 goto :loaderwritefail
move /y "%GAMEDIR%%LDR_STD%" "%GAMEDIR%%LDR_LIVE%" >nul 2>&1
if errorlevel 1 (
    move /y "%GAMEDIR%%LDR_SHIM%" "%GAMEDIR%%LDR_LIVE%" >nul 2>&1
    call :log "ERROR: native swap second rename failed; first rename rolled back"
    goto :loaderwritefail
)
if exist "%GAMEDIR%%LDR_STD%" goto :loaderverifyfail
if not exist "%GAMEDIR%%LDR_LIVE%" goto :loaderverifyfail
if not exist "%GAMEDIR%%LDR_SHIM%" goto :loaderverifyfail
echo.
echo   Loader:      standard OpenXR loader activated.
call :log "loader: renamed live Steam shim to openxr_loader_steam.dll"
call :log "loader: renamed openxr_loader_standard.dll to openxr_loader.dll"
goto :loaderdone

:loadercorrupt
set "SETUPFAIL=1"
set "FAILWHY=loader files are not valid DLLs"
echo.
echo   ERROR: one or more OpenXR loader files are tiny or are not valid DLLs.
echo          The old Setup logger overwrote them with text. Setup will not move
echo          bad files into the backup names.
echo.
echo          Re-extract clean loader DLLs from the mod package, then run Setup.
call :log "ERROR: loader validation failed; clean DLLs must be re-extracted"
goto :loaderdone

:loadermissing
set "SETUPFAIL=1"
set "FAILWHY=the loader pair is incomplete"
echo.
echo   ERROR: the loader pair is incomplete. Setup needs both the standard loader
echo          and the Steam shim so either selection can always be undone.
echo          Re-extract the mod package, then run Setup again.
call :log "ERROR: loader pair incomplete"
goto :loaderdone

:loaderambiguous
set "SETUPFAIL=1"
set "FAILWHY=the three-loader state is ambiguous"
echo.
echo   ERROR: all three loader names exist, but the live DLL matches neither saved
echo          DLL. Setup cannot safely guess which file is which. Re-extract the
echo          two clean loader DLLs and run Setup again.
call :log "ERROR: ambiguous three-loader state"
goto :loaderdone

:loaderwritefail
set "SETUPFAIL=1"
set "FAILWHY=a loader rename failed"
echo.
echo   ERROR: a loader rename failed. Close BioShock and SteamVR, then run Setup
echo          as administrator. Any completed first rename was rolled back.
call :log "ERROR: loader rename failed"
goto :loaderdone

:loaderverifyfail
set "SETUPFAIL=1"
set "FAILWHY=loader names did not verify"
echo.
echo   ERROR: loader rename commands returned success, but the final names are not
echo          correct. Close the game and inspect logs\setup.log.
call :log "ERROR: loader final-name verification failed"

:loaderdone

rem ---- write the helper scripts ----------------------------------------------
call :writescripts

rem ---- verify ----------------------------------------------------------------
echo.
echo   Verifying...
for /l %%I in (1,1,%NINI%) do (
    echo.
    call :say "  !INI%%I!"
    findstr /n "WindowedViewportX= FullscreenViewportX= StartupFullscreen= HorizontalFOV= LevelOfAnisotropy=" "!INI%%I!"
)
echo.
rem ---- DO NOT CLAIM SUCCESS AFTER A FAILURE --------------------------------
rem  Every loader ERROR branch above used to fall through to "Done. Launch the
rem  game with your headset on." A user who had just been told the loader pair
rem  was incomplete scrolled two screens and was told to put the headset on.
rem  The same was true after the not-writable warning, which fires near the top
rem  and was never consulted again.
if defined SETUPFAIL goto :setupfailed
if "%WRITEOK%"=="0" (
    set "SETUPFAIL=1"
    set "FAILWHY=the game folder is not writable"
    goto :setupfailed
)

echo   Done. Launch the game with your headset on.
echo   Above you should see %RESX%, StartupFullscreen=False, and
echo   LevelOfAnisotropy=%ANISO% on the D3D lines.

call :showcontrols

echo   Setup was recorded in logs\setup.log
echo   If anything goes wrong, run logs\CollectLogs.bat and send the zip.
call :log "--- setup finished ok ---"
goto :end

:setupfailed
echo.
echo  ============================================================
echo   SETUP DID NOT COMPLETE
echo  ============================================================
echo.
echo   Reason: %FAILWHY%
echo.
echo   The game will most likely start WITHOUT VR until this is fixed.
echo   Scroll up for the ERROR line -- it says exactly what to do.
echo.
echo   The two most common fixes:
echo     * Close BioShock and SteamVR, right-click Setup.bat and pick
echo       "Run as administrator".
echo     * Re-extract the mod package into the game folder and try again.
echo.
echo   Setup was recorded in logs\setup.log -- send that if you are stuck.
call :log "--- setup FAILED: %FAILWHY% ---"
echo.
pause
endlocal
exit /b 1

:end
echo.
pause
endlocal
exit /b 0

:showcontrols
rem ---------------------------------------------------------------------------
rem  THE TWO CONTROLS NOBODY CAN GUESS, stated for the configuration this run
rem  actually produced.
rem
rem  Both depend on answers given minutes apart: the pause control comes from the
rem  RUNTIME choice, because the menu button does not reliably bind on the shim,
rem  and the modifier comes from the HEADSET choice, because the default lands on
rem  a resting thumb for trackpad controllers. Nobody can derive the combination
rem  from either answer alone, and a user reported reaching the map only after
rem  being told the sequence.
rem
rem  Reads DPADCUR from the ini rather than assuming the default, so this stays
rem  right for someone who tuned it by hand and then re-ran Setup.
rem ---------------------------------------------------------------------------
set "MODNOW=%DPADCUR%"
if defined DPADFIX set "MODNOW=%DPADFIX%"

set "MODNAME=right thumbrest (rest your thumb on it)"
if "%MODNOW%"=="0" set "MODNAME=OFF, no modifier is bound"
if "%MODNOW%"=="2" set "MODNAME=right stick click (R3)"
if "%MODNOW%"=="3" set "MODNAME=left grip, WHICH DOES NOTHING, see below"
if "%MODNOW%"=="4" set "MODNAME=left thumbrest (rest your thumb on it)"
if "%MODNOW%"=="1" if not "%DPADFLIP%"=="0" set "MODNAME=left thumbrest (rest your thumb on it)"

rem  NAME THE BUTTONS THE USER ACTUALLY HAS, on the hand they are actually on.
rem
rem  Checked against each device's OpenXR profile and the vendor documentation,
rem  because two of these are not what anyone would assume:
rem
rem    Touch     ONE menu button, on the LEFT controller. The right controller's
rem              Meta button belongs to the system and cannot be bound.
rem    Index     THE OPENXR PROFILE HAS NO MENU PATH AT ALL. It exposes system
rem              (runtime owned) and the trackpad, nothing else, so the mod binds
rem              a FIRM PRESS ON THE LEFT TRACKPAD. An Index player hunting for a
rem              menu button will never find one, which is worth saying outright.
rem    Vive      a menu button on each controller, above the trackpad. Left one.
rem    WMR       a menu button on each controller. Left one.
rem    PSVR2     left controller carries Triangle and Square, right carries
rem              Circle, Cross and Options. So the chord is Triangle + Square.
set "MENUNAME=the Menu button on the LEFT controller"
if "%HMD%"=="6"  set "MENUNAME=a firm press on the LEFT trackpad (Index has no menu button)"
if "%HMD%"=="9"  set "MENUNAME=a firm press on the LEFT trackpad (Index has no menu button)"
if "%HMD%"=="13" set "MENUNAME=a firm press on the LEFT trackpad (Index has no menu button)"
if "%HMD%"=="15" set "MENUNAME=a firm press on the LEFT trackpad (Index has no menu button)"
if "%HMD%"=="7"  set "MENUNAME=the Menu button on the LEFT controller, above the trackpad"
if "%HMD%"=="8"  set "MENUNAME=the Menu button on the LEFT controller, above the trackpad"
if "%HMD%"=="16" set "MENUNAME=the Options button (SteamVR decides; it is on the RIGHT controller)"

set "PAUSENAME=%MENUNAME%"
if "%CHORD%"=="1" (
    set "PAUSENAME=X and Y together on the LEFT controller"
    if "%HMD%"=="6"  set "PAUSENAME=A and B together on the LEFT controller"
    if "%HMD%"=="9"  set "PAUSENAME=A and B together on the LEFT controller"
    if "%HMD%"=="13" set "PAUSENAME=A and B together on the LEFT controller"
    if "%HMD%"=="15" set "PAUSENAME=A and B together on the LEFT controller"
    if "%HMD%"=="16" set "PAUSENAME=Triangle and Square together on the LEFT controller"
    if "%HMD%"=="17" set "PAUSENAME=both face buttons on the LEFT controller"
)

echo.
echo   -----------------------------------------
echo    Your controls
echo   -----------------------------------------
echo.
echo     Pause            %PAUSENAME%
echo.
echo     Modifier         %MODNAME%
echo                      Hold it and the left stick stops moving you and
echo                      becomes a D-pad for the HUD. It is also what turns
echo                      Pause into the two entries below. Weapons and
echo                      plasmids are on the grip radial, not the D-pad.
echo.
echo     Map              hold the Modifier, then Pause, for half a second
echo     Alt Menu Button  hold the Modifier and tap Pause
echo.
if "%MODNOW%"=="3" (
    echo     WARNING: ControllerDpadModifier=3 cannot work. The grip is claimed
    echo              by the radial before the modifier is read, so the map is
    echo              unreachable. Use 1, 2 or 4.
    echo.
)
if "%MODNOW%"=="0" (
    echo     WARNING: with no modifier there is no map and no alt menu button.
    echo              Set ControllerDpadModifier to 1, 2 or 4.
    echo.
)
echo     Change any of it in BioshockVR.ini under Controllers.
echo.
call :log "controls: pause=%PAUSENAME%  modifier=%MODNOW% (%MODNAME%)  chord=%CHORD%  flip=%DPADFLIP%"
exit /b 0

:writescripts
rem ---------------------------------------------------------------------------
rem  Unpacks Uninstall.bat and logs\CollectLogs.bat from the payload at the end
rem  of THIS file.
rem
rem  Every payload line starts with ::U: or ::C:. The "::" makes each one a
rem  label as far as cmd is concerned, so even if execution ever reached them
rem  they would do nothing. PowerShell reads this file as data, keeps the lines
rem  with the right marker, and strips the four-character prefix.
rem
rem  PowerShell rather than a batch loop on purpose: the payload contains !, %,
rem  ^, & and parentheses, all of which a `for /f` + delayed-expansion loop
rem  would mangle. PowerShell reads raw bytes and cares about none of it.
rem ---------------------------------------------------------------------------
if not exist "%GAMEDIR%logs" mkdir "%GAMEDIR%logs" >nul 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=Get-Content -LiteralPath '%~f0'; Set-Content -LiteralPath '%GAMEDIR%Uninstall.bat' -Encoding ASCII -Value ($s | Where-Object { $_.StartsWith('::U:') } | ForEach-Object { $_.Substring(4) })" 2>nul
if exist "%GAMEDIR%Uninstall.bat" (
    echo   Wrote:       Uninstall.bat
    call :log "wrote Uninstall.bat"
) else (
    echo   WARNING: could not write Uninstall.bat
    call :log "ERROR: could not write Uninstall.bat"
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=Get-Content -LiteralPath '%~f0'; Set-Content -LiteralPath '%GAMEDIR%logs\CollectLogs.bat' -Encoding ASCII -Value ($s | Where-Object { $_.StartsWith('::C:') } | ForEach-Object { $_.Substring(4) })" 2>nul
if exist "%GAMEDIR%logs\CollectLogs.bat" (
    echo   Wrote:       logs\CollectLogs.bat
    call :log "wrote logs\CollectLogs.bat"
) else (
    echo   WARNING: could not write logs\CollectLogs.bat
    call :log "ERROR: could not write logs\CollectLogs.bat"
)
exit /b 0

:cleanupbrokenlogfiles
rem  Old :log treated the text after an arrow as a redirection target. That made
rem  small extensionless files named after byte counts. Preserve their text in
rem  setup.log, then remove only small all-numeric files containing our log words.
for /f "delims=" %%F in ('dir /b /a-d "%GAMEDIR%" 2^>nul ^| findstr /r "^[0-9][0-9]*$"') do call :cleanupone "%GAMEDIR%%%F"
exit /b 0

:cleanupone
set "STRAYSZ="
for %%S in ("%~1") do set "STRAYSZ=%%~zS"
if not defined STRAYSZ exit /b 0
if !STRAYSZ! GTR 4096 exit /b 0
findstr /i /c:"written ok" /c:"bytes" /c:"loader:" /c:"ERROR:" "%~1" >nul 2>&1
if errorlevel 1 exit /b 0
call :log "recovered stray logger file %~nx1 (!STRAYSZ! bytes):"
>>"%SETUPLOG%" type "%~1"
del /f /q "%~1" >nul 2>&1
if not exist "%~1" call :log "removed stray logger file %~nx1"
exit /b 0

:loaderinventory
set "LIVE_EXISTS="
set "SHIM_EXISTS="
set "STD_EXISTS="
set "LIVE_OK=0"
set "SHIM_OK=0"
set "STD_OK=0"
set "LIVE_SIZE=0"
set "SHIM_SIZE=0"
set "STD_SIZE=0"
if exist "%GAMEDIR%%LDR_LIVE%" set "LIVE_EXISTS=1"
if exist "%GAMEDIR%%LDR_SHIM%" set "SHIM_EXISTS=1"
if exist "%GAMEDIR%%LDR_STD%"  set "STD_EXISTS=1"
if defined LIVE_EXISTS (
    call :checkdll "%GAMEDIR%%LDR_LIVE%"
    set "LIVE_OK=!DLL_OK!"
    set "LIVE_SIZE=!DLL_SIZE!"
)
if defined SHIM_EXISTS (
    call :checkdll "%GAMEDIR%%LDR_SHIM%"
    set "SHIM_OK=!DLL_OK!"
    set "SHIM_SIZE=!DLL_SIZE!"
)
if defined STD_EXISTS (
    call :checkdll "%GAMEDIR%%LDR_STD%"
    set "STD_OK=!DLL_OK!"
    set "STD_SIZE=!DLL_SIZE!"
)
call :log "live: present=!LIVE_EXISTS! size=!LIVE_SIZE! valid=!LIVE_OK!"
call :log "steam: present=!SHIM_EXISTS! size=!SHIM_SIZE! valid=!SHIM_OK!"
call :log "standard: present=!STD_EXISTS! size=!STD_SIZE! valid=!STD_OK!"
exit /b 0

:checkdll
set "DLL_OK=0"
set "DLL_SIZE=0"
if not exist "%~1" exit /b 0
for %%S in ("%~1") do set "DLL_SIZE=%%~zS"
if !DLL_SIZE! LSS 4096 exit /b 0
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p='%~1'; try{$s=[IO.File]::OpenRead($p); $a=$s.ReadByte(); $b=$s.ReadByte(); $s.Dispose(); if($a -eq 77 -and $b -eq 90){exit 0}}catch{}; exit 1" >nul 2>&1
if not errorlevel 1 set "DLL_OK=1"
exit /b 0

:sameas
rem  %1 and %2 = two files. Sets ISSHIM=1 if they are byte-identical.
rem  Its own subroutine so `if errorlevel` is never read inside a block, where
rem  it is easy to get wrong.
set "ISSHIM=0"
fc /b "%~1" "%~2" >nul 2>&1
if not errorlevel 1 set "ISSHIM=1"
exit /b 0

:say
rem  Echo a path SAFELY -- see the note in Uninstall.bat. A path containing
rem  parentheses, expanded unquoted inside a block, truncates the block.
echo %~1
exit /b 0

:addini
rem ---------------------------------------------------------------------------
rem  %1 = full path to a Bioshock.ini,  %2 = EPIC / STEAM / ANY
rem  Adds it to the list if it exists and is not already there. Sets PRIMARY
rem  the first time a config matching THIS install's store turns up.
rem ---------------------------------------------------------------------------
if not exist "%~1" exit /b 0
for /l %%I in (1,1,%NINI%) do if /i "!INI%%I!"=="%~1" exit /b 0
set /a NINI+=1
set "INI!NINI!=%~1"
if not defined PRIMARY if /i "%~2"=="%STORE%" set "PRIMARY=%~1"
exit /b 0

:writeone
rem ---------------------------------------------------------------------------
rem  %1 = full path to one Bioshock.ini. Backs it up, writes our keys, and
rem  restores the backup if the file came out obviously wrong.
rem
rem  LevelOfAnisotropy exists in five sections. The PS4 one is skipped; the
rem  three Direct3D device sections and Engine.RenderConfig are all set, so it
rem  takes whichever the game actually reads.
rem ---------------------------------------------------------------------------
set "ONE=%~1"
echo.
call :say "  Writing:     !ONE!"

if not exist "!ONE!.vrbackup" copy /y "!ONE!" "!ONE!.vrbackup" >nul
for %%F in ("!ONE!") do set "SZB=%%~zF"
call :log "backup: !ONE!.vrbackup (!SZB! bytes)"

powershell -NoProfile -ExecutionPolicy Bypass -Command "$d='[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string a, string b, string c, string d);'; Add-Type -MemberDefinition $d -Name Ini -Namespace W32 | Out-Null; $f='!ONE!'; $w='WinDrv.WindowsClient'; $u='ShockGame.ShockUserSettings'; $r='Engine.RenderConfig'; $set=@(@($w,'WindowedViewportX','%RESX%'),@($w,'WindowedViewportY','%RESY%'),@($w,'FullscreenViewportX','%RESX%'),@($w,'FullscreenViewportY','%RESY%'),@($w,'MenuViewportX','%RESX%'),@($w,'MenuViewportY','%RESY%'),@($w,'StartupFullscreen','False'),@($u,'HorizontalFOV','%FOV%'),@($u,'bHorizontalFOVLock','True'),@($r,'HorizontalFOVLock','True'),@($r,'LevelOfAnisotropy','%ANISO%'),@('D3DDrv.D3DRenderDevice','LevelOfAnisotropy','%ANISO%'),@('D3DDrv10.D3DRenderDevice10','LevelOfAnisotropy','%ANISO%'),@('D3DDrv11.D3DRenderDevice11','LevelOfAnisotropy','%ANISO%')); foreach($k in $set){ [void][W32.Ini]::WritePrivateProfileString($k[0],$k[1],$k[2],$f) }; [void][W32.Ini]::WritePrivateProfileString($null,$null,$null,$f)"

if errorlevel 1 (
    echo                FAILED - the write step did not run.
    call :log "ERROR: write step failed for !ONE!"
    exit /b 0
)

for %%F in ("!ONE!") do set "SZA=%%~zF"
set /a "HALF=!SZB!/2"
if !SZA! LSS !HALF! (
    echo                File shrank from !SZB! to !SZA! bytes - restoring backup.
    copy /y "!ONE!.vrbackup" "!ONE!" >nul
    call :log "ERROR: !ONE! shrank from !SZB! to !SZA! bytes, backup restored"
    exit /b 0
)

echo                ok  (!SZB! -^> !SZA! bytes)
call :log "written ok: !ONE!  !SZB! to !SZA! bytes"
exit /b 0

:log
rem  Store first, then expand with delayed expansion.
rem  Characters such as > are written as text, not treated as redirection.
set "LOGMSG=%~1"
>>"%SETUPLOG%" echo(!LOGMSG!
exit /b 0






rem ===========================================================================
rem  EMBEDDED PAYLOAD -- generated. Do not edit by hand, do not reorder.
rem    ::U:  ->  Uninstall.bat
rem    ::C:  ->  logs\CollectLogs.bat
rem ===========================================================================

::U:@echo off
::U:rem ===========================================================================
::U:rem  BioShock Remastered VR -- Uninstall            BUILD 2026-08-13-a
::U:rem
::U:rem  Run this from the BioShock Remastered game folder, beside BioshockHD.exe.
::U:rem
::U:rem  Restores every Bioshock.ini.vrbackup created by Setup before removing the
::U:rem  mod. Deletes only explicit BioShock VR filenames and its own folders.
::U:rem  The uninstaller deletes itself last, but only after every required action
::U:rem  succeeds. If anything is locked or restoration fails, it stays so the
::U:rem  uninstall can be retried safely.
::U:rem ===========================================================================
::U:
::U:setlocal EnableExtensions DisableDelayedExpansion
::U:
::U:set "BUILD=2026-08-13-a"
::U:set "GAMEDIR=%~dp0"
::U:set "FAILED=0"
::U:set "RESTORED=0"
::U:set "GAMEINI="
::U:
::U:echo.
::U:echo   BioShock Remastered VR -- Uninstall
::U:echo   -----------------------------------------
::U:echo   Build:  %BUILD%
::U:call :say "  Folder: %GAMEDIR%"
::U:echo.
::U:
::U:rem  Validate the LOCATION, not one mod file. A partially removed install may no
::U:rem  longer have BioshockVR.dll, but the uninstaller must still be able to finish.
::U:if not exist "%GAMEDIR%BioshockHD.exe" (
::U:    echo   BioshockHD.exe was not found here.
::U:    echo   Put Uninstall.bat in the same folder as BioshockHD.exe, then run it.
::U:    echo.
::U:    pause
::U:    exit /b 1
::U:)
::U:
::U:rem ---- remember the exact config path before deleting BioshockVR.ini ---------
::U:if exist "%GAMEDIR%BioshockVR.ini" (
::U:    for /f "usebackq eol=; tokens=1,* delims==" %%A in ("%GAMEDIR%BioshockVR.ini") do (
::U:        if /i "%%A"=="GameIniPath" set "GAMEINI=%%B"
::U:    )
::U:)
::U:
::U:echo   What will happen:
::U:echo.
::U:echo     - every Bioshock.ini.vrbackup found is restored in place
::U:echo     - all BioShock VR DLLs, loader aliases, scripts and logs go
::U:echo     - your tuned BioshockVR.ini is KEPT, renamed to BioshockVR.ini.bak
::U:echo     - dxgi.dll and winmm.dll are shared with ReShade and Special K, so
::U:echo       you will be asked before either one is removed
::U:echo     - save games and original game files are not touched
::U:echo     - this uninstaller deletes itself last if everything succeeds
::U:echo.
::U:
::U:if defined GAMEINI (
::U:    echo   Config recorded by Setup:
::U:    call :say "      %GAMEINI%"
::U:) else (
::U:    echo   No GameIniPath was recorded; all standard Steam and Epic AppData
::U:    echo   locations will still be checked for a .vrbackup.
::U:)
::U:
::U:echo.
::U:set "OK="
::U:set /p "OK=  Type  yes  to continue: "
::U:if /i not "%OK%"=="yes" (
::U:    echo.
::U:    echo   Cancelled. Nothing was changed.
::U:    echo.
::U:    pause
::U:    exit /b 0
::U:)
::U:
::U:echo.
::U:echo   Restoring BioShock configuration backups...
::U:echo.
::U:
::U:rem  Restore the configured path first. The standard paths are then checked too;
::U:rem  a duplicate is harmless because the first successful restore removes backup.
::U:if defined GAMEINI call :restoreini "%GAMEINI%"
::U:call :restoreini "%APPDATA%\My Games\BioshockHD\Bioshock\Bioshock.ini"
::U:call :restoreini "%APPDATA%\BioshockHD\Bioshock\Bioshock.ini"
::U:call :restoreini "%APPDATA%\My Games\Bioshock Epic HD\Bioshock\Bioshock.ini"
::U:call :restoreini "%APPDATA%\Bioshock Epic HD\Bioshock\Bioshock.ini"
::U:call :restoreini "%USERPROFILE%\Documents\My Games\Bioshock Epic HD\Bioshock\Bioshock.ini"
::U:call :restoreini "%USERPROFILE%\Documents\My Games\BioshockHD\Bioshock\Bioshock.ini"
::U:
::U:if "%RESTORED%"=="0" (
::U:    echo   No Bioshock.ini.vrbackup was found. Bioshock.ini was left unchanged.
::U:)
::U:
::U:echo.
::U:echo   Removing mod files...
::U:echo.
::U:
::U:rem ---- OpenXR loaders: active name plus both parked variants -----------------
::U:call :kill "openxr_loader.dll"
::U:call :kill "openxr_loader_standard.dll"
::U:call :kill "openxr_loader_steam.dll"
::U:call :kill "openxr_loader_original.dll"
::U:
::U:rem ---- current mod payload ---------------------------------------------------
::U:call :kill "BioshockVR.dll"
::U:call :kill "openvr_api.dll"
::U:
::U:rem ---- YOUR SETTINGS ARE KEPT ------------------------------------------------
::U:rem  BioshockVR.ini is the one file here that is YOURS. Per-weapon grip, rotation
::U:rem  and crosshair values are tuned by hand in the headset over hours and are
::U:rem  written back into it as you go -- deleting it throws that away, and a
::U:rem  reinstall would have restored it for free. Renamed, not removed.
::U:call :keep "BioshockVR.ini"
::U:
::U:rem ---- shared filenames: ASK FIRST -------------------------------------------
::U:rem  dxgi.dll and winmm.dll are how this mod is loaded, but they are also how
::U:rem  ReShade, Special K and DXVK are loaded -- one filename, one owner. If the
::U:rem  user installed one of those AFTER this mod, the file in this folder is
::U:rem  theirs and deleting it silently breaks a tool we never installed.
::U:call :asktokill "dxgi.dll"
::U:call :asktokill "winmm.dll"
::U:
::U:rem ---- known older loader routes from this project ---------------------------
::U:call :kill "FirstTimeSetup.bat"
::U:call :kill "SelectRuntime.bat"
::U:
::U:rem ---- installer and loose logs ---------------------------------------------
::U:call :kill "Setup.bat"
::U:call :kill "Setup_fixed.bat"
::U:call :kill "README.txt"
::U:call :kill "changelog.txt"
::U:call :kill "setup.log"
::U:call :kill "BioshockVR.log"
::U:call :kill "BioshockVR_loader.log"
::U:call :kill "openxr_shim.log"
::U:call :kill "openvr_api.log"
::U:
::U:rem ---- folders owned by this mod --------------------------------------------
::U:call :killdir "openvr_input"
::U:call :killdir "logs"
::U:
::U:rem ---- fallback log location used when Program Files is not writable --------
::U:if exist "%LOCALAPPDATA%\BioshockVR" (
::U:    rd /s /q "%LOCALAPPDATA%\BioshockVR" >nul 2>&1
::U:    if exist "%LOCALAPPDATA%\BioshockVR" (
::U:        call :say "    [x] could not remove %LOCALAPPDATA%\BioshockVR\"
::U:        set "FAILED=1"
::U:    ) else (
::U:        call :say "    removed  %LOCALAPPDATA%\BioshockVR\"
::U:    )
::U:)
::U:
::U:rem ===========================================================================
::U:rem  REPORT
::U:rem ===========================================================================
::U:echo.
::U:echo   -----------------------------------------
::U:if "%FAILED%"=="0" goto :allclean
::U:
::U:echo   Finished, but something could not be restored or removed.
::U:echo.
::U:echo   The game or SteamVR is probably still using a DLL, or Windows denied a
::U:echo   config restore. Close the game, SteamVR and any Explorer window open
::U:echo   inside the game folder, then run this file again.
::U:echo.
::U:echo   This uninstaller is being kept so the operation can be retried safely.
::U:echo   Any backup that failed to restore was also kept.
::U:echo   -----------------------------------------
::U:echo.
::U:pause
::U:exit /b 1
::U:
::U::allclean
::U:echo   Done. BioShock VR files were removed and all found INI backups restored.
::U:echo.
::U:echo   Your tuned settings were kept as BioshockVR.ini.bak. Rename it back to
::U:echo   BioshockVR.ini if you reinstall, and every weapon stays calibrated.
::U:echo   -----------------------------------------
::U:echo.
::U:echo   This uninstaller will now delete itself.
::U:echo.
::U:pause
::U:
::U:rem  A child cmd waits briefly until this cmd releases the batch-file handle,
::U:rem  then removes the final file. Nothing belonging to the mod is left behind.
::U:start "" /b "%ComSpec%" /d /c "ping 127.0.0.1 -n 2 >nul & del /f /q ""%~f0""" >nul 2>&1
::U:endlocal
::U:exit /b 0
::U:
::U:rem ===========================================================================
::U:rem  SUBROUTINES
::U:rem ===========================================================================
::U:
::U::say
::U:echo %~1
::U:exit /b 0
::U:
::U::restoreini
::U:rem  %1 = full path of Bioshock.ini, without .vrbackup
::U:if "%~1"=="" exit /b 0
::U:if not exist "%~1.vrbackup" exit /b 0
::U:
::U:call :say "    restoring %~1"
::U:copy /y "%~1.vrbackup" "%~1" >nul 2>&1
::U:if errorlevel 1 goto :restorefail
::U:if not exist "%~1" goto :restorefail
::U:
::U:rem  Verify the restored file is byte-for-byte identical before deleting backup.
::U:fc /b "%~1.vrbackup" "%~1" >nul 2>&1
::U:if errorlevel 1 goto :restorefail
::U:
::U:del /f /q "%~1.vrbackup" >nul 2>&1
::U:if exist "%~1.vrbackup" (
::U:    call :say "    [x] restored INI, but could not remove backup: %~1.vrbackup"
::U:    set "FAILED=1"
::U:    set "RESTORED=1"
::U:    exit /b 0
::U:)
::U:
::U:call :say "    restored  %~1"
::U:set "RESTORED=1"
::U:exit /b 0
::U:
::U::restorefail
::U:call :say "    [x] restore failed; backup kept at %~1.vrbackup"
::U:set "FAILED=1"
::U:exit /b 0
::U:
::U::kill
::U:rem  %1 = filename relative to GAMEDIR
::U:if not exist "%GAMEDIR%%~1" exit /b 0
::U:del /f /q "%GAMEDIR%%~1" >nul 2>&1
::U:if exist "%GAMEDIR%%~1" (
::U:    call :say "    [x] in use   %GAMEDIR%%~1"
::U:    set "FAILED=1"
::U:    exit /b 0
::U:)
::U:echo     removed  %~1
::U:exit /b 0
::U:
::U::keep
::U:rem  %1 = filename relative to GAMEDIR. Renamed to .bak rather than deleted.
::U:rem
::U:rem  An existing .bak is overwritten deliberately: it is from an earlier
::U:rem  uninstall of the same mod, and the file being renamed now is the newer
::U:rem  tuning. Keeping the older one would preserve the wrong version.
::U:if not exist "%GAMEDIR%%~1" exit /b 0
::U:if exist "%GAMEDIR%%~1.bak" del /f /q "%GAMEDIR%%~1.bak" >nul 2>&1
::U:move /y "%GAMEDIR%%~1" "%GAMEDIR%%~1.bak" >nul 2>&1
::U:if exist "%GAMEDIR%%~1" (
::U:    call :say "    [x] in use   %GAMEDIR%%~1"
::U:    set "FAILED=1"
::U:    exit /b 0
::U:)
::U:echo     KEPT     %~1 renamed to %~1.bak - your tuning is safe
::U:exit /b 0
::U:
::U::asktokill
::U:rem  %1 = filename relative to GAMEDIR, shared with other graphics mods.
::U:rem
::U:rem  Default is YES on Enter: the common case by far is that this file is ours.
::U:rem  The prompt exists for the minority who installed ReShade afterwards, and
::U:rem  for them a wrong answer is a broken tool with no clue why.
::U:if not exist "%GAMEDIR%%~1" exit /b 0
::U:echo.
::U:echo     %~1 is used by this mod, and by ReShade, Special K and DXVK.
::U:echo     If you installed one of those AFTER this mod, keep it.
::U:set "DELOK="
::U:set /p "DELOK=    Delete %~1? [Y/n]: "
::U:if /i "%DELOK%"=="n" (
::U:    call :say "    kept     %GAMEDIR%%~1  (at your request)"
::U:    exit /b 0
::U:)
::U:call :kill "%~1"
::U:exit /b 0
::U:
::U::killdir
::U:rem  %1 = folder relative to GAMEDIR
::U:if not exist "%GAMEDIR%%~1" exit /b 0
::U:rd /s /q "%GAMEDIR%%~1" >nul 2>&1
::U:if exist "%GAMEDIR%%~1" (
::U:    call :say "    [x] in use   %GAMEDIR%%~1\"
::U:    set "FAILED=1"
::U:    exit /b 0
::U:)
::U:echo     removed  %~1\
::U:exit /b 0
::C:@echo off
::C:rem  NO delayed expansion: nothing here needs it, and with it enabled every
::C:rem  `!` becomes a variable marker -- which is why "[!] Zipping failed."
::C:rem  printed as "[] Zipping failed." and told you nothing.
::C:setlocal
::C:title BioshockVR - Collect Logs
::C:
::C:set "BUILD=2026-08-13-a"
::C:
::C:REM ============================================================================
::C:REM  CollectLogs.bat        LIVES IN THE logs\ FOLDER
::C:REM
::C:REM  Two picks and one optional sentence. That is the whole thing.
::C:REM
::C:REM  Fewer questions on purpose: a short form that everyone finishes beats a
::C:REM  long form that half of people abandon, and the logs carry the detail
::C:REM  anyway. All we need from the user is WHEN and WHAT -- enough to know which
::C:REM  part of the log to read first.
::C:REM
::C:REM  Zipping uses PowerShell's Compress-Archive, present on every Windows 10
::C:REM  and 11 install. Nothing to download.
::C:REM ============================================================================
::C:
::C:cd /d "%~dp0"
::C:
::C:REM  Clear anything a previous run left here BEFORE gathering. A copy whose
::C:REM  source has since disappeared -- a VirtualStore folder that got cleaned up,
::C:REM  a log from an install that was removed -- would otherwise ride along in this
::C:REM  bundle looking exactly as current as the rest of it.
::C:call :sweep
::C:
::C:REM  NO wmic. It is a Feature-on-Demand Microsoft is removing from Windows 11,
::C:REM  and where it is missing this fell back to naming every bundle "report" --
::C:REM  so two reports from the same person overwrote each other on the Desktop.
::C:REM  PowerShell formats the stamp directly, which is fewer moving parts than
::C:REM  slicing LocalDateTime by character offset ever was.
::C:for /f "usebackq delims=" %%A in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Date -Format 'yyyy-MMdd-HHmm'" 2^>nul`) do set "STAMP=%%A"
::C:if not defined STAMP set "STAMP=report"
::C:
::C:set "ISSUE=ISSUE.txt"
::C:REM DESKTOP IS NOT ALWAYS %USERPROFILE%\Desktop. Under OneDrive Known Folder
::C:REM Move -- on by default for a large share of consumer Windows 11 -- it lives at
::C:REM %USERPROFILE%\OneDrive\Desktop, and Compress-Archive then fails with "could
::C:REM not find a part of the path", which reads to the user as the tool being
::C:REM broken. Prefer the redirected one, and fall back to this folder if neither
::C:REM exists so the bundle is always produced somewhere.
::C:set "ZIP=%USERPROFILE%\Desktop\BioshockVR-logs-%STAMP%.zip"
::C:if exist "%USERPROFILE%\OneDrive\Desktop\" set "ZIP=%USERPROFILE%\OneDrive\Desktop\BioshockVR-logs-%STAMP%.zip"
::C:if not exist "%USERPROFILE%\Desktop\" if not exist "%USERPROFILE%\OneDrive\Desktop\" set "ZIP=%~dp0BioshockVR-logs-%STAMP%.zip"
::C:
::C:cls
::C:echo.
::C:echo  ============================================================
::C:echo   BioshockVR - Log Collector       build %BUILD%
::C:echo  ============================================================
::C:echo.
::C:echo   Two quick questions, then everything zips to your Desktop.
::C:echo.
::C:
::C:REM ------------------------------------------------------------------ WHEN
::C:echo  ------------------------------------------------------------
::C:echo   When did the problem occur?
::C:echo  ------------------------------------------------------------
::C:echo.
::C:echo     1  Before launch                7  Saving / loading
::C:echo     2  During launch                8  Inventory / map / hacking
::C:echo     3  Main menu                    9  Combat
::C:echo     4  General gameplay            10  Level transition
::C:echo     5  Menus                       11  Other
::C:echo     6  Cutscenes
::C:echo.
::C:set "WHEN="
::C:set /p "WHEN=  Number: "
::C:
::C:set "WHENTXT=not answered"
::C:if "%WHEN%"=="1"  set "WHENTXT=Before launch"
::C:if "%WHEN%"=="2"  set "WHENTXT=During launch"
::C:if "%WHEN%"=="3"  set "WHENTXT=Main menu"
::C:if "%WHEN%"=="4"  set "WHENTXT=General gameplay"
::C:if "%WHEN%"=="5"  set "WHENTXT=Menus"
::C:if "%WHEN%"=="6"  set "WHENTXT=Cutscenes"
::C:if "%WHEN%"=="7"  set "WHENTXT=Saving / loading"
::C:if "%WHEN%"=="8"  set "WHENTXT=Inventory / map / hacking"
::C:if "%WHEN%"=="9"  set "WHENTXT=Combat"
::C:if "%WHEN%"=="10" set "WHENTXT=Level transition"
::C:if "%WHEN%"=="11" (
::C:    set "WHENTXT="
::C:    set /p "WHENTXT=  Describe when: "
::C:    if not defined WHENTXT set "WHENTXT=Other (not described)"
::C:)
::C:
::C:REM ------------------------------------------------------------------ WHAT
::C:echo.
::C:echo  ------------------------------------------------------------
::C:echo   What was the problem?
::C:echo  ------------------------------------------------------------
::C:echo.
::C:echo     1  Game did not start
::C:echo     2  Crash
::C:echo     3  Controllers not working
::C:echo     4  Black screen
::C:echo     5  Flat 2D image, no VR
::C:echo     6  Warped or stretched image / distortion
::C:echo     7  A feature does not work correctly
::C:echo     8  Bad performance - low framerate or stutter
::C:echo     9  Other
::C:echo.
::C:set "WHAT="
::C:set /p "WHAT=  Number: "
::C:
::C:set "WHATTXT=not answered"
::C:if "%WHAT%"=="1" set "WHATTXT=Game did not start"
::C:if "%WHAT%"=="2" set "WHATTXT=Crash"
::C:if "%WHAT%"=="3" set "WHATTXT=Controllers not working"
::C:if "%WHAT%"=="4" set "WHATTXT=Black screen"
::C:if "%WHAT%"=="5" set "WHATTXT=Flat 2D image, no VR"
::C:if "%WHAT%"=="6" set "WHATTXT=Warped or stretched image / distortion"
::C:if "%WHAT%"=="7" set "WHATTXT=A feature does not work correctly"
::C:if "%WHAT%"=="8" set "WHATTXT=Performance - low framerate or stutter"
::C:if "%WHAT%"=="8" set "PERF=1"
::C:if "%WHAT%"=="9" (
::C:    set "WHATTXT="
::C:    set /p "WHATTXT=  Describe the problem: "
::C:    if not defined WHATTXT set "WHATTXT=Other (not described)"
::C:)
::C:
::C:REM ================================================================ write it
::C:> "%ISSUE%" echo ============================================================
::C:>>"%ISSUE%" echo  BioshockVR issue report
::C:>>"%ISSUE%" echo  Generated %DATE% %TIME%
::C:>>"%ISSUE%" echo ============================================================
::C:>>"%ISSUE%" echo.
::C:>>"%ISSUE%" echo WHEN : %WHENTXT%
::C:>>"%ISSUE%" echo WHAT : %WHATTXT%
::C:>>"%ISSUE%" echo.
::C:>>"%ISSUE%" echo ------------------------------------------------------------
::C:>>"%ISSUE%" echo  FILES IN THIS ZIP
::C:>>"%ISSUE%" echo ------------------------------------------------------------
::C:REM The list is written FURTHER DOWN, after every copy above has happened. It
::C:REM used to be built here, before them, so it never mentioned the files it was
::C:REM supposed to describe. A manifest exists to be trusted.
::C:
::C:REM The mod config goes in too -- almost every report needs it.
::C:if exist "..\BioshockVR.ini" copy /y "..\BioshockVR.ini" "BioshockVR.ini.copy" >nul
::C:
::C:REM ============================================================================
::C:REM  GATHER FROM EVERY PLACE A LOG CAN ACTUALLY LAND
::C:REM
::C:REM  This used to zip its own folder and nothing else, which fails in exactly the
::C:REM  case it exists for. Three redirections can move the evidence somewhere this
::C:REM  folder is not:
::C:REM
::C:REM    1. The mod relocates its log to LocalAppData when the game folder is not
::C:REM       writable -- the normal case for a Program Files install without admin.
::C:REM    2. VirtualStore silently redirects a 32-bit game's writes to Program Files
::C:REM       into a per-user shadow copy. The write SUCCEEDS and the file appears
::C:REM       nowhere the user looks. That is why tuned settings "don't save".
::C:REM    3. The game's own Bioshock.ini lives under the user profile, and it is the
::C:REM       file Setup modified -- the direct cause of the two commonest reports.
::C:REM ============================================================================
::C:
::C:if exist "%LOCALAPPDATA%\BioshockVR\logs\BioshockVR.log" (
::C:    copy /y "%LOCALAPPDATA%\BioshockVR\logs\BioshockVR.log" "BioshockVR.localappdata.log" >nul
::C:    echo   [i] Also collected the LocalAppData copy of the mod log.
::C:)
::C:
::C:for %%V in (
::C:    "%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Steam\steamapps\common\BioShock Remastered\Build\Final"
::C:    "%LOCALAPPDATA%\VirtualStore\Program Files\Epic Games\BioshockRemastered\Build\FinalEpic"
::C:    "%LOCALAPPDATA%\VirtualStore\Program Files\Epic Games\BioshockRemastered\Build\Final"
::C:    "%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Epic Games\BioshockRemastered\Build\FinalEpic"
::C:    "%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Epic Games\BioshockRemastered\Build\Final"
::C:) do (
::C:    if exist "%%~V\BioshockVR.ini" (
::C:        copy /y "%%~V\BioshockVR.ini" "BioshockVR.virtualstore.ini" >nul
::C:        echo   [!] VirtualStore copy found - your settings are being redirected.
::C:    )
::C:    if exist "%%~V\logs\BioshockVR.log" copy /y "%%~V\logs\BioshockVR.log" "BioshockVR.virtualstore.log" >nul
::C:)
::C:
::C:for %%G in (
::C:    "%APPDATA%\My Games\BioshockHD\Bioshock\Bioshock.ini"
::C:    "%APPDATA%\BioshockHD\Bioshock\Bioshock.ini"
::C:    "%APPDATA%\My Games\Bioshock Epic HD\Bioshock\Bioshock.ini"
::C:    "%APPDATA%\Bioshock Epic HD\Bioshock\Bioshock.ini"
::C:) do (
::C:    if exist "%%~G" copy /y "%%~G" "Bioshock.game.ini.copy" >nul
::C:)
::C:
::C:REM ============================================================================
::C:REM  PERFORMANCE SUMMARY -- only when the user picked the performance option.
::C:REM
::C:REM  Every number here is ALREADY in the log. The point is to lift it to the top
::C:REM  so a framerate complaint arrives with its own evidence instead of a
::C:REM  paragraph of prose, and it cannot disagree with the log it came from.
::C:REM ============================================================================
::C:if defined PERF (
::C:    > "PERFORMANCE.txt" echo ============================================================
::C:    >>"PERFORMANCE.txt" echo  BioshockVR performance summary
::C:    >>"PERFORMANCE.txt" echo ============================================================
::C:    >>"PERFORMANCE.txt" echo.
::C:    >>"PERFORMANCE.txt" echo ---- what the mod was asked to render ----
::C:    if exist "BioshockVR.log" findstr /c:"ResolutionX" /c:"ResolutionY" /c:"GameFovDegrees" /c:"ForegroundFovValue" /c:"MirrorPresentEvery" "BioshockVR.log" >>"PERFORMANCE.txt"
::C:    >>"PERFORMANCE.txt" echo.
::C:    >>"PERFORMANCE.txt" echo ---- frame timing ----
::C:    if exist "BioshockVR.log" findstr /c:"PER PRESENT" /c:"PER SUBMIT" /c:"frames:" /c:"EYEQ" "BioshockVR.log" >>"PERFORMANCE.txt"
::C:    >>"PERFORMANCE.txt" echo.
::C:    >>"PERFORMANCE.txt" echo ---- runtime and GPU ----
::C:    if exist "BioshockVR.log" findstr /c:"XR: runtime" /c:"adapter LUID" "BioshockVR.log" >>"PERFORMANCE.txt"
::C:    >>"PERFORMANCE.txt" echo.
::C:    >>"PERFORMANCE.txt" echo ---- machine ----
::C:    powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $o=Get-CimInstance Win32_OperatingSystem; $c=Get-CimInstance Win32_Processor; $v=Get-CimInstance Win32_VideoController; 'OS  : '+$o.Caption; 'CPU : '+($c.Name -join ', '); 'RAM : '+[math]::Round($o.TotalVisibleMemorySize/1MB,1)+' GB'; foreach($g in $v){'GPU : '+$g.Name+'  driver '+$g.DriverVersion} } catch { 'machine details unavailable' }" >>"PERFORMANCE.txt" 2>nul
::C:    echo   [i] Performance summary written.
::C:)
::C:
::C:REM Now that everything has been gathered, describe what is actually here.
::C:for %%F in (*.log *.ini *.txt *.copy) do >>"%ISSUE%" echo   %%F   (%%~zF bytes, %%~tF)
::C:
::C:echo.
::C:echo   Zipping...
::C:
::C:REM  The pipe below is NOT caret-escaped -- it sits inside a double-quoted
::C:REM  -Command string, so cmd passes it through untouched. Escaping it sent a
::C:REM  a caret plus pipe to PowerShell, a syntax error, which the try/catch
::C:REM  swallowed into a bare "Zipping failed" with no clue why. That was the bug.
::C:REM
::C:REM  Also explicit about paths: %~dp0 rather than '.', and .FullName strings
::C:REM  rather than FileInfo objects, because Compress-Archive -Path wants strings
::C:REM  and quietly misbehaves when handed objects.
::C:REM
::C:REM  Errors are written to ziperror.txt so a failure says something useful.
::C:powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $src='%~dp0'.TrimEnd('\'); $f=Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Name -ne 'CollectLogs.bat' -and $_.Name -ne 'ziperror.txt' } | ForEach-Object { $_.FullName }; if (-not $f) { throw 'no files to zip' }; Compress-Archive -LiteralPath $f -DestinationPath '%ZIP%' -Force; exit 0 } catch { $_.Exception.Message | Out-File -FilePath (Join-Path '%~dp0' 'ziperror.txt') -Encoding ASCII; exit 1 }"
::C:
::C:if errorlevel 1 (
::C:    echo.
::C:    echo  [x] Zipping failed.
::C:    if exist "%~dp0ziperror.txt" (
::C:        echo.
::C:        echo      Reason:
::C:        type "%~dp0ziperror.txt"
::C:    )
::C:    echo.
::C:    echo      Do it by hand instead - it works just as well:
::C:    echo        1. Select every file in this folder.
::C:    echo        2. Right-click, "Send to", "Compressed (zipped) folder".
::C:    echo        3. Send that zip.
::C:    echo.
::C:    REM  The gathered copies are deliberately LEFT here on failure -- they are
::C:    REM  what the manual zip above is meant to contain.
::C:    pause
::C:    exit /b 1
::C:)
::C:
::C:call :sweep
::C:
::C:echo.
::C:echo  ============================================================
::C:echo   Done.
::C:echo  ============================================================
::C:echo.
::C:echo   On your Desktop:  BioshockVR-logs-%STAMP%.zip
::C:echo.
::C:echo   Send that one file. It has the logs, your settings, your
::C:echo   setup record, and your answers in it.
::C:echo.
::C:
::C:explorer /select,"%ZIP%"
::C:pause
::C:exit /b 0
::C:
::C:REM ============================================================================
::C:REM  SWEEP -- remove everything this script COPIED IN or GENERATED.
::C:REM
::C:REM  It used to delete only BioshockVR.ini.copy, so a second run re-zipped the
::C:REM  first run's LocalAppData log, VirtualStore config, game ini and performance
::C:REM  summary alongside the current ones. Stale evidence carrying a fresh
::C:REM  timestamp is worse than no evidence: it sends whoever reads the bundle after
::C:REM  a problem that was already fixed.
::C:REM
::C:REM  The real logs are NOT touched -- those belong to the mod, not to this
::C:REM  script, and the folder must look afterwards exactly as it did before.
::C:REM ============================================================================
::C::sweep
::C:for %%D in (
::C:    "ISSUE.txt"
::C:    "PERFORMANCE.txt"
::C:    "ziperror.txt"
::C:    "BioshockVR.ini.copy"
::C:    "BioshockVR.localappdata.log"
::C:    "BioshockVR.virtualstore.ini"
::C:    "BioshockVR.virtualstore.log"
::C:    "Bioshock.game.ini.copy"
::C:) do if exist "%~dp0%%~D" del /f /q "%~dp0%%~D" >nul 2>&1
::C:exit /b 0
