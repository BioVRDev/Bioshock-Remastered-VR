// BioshockVR/Core/dllmain.cpp
//
// Entry point and logging.
//
// Config lived here too until it outgrew the file: ~138 globals, their ini
// reads, and a 150-line echo, alongside the entry point and the logger. It now
// lives in Config.cpp behind the single g_cfg struct in Config.h.
//
// What stays here is what genuinely belongs to the DLL itself: finding a
// writable place for the log (harder than it sounds -- see DirTakesOurLog), and
// the init thread that loads config, syncs the game ini, and arms the hooks.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include "Core/Config.h"
#include "Render/Hooks.h"
#include "Game/GameIni.h"

#include <MinHook.h>

static HMODULE           g_hSelf = nullptr;
static CRITICAL_SECTION  g_logLock;
static char              g_logPath[MAX_PATH] = {};
static char              g_iniPath[MAX_PATH] = {};


// ============================================================================
//  LOGGING
// ============================================================================

// TRUE once the log had to move out of the game folder. Reported in the header
// so a log someone sends us says where it came from.
static bool g_logRelocated = false;

// Does a write to this directory ACTUALLY land there?
//
// MEASURED: on a Program Files install without admin rights, Windows silently
// redirects the write into %LOCALAPPDATA%\VirtualStore\Program Files\... The
// open SUCCEEDS, the file exists, and the user never finds it -- which is why
// "I have no log file" reports could never be confirmed or denied. Opening the
// file and asking Windows where the handle really points is the only reliable
// test; checking the return code is not enough.
static bool DirTakesOurLog(const char* dir, char* out, size_t outSz)
{
    char probe[MAX_PATH] = {};
    _snprintf_s(probe, MAX_PATH, _TRUNCATE, "%sBioshockVR.log", dir);

    HANDLE h = CreateFileA(probe, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    char real[MAX_PATH] = {};
    const DWORD n = GetFinalPathNameByHandleA(h, real, MAX_PATH, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= MAX_PATH) return false;

    // The returned path is \\?\ prefixed; skip that before comparing.
    const char* r = real;
    if (strncmp(r, "\\\\?\\", 4) == 0) r += 4;
    if (_stricmp(r, probe) != 0) return false;      // redirected -- not ours

    strncpy_s(out, outSz, probe, _TRUNCATE);
    return true;
}

static void InitLogPath()
{
    char gameDir[MAX_PATH] = {};

    if (GetModuleFileNameA(
        g_hSelf,
        gameDir,
        MAX_PATH) == 0)
    {
        g_logPath[0] = 0;
        return;
    }

    char* slash = strrchr(gameDir, '\\');

    if (!slash)
    {
        g_logPath[0] = 0;
        return;
    }

    // Keep the trailing slash.
    *(slash + 1) = 0;

    // BioshockVR.ini remains beside BioshockHD.exe.
    _snprintf_s(
        g_iniPath,
        MAX_PATH,
        _TRUNCATE,
        "%sBioshockVR.ini",
        gameDir);

    // First choice:
    // <game folder>\logs\BioshockVR.log
    char logDir[MAX_PATH] = {};

    _snprintf_s(
        logDir,
        MAX_PATH,
        _TRUNCATE,
        "%slogs\\",
        gameDir);

    CreateDirectoryA(logDir, nullptr);

    if (DirTakesOurLog(
        logDir,
        g_logPath,
        MAX_PATH))
    {
        return;
    }

    // Fallback:
    // %LOCALAPPDATA%\BioshockVR\logs\BioshockVR.log
    //
    // This is only used when the game folder is not writable.
    char localAppData[MAX_PATH] = {};

    if (GetEnvironmentVariableA(
        "LOCALAPPDATA",
        localAppData,
        MAX_PATH) > 0 &&
        localAppData[0])
    {
        char fallbackBase[MAX_PATH] = {};

        _snprintf_s(
            fallbackBase,
            MAX_PATH,
            _TRUNCATE,
            "%s\\BioshockVR",
            localAppData);

        CreateDirectoryA(
            fallbackBase,
            nullptr);

        char fallbackLogs[MAX_PATH] = {};

        _snprintf_s(
            fallbackLogs,
            MAX_PATH,
            _TRUNCATE,
            "%s\\logs\\",
            fallbackBase);

        CreateDirectoryA(
            fallbackLogs,
            nullptr);

        if (DirTakesOurLog(
            fallbackLogs,
            g_logPath,
            MAX_PATH))
        {
            g_logRelocated = true;
            return;
        }
    }

    // Logging failed everywhere. The mod still runs.
    g_logPath[0] = 0;
}

void LogFile(const char* msg)
{
    if (!msg || !g_logPath[0])
        return;

    EnterCriticalSection(&g_logLock);

    FILE* file = nullptr;

    if (fopen_s(
        &file,
        g_logPath,
        "a") == 0 &&
        file)
    {
        SYSTEMTIME time = {};
        GetLocalTime(&time);

        fprintf(
            file,
            "[%02u:%02u:%02u.%03u] %s\n",
            time.wHour,
            time.wMinute,
            time.wSecond,
            time.wMilliseconds,
            msg);

        fclose(file);
    }

    LeaveCriticalSection(&g_logLock);
}

static void Log(const char* format, ...)
{
    char message[1024] = {};

    va_list args;
    va_start(args, format);

    _vsnprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        format,
        args);

    va_end(args);

    LogFile(message);
}


// ============================================================================
//  INIT
// ============================================================================
// Real init happens off the loader lock. DllMain is not a safe place to
// allocate, hook, or touch other modules.
static DWORD WINAPI InitThread(LPVOID)
{
    Log("=== BioshockVR ===");
    // Bump this on every release. It is the first thing to check on any log a
    // stranger sends you -- "which build is this?" has already cost one round
    // trip in this project, and __DATE__/__TIME__ alone cannot answer it.
    Log("BioshockVR version: 1.0.3");
    Log("dllmain build: M7-S4 motion arms + bathysphere  (%s %s)",
        __DATE__, __TIME__);

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* exeName = strrchr(exe, '\\');
    exeName = exeName ? exeName + 1 : exe;

    Log("host process : %s", exeName);

    if (g_logRelocated)
    {
        Log("log path     : %s", g_logPath);
        Log("log NOTE     : the game folder is not writable, so this log was");
        Log("log NOTE     : written to LocalAppData instead. That is expected on");
        Log("log NOTE     : a Program Files install without admin rights.");
    }

    Log("pointer size : %u bytes  (4 == x86, correct)", (unsigned)sizeof(void*));
    Log("dll base     : 0x%08X", (unsigned)(uintptr_t)g_hSelf);

    MH_STATUS s = MH_Initialize();
    Log("MH_Initialize -> %d  (0 == MH_OK)", (int)s);
    if (s != MH_OK) { Log("!!! MinHook dead. Stopping."); return 0; }

    Config_Load(g_iniPath);
    SyncGameIni();

    if (!Hooks_Install())
    {
        Log("!!! Hooks_Install FAILED. No hook installed. Game will run clean.");
        return 0;
    }

    Log("Init done. Present hook armed.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hSelf = hModule;

        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&g_logLock);
        InitLogPath();

        // Truncate any log from a previous run, so what we read is this run only.
        if (g_logPath[0])
        {
            FILE* f = nullptr;
            if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);
        }

        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}