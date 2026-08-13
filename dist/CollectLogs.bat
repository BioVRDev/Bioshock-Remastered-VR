@echo off
rem  NO delayed expansion: nothing here needs it, and with it enabled every
rem  `!` becomes a variable marker -- which is why "[!] Zipping failed."
rem  printed as "[] Zipping failed." and told you nothing.
setlocal
title BioshockVR - Collect Logs

set "BUILD=2026-08-12-a"

REM ============================================================================
REM  CollectLogs.bat        LIVES IN THE logs\ FOLDER
REM
REM  Two picks and one optional sentence. That is the whole thing.
REM
REM  Fewer questions on purpose: a short form that everyone finishes beats a
REM  long form that half of people abandon, and the logs carry the detail
REM  anyway. All we need from the user is WHEN and WHAT -- enough to know which
REM  part of the log to read first.
REM
REM  Zipping uses PowerShell's Compress-Archive, present on every Windows 10
REM  and 11 install. Nothing to download.
REM ============================================================================

cd /d "%~dp0"

for /f "tokens=2 delims==" %%A in ('wmic os get LocalDateTime /value 2^>nul') do set "DT=%%A"
set "STAMP=%DT:~0,4%-%DT:~4,2%%DT:~6,2%-%DT:~8,2%%DT:~10,2%"
if "%DT%"=="" set "STAMP=report"

set "ISSUE=ISSUE.txt"
REM DESKTOP IS NOT ALWAYS %USERPROFILE%\Desktop. Under OneDrive Known Folder
REM Move -- on by default for a large share of consumer Windows 11 -- it lives at
REM %USERPROFILE%\OneDrive\Desktop, and Compress-Archive then fails with "could
REM not find a part of the path", which reads to the user as the tool being
REM broken. Prefer the redirected one, and fall back to this folder if neither
REM exists so the bundle is always produced somewhere.
set "ZIP=%USERPROFILE%\Desktop\BioshockVR-logs-%STAMP%.zip"
if exist "%USERPROFILE%\OneDrive\Desktop\" set "ZIP=%USERPROFILE%\OneDrive\Desktop\BioshockVR-logs-%STAMP%.zip"
if not exist "%USERPROFILE%\Desktop\" if not exist "%USERPROFILE%\OneDrive\Desktop\" set "ZIP=%~dp0BioshockVR-logs-%STAMP%.zip"

cls
echo.
echo  ============================================================
echo   BioshockVR - Log Collector       build %BUILD%
echo  ============================================================
echo.
echo   Two quick questions, then everything zips to your Desktop.
echo.

REM ------------------------------------------------------------------ WHEN
echo  ------------------------------------------------------------
echo   When did the problem occur?
echo  ------------------------------------------------------------
echo.
echo     1  Before launch                7  Saving / loading
echo     2  During launch                8  Inventory / map / hacking
echo     3  Main menu                    9  Combat
echo     4  General gameplay            10  Level transition
echo     5  Menus                       11  Other
echo     6  Cutscenes
echo.
set "WHEN="
set /p "WHEN=  Number: "

set "WHENTXT=not answered"
if "%WHEN%"=="1"  set "WHENTXT=Before launch"
if "%WHEN%"=="2"  set "WHENTXT=During launch"
if "%WHEN%"=="3"  set "WHENTXT=Main menu"
if "%WHEN%"=="4"  set "WHENTXT=General gameplay"
if "%WHEN%"=="5"  set "WHENTXT=Menus"
if "%WHEN%"=="6"  set "WHENTXT=Cutscenes"
if "%WHEN%"=="7"  set "WHENTXT=Saving / loading"
if "%WHEN%"=="8"  set "WHENTXT=Inventory / map / hacking"
if "%WHEN%"=="9"  set "WHENTXT=Combat"
if "%WHEN%"=="10" set "WHENTXT=Level transition"
if "%WHEN%"=="11" (
    set "WHENTXT="
    set /p "WHENTXT=  Describe when: "
    if not defined WHENTXT set "WHENTXT=Other (not described)"
)

REM ------------------------------------------------------------------ WHAT
echo.
echo  ------------------------------------------------------------
echo   What was the problem?
echo  ------------------------------------------------------------
echo.
echo     1  Game did not start
echo     2  Crash
echo     3  Controllers not working
echo     4  Black screen
echo     5  Flat 2D image, no VR
echo     6  Warped or stretched image / distortion
echo     7  A feature does not work correctly
echo     8  Bad performance - low framerate or stutter
echo     9  Other
echo.
set "WHAT="
set /p "WHAT=  Number: "

set "WHATTXT=not answered"
if "%WHAT%"=="1" set "WHATTXT=Game did not start"
if "%WHAT%"=="2" set "WHATTXT=Crash"
if "%WHAT%"=="3" set "WHATTXT=Controllers not working"
if "%WHAT%"=="4" set "WHATTXT=Black screen"
if "%WHAT%"=="5" set "WHATTXT=Flat 2D image, no VR"
if "%WHAT%"=="6" set "WHATTXT=Warped or stretched image / distortion"
if "%WHAT%"=="7" set "WHATTXT=A feature does not work correctly"
if "%WHAT%"=="8" set "WHATTXT=Performance - low framerate or stutter"
if "%WHAT%"=="8" set "PERF=1"
if "%WHAT%"=="9" (
    set "WHATTXT="
    set /p "WHATTXT=  Describe the problem: "
    if not defined WHATTXT set "WHATTXT=Other (not described)"
)

REM ================================================================ write it
> "%ISSUE%" echo ============================================================
>>"%ISSUE%" echo  BioshockVR issue report
>>"%ISSUE%" echo  Generated %DATE% %TIME%
>>"%ISSUE%" echo ============================================================
>>"%ISSUE%" echo.
>>"%ISSUE%" echo WHEN : %WHENTXT%
>>"%ISSUE%" echo WHAT : %WHATTXT%
>>"%ISSUE%" echo.
>>"%ISSUE%" echo ------------------------------------------------------------
>>"%ISSUE%" echo  FILES IN THIS ZIP
>>"%ISSUE%" echo ------------------------------------------------------------
REM The list is written FURTHER DOWN, after every copy above has happened. It
REM used to be built here, before them, so it never mentioned the files it was
REM supposed to describe. A manifest exists to be trusted.

REM The mod config goes in too -- almost every report needs it.
if exist "..\BioshockVR.ini" copy /y "..\BioshockVR.ini" "BioshockVR.ini.copy" >nul

REM ============================================================================
REM  GATHER FROM EVERY PLACE A LOG CAN ACTUALLY LAND
REM
REM  This used to zip its own folder and nothing else, which fails in exactly the
REM  case it exists for. Three redirections can move the evidence somewhere this
REM  folder is not:
REM
REM    1. The mod relocates its log to LocalAppData when the game folder is not
REM       writable -- the normal case for a Program Files install without admin.
REM    2. VirtualStore silently redirects a 32-bit game's writes to Program Files
REM       into a per-user shadow copy. The write SUCCEEDS and the file appears
REM       nowhere the user looks. That is why tuned settings "don't save".
REM    3. The game's own Bioshock.ini lives under the user profile, and it is the
REM       file Setup modified -- the direct cause of the two commonest reports.
REM ============================================================================

if exist "%LOCALAPPDATA%\BioshockVR\logs\BioshockVR.log" (
    copy /y "%LOCALAPPDATA%\BioshockVR\logs\BioshockVR.log" "BioshockVR.localappdata.log" >nul
    echo   [i] Also collected the LocalAppData copy of the mod log.
)

for %%V in (
    "%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Steam\steamapps\common\BioShock Remastered\Build\Final"
    "%LOCALAPPDATA%\VirtualStore\Program Files\Epic Games\BioshockRemastered\Build\FinalEpic"
    "%LOCALAPPDATA%\VirtualStore\Program Files\Epic Games\BioshockRemastered\Build\Final"
    "%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Epic Games\BioshockRemastered\Build\FinalEpic"
    "%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Epic Games\BioshockRemastered\Build\Final"
) do (
    if exist "%%~V\BioshockVR.ini" (
        copy /y "%%~V\BioshockVR.ini" "BioshockVR.virtualstore.ini" >nul
        echo   [!] VirtualStore copy found - your settings are being redirected.
    )
    if exist "%%~V\logs\BioshockVR.log" copy /y "%%~V\logs\BioshockVR.log" "BioshockVR.virtualstore.log" >nul
)

for %%G in (
    "%APPDATA%\My Games\BioshockHD\Bioshock\Bioshock.ini"
    "%APPDATA%\BioshockHD\Bioshock\Bioshock.ini"
    "%APPDATA%\My Games\Bioshock Epic HD\Bioshock\Bioshock.ini"
    "%APPDATA%\Bioshock Epic HD\Bioshock\Bioshock.ini"
) do (
    if exist "%%~G" copy /y "%%~G" "Bioshock.game.ini.copy" >nul
)

REM ============================================================================
REM  PERFORMANCE SUMMARY -- only when the user picked the performance option.
REM
REM  Every number here is ALREADY in the log. The point is to lift it to the top
REM  so a framerate complaint arrives with its own evidence instead of a
REM  paragraph of prose, and it cannot disagree with the log it came from.
REM ============================================================================
if defined PERF (
    > "PERFORMANCE.txt" echo ============================================================
    >>"PERFORMANCE.txt" echo  BioshockVR performance summary
    >>"PERFORMANCE.txt" echo ============================================================
    >>"PERFORMANCE.txt" echo.
    >>"PERFORMANCE.txt" echo ---- what the mod was asked to render ----
    if exist "BioshockVR.log" findstr /c:"ResolutionX" /c:"ResolutionY" /c:"GameFovDegrees" /c:"ForegroundFovValue" /c:"MirrorPresentEvery" "BioshockVR.log" >>"PERFORMANCE.txt"
    >>"PERFORMANCE.txt" echo.
    >>"PERFORMANCE.txt" echo ---- frame timing ----
    if exist "BioshockVR.log" findstr /c:"PER PRESENT" /c:"PER SUBMIT" /c:"frames:" /c:"EYEQ" "BioshockVR.log" >>"PERFORMANCE.txt"
    >>"PERFORMANCE.txt" echo.
    >>"PERFORMANCE.txt" echo ---- runtime and GPU ----
    if exist "BioshockVR.log" findstr /c:"XR: runtime" /c:"adapter LUID" "BioshockVR.log" >>"PERFORMANCE.txt"
    >>"PERFORMANCE.txt" echo.
    >>"PERFORMANCE.txt" echo ---- machine ----
    powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $o=Get-CimInstance Win32_OperatingSystem; $c=Get-CimInstance Win32_Processor; $v=Get-CimInstance Win32_VideoController; 'OS  : '+$o.Caption; 'CPU : '+($c.Name -join ', '); 'RAM : '+[math]::Round($o.TotalVisibleMemorySize/1MB,1)+' GB'; foreach($g in $v){'GPU : '+$g.Name+'  driver '+$g.DriverVersion} } catch { 'machine details unavailable' }" >>"PERFORMANCE.txt" 2>nul
    echo   [i] Performance summary written.
)

REM Now that everything has been gathered, describe what is actually here.
for %%F in (*.log *.ini *.txt *.copy) do >>"%ISSUE%" echo   %%F   (%%~zF bytes, %%~tF)

echo.
echo   Zipping...

REM  The pipe below is NOT caret-escaped -- it sits inside a double-quoted
REM  -Command string, so cmd passes it through untouched. Escaping it sent a
REM  a caret plus pipe to PowerShell, a syntax error, which the try/catch
REM  swallowed into a bare "Zipping failed" with no clue why. That was the bug.
REM
REM  Also explicit about paths: %~dp0 rather than '.', and .FullName strings
REM  rather than FileInfo objects, because Compress-Archive -Path wants strings
REM  and quietly misbehaves when handed objects.
REM
REM  Errors are written to ziperror.txt so a failure says something useful.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $src='%~dp0'.TrimEnd('\'); $f=Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Name -ne 'CollectLogs.bat' -and $_.Name -ne 'ziperror.txt' } | ForEach-Object { $_.FullName }; if (-not $f) { throw 'no files to zip' }; Compress-Archive -LiteralPath $f -DestinationPath '%ZIP%' -Force; exit 0 } catch { $_.Exception.Message | Out-File -FilePath (Join-Path '%~dp0' 'ziperror.txt') -Encoding ASCII; exit 1 }"

if errorlevel 1 (
    echo.
    echo  [x] Zipping failed.
    if exist "%~dp0ziperror.txt" (
        echo.
        echo      Reason:
        type "%~dp0ziperror.txt"
    )
    echo.
    echo      Do it by hand instead - it works just as well:
    echo        1. Select every file in this folder.
    echo        2. Right-click, "Send to", "Compressed (zipped) folder".
    echo        3. Send that zip.
    echo.
    if exist "%~dp0BioshockVR.ini.copy" del "%~dp0BioshockVR.ini.copy" >nul 2>&1
    pause
    exit /b 1
)

if exist "%~dp0ziperror.txt" del "%~dp0ziperror.txt" >nul 2>&1
if exist "%~dp0BioshockVR.ini.copy" del "%~dp0BioshockVR.ini.copy" >nul 2>&1

echo.
echo  ============================================================
echo   Done.
echo  ============================================================
echo.
echo   On your Desktop:  BioshockVR-logs-%STAMP%.zip
echo.
echo   Send that one file. It has the logs, your settings, your
echo   setup record, and your answers in it.
echo.

explorer /select,"%ZIP%"
pause
exit /b 0
