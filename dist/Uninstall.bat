@echo off
rem ===========================================================================
rem  BioShock Remastered VR -- Uninstall            BUILD 2026-08-13-a
rem
rem  Run this from the BioShock Remastered game folder, beside BioshockHD.exe.
rem
rem  Restores every Bioshock.ini.vrbackup created by Setup before removing the
rem  mod. Deletes only explicit BioShock VR filenames and its own folders.
rem  The uninstaller deletes itself last, but only after every required action
rem  succeeds. If anything is locked or restoration fails, it stays so the
rem  uninstall can be retried safely.
rem ===========================================================================

setlocal EnableExtensions DisableDelayedExpansion

set "BUILD=2026-08-13-a"
set "GAMEDIR=%~dp0"
set "FAILED=0"
set "RESTORED=0"
set "GAMEINI="

echo.
echo   BioShock Remastered VR -- Uninstall
echo   -----------------------------------------
echo   Build:  %BUILD%
call :say "  Folder: %GAMEDIR%"
echo.

rem  Validate the LOCATION, not one mod file. A partially removed install may no
rem  longer have BioshockVR.dll, but the uninstaller must still be able to finish.
if not exist "%GAMEDIR%BioshockHD.exe" (
    echo   BioshockHD.exe was not found here.
    echo   Put Uninstall.bat in the same folder as BioshockHD.exe, then run it.
    echo.
    pause
    exit /b 1
)

rem ---- remember the exact config path before deleting BioshockVR.ini ---------
if exist "%GAMEDIR%BioshockVR.ini" (
    for /f "usebackq eol=; tokens=1,* delims==" %%A in ("%GAMEDIR%BioshockVR.ini") do (
        if /i "%%A"=="GameIniPath" set "GAMEINI=%%B"
    )
)

echo   What will happen:
echo.
echo     - every Bioshock.ini.vrbackup found is restored in place
echo     - all BioShock VR DLLs, loader aliases, scripts and logs go
echo     - your tuned BioshockVR.ini is KEPT, renamed to BioshockVR.ini.bak
echo     - dxgi.dll and winmm.dll are shared with ReShade and Special K, so
echo       you will be asked before either one is removed
echo     - save games and original game files are not touched
echo     - this uninstaller deletes itself last if everything succeeds
echo.

if defined GAMEINI (
    echo   Config recorded by Setup:
    call :say "      %GAMEINI%"
) else (
    echo   No GameIniPath was recorded; all standard Steam and Epic AppData
    echo   locations will still be checked for a .vrbackup.
)

echo.
set "OK="
set /p "OK=  Type  yes  to continue: "
if /i not "%OK%"=="yes" (
    echo.
    echo   Cancelled. Nothing was changed.
    echo.
    pause
    exit /b 0
)

echo.
echo   Restoring BioShock configuration backups...
echo.

rem  Restore the configured path first. The standard paths are then checked too;
rem  a duplicate is harmless because the first successful restore removes backup.
if defined GAMEINI call :restoreini "%GAMEINI%"
call :restoreini "%APPDATA%\My Games\BioshockHD\Bioshock\Bioshock.ini"
call :restoreini "%APPDATA%\BioshockHD\Bioshock\Bioshock.ini"
call :restoreini "%APPDATA%\My Games\Bioshock Epic HD\Bioshock\Bioshock.ini"
call :restoreini "%APPDATA%\Bioshock Epic HD\Bioshock\Bioshock.ini"
call :restoreini "%USERPROFILE%\Documents\My Games\Bioshock Epic HD\Bioshock\Bioshock.ini"
call :restoreini "%USERPROFILE%\Documents\My Games\BioshockHD\Bioshock\Bioshock.ini"

if "%RESTORED%"=="0" (
    echo   No Bioshock.ini.vrbackup was found. Bioshock.ini was left unchanged.
)

echo.
echo   Removing mod files...
echo.

rem ---- OpenXR loaders: active name plus both parked variants -----------------
call :kill "openxr_loader.dll"
call :kill "openxr_loader_standard.dll"
call :kill "openxr_loader_steam.dll"
call :kill "openxr_loader_original.dll"

rem ---- current mod payload ---------------------------------------------------
call :kill "BioshockVR.dll"
call :kill "openvr_api.dll"

rem ---- YOUR SETTINGS ARE KEPT ------------------------------------------------
rem  BioshockVR.ini is the one file here that is YOURS. Per-weapon grip, rotation
rem  and crosshair values are tuned by hand in the headset over hours and are
rem  written back into it as you go -- deleting it throws that away, and a
rem  reinstall would have restored it for free. Renamed, not removed.
call :keep "BioshockVR.ini"

rem ---- shared filenames: ASK FIRST -------------------------------------------
rem  dxgi.dll and winmm.dll are how this mod is loaded, but they are also how
rem  ReShade, Special K and DXVK are loaded -- one filename, one owner. If the
rem  user installed one of those AFTER this mod, the file in this folder is
rem  theirs and deleting it silently breaks a tool we never installed.
call :asktokill "dxgi.dll"
call :asktokill "winmm.dll"

rem ---- known older loader routes from this project ---------------------------
call :kill "FirstTimeSetup.bat"
call :kill "SelectRuntime.bat"

rem ---- installer and loose logs ---------------------------------------------
call :kill "Setup.bat"
call :kill "Setup_fixed.bat"
call :kill "README.txt"
call :kill "changelog.txt"
call :kill "setup.log"
call :kill "BioshockVR.log"
call :kill "BioshockVR_loader.log"
call :kill "openxr_shim.log"
call :kill "openvr_api.log"

rem ---- folders owned by this mod --------------------------------------------
call :killdir "openvr_input"
call :killdir "logs"

rem ---- fallback log location used when Program Files is not writable --------
if exist "%LOCALAPPDATA%\BioshockVR" (
    rd /s /q "%LOCALAPPDATA%\BioshockVR" >nul 2>&1
    if exist "%LOCALAPPDATA%\BioshockVR" (
        call :say "    [x] could not remove %LOCALAPPDATA%\BioshockVR\"
        set "FAILED=1"
    ) else (
        call :say "    removed  %LOCALAPPDATA%\BioshockVR\"
    )
)

rem ===========================================================================
rem  REPORT
rem ===========================================================================
echo.
echo   -----------------------------------------
if "%FAILED%"=="0" goto :allclean

echo   Finished, but something could not be restored or removed.
echo.
echo   The game or SteamVR is probably still using a DLL, or Windows denied a
echo   config restore. Close the game, SteamVR and any Explorer window open
echo   inside the game folder, then run this file again.
echo.
echo   This uninstaller is being kept so the operation can be retried safely.
echo   Any backup that failed to restore was also kept.
echo   -----------------------------------------
echo.
pause
exit /b 1

:allclean
echo   Done. BioShock VR files were removed and all found INI backups restored.
echo.
echo   Your tuned settings were kept as BioshockVR.ini.bak. Rename it back to
echo   BioshockVR.ini if you reinstall, and every weapon stays calibrated.
echo   -----------------------------------------
echo.
echo   This uninstaller will now delete itself.
echo.
pause

rem  A child cmd waits briefly until this cmd releases the batch-file handle,
rem  then removes the final file. Nothing belonging to the mod is left behind.
start "" /b "%ComSpec%" /d /c "ping 127.0.0.1 -n 2 >nul & del /f /q ""%~f0""" >nul 2>&1
endlocal
exit /b 0

rem ===========================================================================
rem  SUBROUTINES
rem ===========================================================================

:say
echo %~1
exit /b 0

:restoreini
rem  %1 = full path of Bioshock.ini, without .vrbackup
if "%~1"=="" exit /b 0
if not exist "%~1.vrbackup" exit /b 0

call :say "    restoring %~1"
copy /y "%~1.vrbackup" "%~1" >nul 2>&1
if errorlevel 1 goto :restorefail
if not exist "%~1" goto :restorefail

rem  Verify the restored file is byte-for-byte identical before deleting backup.
fc /b "%~1.vrbackup" "%~1" >nul 2>&1
if errorlevel 1 goto :restorefail

del /f /q "%~1.vrbackup" >nul 2>&1
if exist "%~1.vrbackup" (
    call :say "    [x] restored INI, but could not remove backup: %~1.vrbackup"
    set "FAILED=1"
    set "RESTORED=1"
    exit /b 0
)

call :say "    restored  %~1"
set "RESTORED=1"
exit /b 0

:restorefail
call :say "    [x] restore failed; backup kept at %~1.vrbackup"
set "FAILED=1"
exit /b 0

:kill
rem  %1 = filename relative to GAMEDIR
if not exist "%GAMEDIR%%~1" exit /b 0
del /f /q "%GAMEDIR%%~1" >nul 2>&1
if exist "%GAMEDIR%%~1" (
    call :say "    [x] in use   %GAMEDIR%%~1"
    set "FAILED=1"
    exit /b 0
)
echo     removed  %~1
exit /b 0

:keep
rem  %1 = filename relative to GAMEDIR. Renamed to .bak rather than deleted.
rem
rem  An existing .bak is overwritten deliberately: it is from an earlier
rem  uninstall of the same mod, and the file being renamed now is the newer
rem  tuning. Keeping the older one would preserve the wrong version.
if not exist "%GAMEDIR%%~1" exit /b 0
if exist "%GAMEDIR%%~1.bak" del /f /q "%GAMEDIR%%~1.bak" >nul 2>&1
move /y "%GAMEDIR%%~1" "%GAMEDIR%%~1.bak" >nul 2>&1
if exist "%GAMEDIR%%~1" (
    call :say "    [x] in use   %GAMEDIR%%~1"
    set "FAILED=1"
    exit /b 0
)
echo     KEPT     %~1 renamed to %~1.bak - your tuning is safe
exit /b 0

:asktokill
rem  %1 = filename relative to GAMEDIR, shared with other graphics mods.
rem
rem  Default is YES on Enter: the common case by far is that this file is ours.
rem  The prompt exists for the minority who installed ReShade afterwards, and
rem  for them a wrong answer is a broken tool with no clue why.
if not exist "%GAMEDIR%%~1" exit /b 0
echo.
echo     %~1 is used by this mod, and by ReShade, Special K and DXVK.
echo     If you installed one of those AFTER this mod, keep it.
set "DELOK="
set /p "DELOK=    Delete %~1? [Y/n]: "
if /i "%DELOK%"=="n" (
    call :say "    kept     %GAMEDIR%%~1  (at your request)"
    exit /b 0
)
call :kill "%~1"
exit /b 0

:killdir
rem  %1 = folder relative to GAMEDIR
if not exist "%GAMEDIR%%~1" exit /b 0
rd /s /q "%GAMEDIR%%~1" >nul 2>&1
if exist "%GAMEDIR%%~1" (
    call :say "    [x] in use   %GAMEDIR%%~1\"
    set "FAILED=1"
    exit /b 0
)
echo     removed  %~1\
exit /b 0
