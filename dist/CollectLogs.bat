@echo off
rem  NO delayed expansion: nothing here needs it, and with it enabled every
rem  `!` becomes a variable marker -- which is why "[!] Zipping failed."
rem  printed as "[] Zipping failed." and told you nothing.
setlocal
title BioshockVR - Collect Logs

set "BUILD=2026-07-31-c"

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
set "ZIP=%USERPROFILE%\Desktop\BioshockVR-logs-%STAMP%.zip"

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
echo     8  Other
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
if "%WHAT%"=="8" (
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
for %%F in (*.log *.ini *.txt *.copy) do >>"%ISSUE%" echo   %%F   (%%~zF bytes, %%~tF)

REM The mod config goes in too -- almost every report needs it.
if exist "..\BioshockVR.ini" copy /y "..\BioshockVR.ini" "BioshockVR.ini.copy" >nul

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
