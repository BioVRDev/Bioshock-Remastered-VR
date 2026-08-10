// BioshockVR/GameIni.cpp
//
// ONE SOURCE OF TRUTH for FOV and resolution.
//
// The invariant that matters: what we REPORT to OpenXR (GameFovDegrees) must
// equal what the game actually RENDERS (Bioshock.ini HorizontalFOV). When they
// drifted by 10 degrees the compositor reprojected horizontal content by the
// wrong factor -- yaw warped, pitch stayed clean. Keeping them in sync by hand
// across two files in two directories is a bug generator, so we do it in code.
//
// LAUNCH ORDERING, be honest about it: the game reads its config very early,
// possibly before our InitThread runs. So a changed value may only take effect
// on the NEXT launch. SyncGameIni therefore reads the file back after writing
// and logs LOUDLY when the on-disk value disagrees with ours, which is the
// thing you actually need to know.
//
// Bioshock.ini lives under the user profile (NOT Program Files), so plain
// WritePrivateProfileString works -- none of the UAC/VirtualStore pain from S8.

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

#include "GameIni.h"
#include "Config.h"

#pragma comment(lib, "shell32.lib")

extern void  LogFile(const char* msg);

static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

// Section names, MEASURED from a real Bioshock.ini (see S12 notes):
//   HorizontalFOV, bHorizontalFOVLock -> [ShockGame.ShockUserSettings]
//   HorizontalFOVLock                 -> [Engine.RenderConfig]
//   WindowedViewportX / Y             -> [WinDrv.WindowsClient]
// NOTE: FOVAngleDegrees lives in [Editor.EditorEngine] and is the LEVEL EDITOR's
// viewport FOV. It does nothing in game. Do not "fix" it. It is a red herring.
static const char* kSecUser = "ShockGame.ShockUserSettings";
static const char* kSecRender = "Engine.RenderConfig";
static const char* kSecWin = "WinDrv.WindowsClient";

// Candidate locations for Bioshock.ini, tried in order. The first that exists
// wins. An explicit GameIniPath in BioshockVR.ini overrides all of them.
static bool FindGameIni(char* out, size_t outSz)
{
    // 1. Explicit override from our own ini, if the user set one.
    {
        char self[MAX_PATH] = {};
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&FindGameIni, &hm);
        if (GetModuleFileNameA(hm, self, MAX_PATH))
        {
            char* slash = strrchr(self, '\\');
            if (slash)
            {
                *(slash + 1) = 0;
                char ours[MAX_PATH] = {};
                _snprintf_s(ours, MAX_PATH, _TRUNCATE, "%sBioshockVR.ini", self);

                char over[MAX_PATH] = {};
                GetPrivateProfileStringA("VR", "GameIniPath", "", over, MAX_PATH, ours);
                if (over[0])
                {
                    if (GetFileAttributesA(over) != INVALID_FILE_ATTRIBUTES)
                    {
                        strncpy_s(out, outSz, over, _TRUNCATE);
                        Log("gameini: using GameIniPath override");
                        return true;
                    }
                    Log("!!! gameini: GameIniPath '%s' does not exist. Falling back.", over);
                }
            }
        }
    }

    // 2. Known layouts. Remastered and classic disagree, so try both roots.
    char appdata[MAX_PATH] = {};
    char docs[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);

    // MEASURED from real user logs, most specific first. Epic writes
    // "Bioshock Epic HD" and Steam writes "BioshockHD"; BOTH appear with and
    // without an intervening "My Games" folder, so all four combinations are
    // tried. The two original bare names stay last as a catch-all.
    const char* candidates[] = {
        "%s\\My Games\\Bioshock Epic HD\\Bioshock\\Bioshock.ini",
        "%s\\Bioshock Epic HD\\Bioshock\\Bioshock.ini",
        "%s\\My Games\\BioshockHD\\Bioshock\\Bioshock.ini",
        "%s\\BioshockHD\\Bioshock\\Bioshock.ini",
        "%s\\My Games\\Bioshock\\Bioshock.ini",
        "%s\\Bioshock\\Bioshock.ini",
        "%s\\BioShock Remastered\\Bioshock.ini",
    };

    for (int pass = 0; pass < 2; ++pass)
    {
        const char* root = (pass == 0) ? appdata : docs;
        if (!root[0]) continue;
        for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i)
        {
            char p[MAX_PATH] = {};
            _snprintf_s(p, MAX_PATH, _TRUNCATE, candidates[i], root);
            if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES)
            {
                strncpy_s(out, outSz, p, _TRUNCATE);
                return true;
            }
        }
    }
    return false;
}

static void WriteInt(const char* path, const char* sec, const char* key, int val)
{
    char v[32];
    _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", val);
    if (!WritePrivateProfileStringA(sec, key, v, path))
        Log("!!! gameini: write [%s] %s failed (err %lu)", sec, key, GetLastError());
}

void SyncGameIni()
{
    if (!g_cfg.syncGameIni)
    {
        Log("gameini: SyncGameIni=0. Leaving Bioshock.ini alone.");
        return;
    }

    char path[MAX_PATH] = {};
    if (!FindGameIni(path, MAX_PATH))
    {
        Log("!!! gameini: could not locate Bioshock.ini. Set GameIniPath= in");
        Log("!!! gameini: BioshockVR.ini to the full path, or set SyncGameIni=0.");
        return;
    }
    Log("gameini: %s", path);

    // FOV. Round to int -- HorizontalFOV is an integer key in this game.
    const int fov = (int)(g_cfg.fovDeg + 0.5f);
    WriteInt(path, kSecUser, "HorizontalFOV", fov);

    // Pin the FOV so the engine stops deriving it from aspect ratio. Without
    // these two, "110" is a suggestion and the real horizontal FOV depends on
    // the buffer shape -- which is precisely how the report/actual mismatch
    // sneaks back in on a non-16:9 buffer.
    WritePrivateProfileStringA(kSecUser, "bHorizontalFOVLock", "True", path);
    WritePrivateProfileStringA(kSecRender, "HorizontalFOVLock", "True", path);

    // Resolution. 0 means "don't touch it".
    //
    // FULLSCREEN vs WINDOWED: UE2 keeps TWO viewport sizes and reads whichever
    // mode it starts in. Writing only the Windowed pair is why setting the
    // resolution silently stopped working the moment fullscreen was enabled.
    // Both pairs get the same value, so switching modes cannot change the
    // buffer shape -- which ForegroundFovAuto derives the weapon FOV from.
    if (g_cfg.resX > 0 && g_cfg.resY > 0)
    {
        WriteInt(path, kSecWin, "WindowedViewportX", g_cfg.resX);
        WriteInt(path, kSecWin, "WindowedViewportY", g_cfg.resY);
        WriteInt(path, kSecWin, "FullscreenViewportX", g_cfg.resX);
        WriteInt(path, kSecWin, "FullscreenViewportY", g_cfg.resY);
    }

    // Fullscreen EXCLUSIVE is the only mode MEASURED to ignore the display
    // refresh cap -- windowed presents are throttled by the compositor to one
    // per composition no matter what SyncInterval says, which is why
    // DisableVSync=1 never helped. -1 leaves the game's own choice alone.
    if (g_cfg.fullscreen >= 0)
        WritePrivateProfileStringA(kSecWin, "StartupFullscreen",
            g_cfg.fullscreen ? "True" : "False", path);

    WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);   // flush cache

    // ---- READ BACK. This is the part that earns its keep. -------------------
    const int gotFov = GetPrivateProfileIntA(kSecUser, "HorizontalFOV", -1, path);
    const int gotX = GetPrivateProfileIntA(kSecWin, "WindowedViewportX", -1, path);
    const int gotY = GetPrivateProfileIntA(kSecWin, "WindowedViewportY", -1, path);

    const int gotFsX = GetPrivateProfileIntA(kSecWin, "FullscreenViewportX", -1, path);
    const int gotFsY = GetPrivateProfileIntA(kSecWin, "FullscreenViewportY", -1, path);

    Log("gameini: HorizontalFOV      = %d   (we want %d)", gotFov, fov);
    Log("gameini: WindowedViewport   = %d x %d", gotX, gotY);
    Log("gameini: FullscreenViewport = %d x %d", gotFsX, gotFsY);
    Log("gameini: NOTE -- what the ini says is only a REQUEST. Compare against");
    Log("gameini: the 'backbuffer :' line later in this log. In exclusive");
    Log("gameini: fullscreen the buffer must be a real DISPLAY MODE, so a");
    Log("gameini: non-standard size can be silently snapped to the nearest one.");

    if (gotFov != fov)
    {
        Log("!!! gameini: FOV MISMATCH. The game is rendering %d while we report", gotFov);
        Log("!!! gameini: %d to OpenXR. Turning will look warped. Relaunch once;", fov);
        Log("!!! gameini: if it persists the game is rewriting the ini at exit.");
    }
    else
    {
        Log("gameini: FOV in sync. Report == actual.");
    }
}
