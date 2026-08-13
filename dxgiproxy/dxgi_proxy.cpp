// dxgi_proxy.cpp
//
// A LOADER, not a mod. This exists only so that dropping two files next to
// BioshockHD.exe is enough to run the mod -- no plugin folder, no injector.
//
// HOW IT WORKS
//   The game imports CreateDXGIFactory1 from "DXGI.dll". Windows looks in the
//   executable's own directory before the system directory, so a file named
//   dxgi.dll sitting next to the exe wins. The game calls into us, we make sure
//   BioshockVR.dll is loaded, then hand the call straight to the real system
//   DXGI and get out of the way.
//
// WHY THE MOD IS LOADED HERE AND NOT IN DllMain
//   DllMain runs under the loader lock, and calling LoadLibrary from inside it
//   is a documented way to deadlock. The first DXGI export call happens long
//   after the lock is released and still comfortably before the game creates
//   its swapchain -- which is earlier than the mod's hooks need to be in place.
//   So the safe moment and the correct moment are the same moment.
//
// WHY AN ABSOLUTE PATH TO THE REAL DXGI
//   LoadLibraryW(L"dxgi.dll") from in here would find US again. The real one is
//   loaded from the system directory by full path, every time.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>

static HMODULE   g_realDxgi = nullptr;
static HMODULE   g_mod = nullptr;
static HMODULE   g_self = nullptr;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

typedef HRESULT(WINAPI* CreateFactoryFn)(REFIID, void**);
typedef HRESULT(WINAPI* CreateFactory2Fn)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* DeclareRemovalFn)();
typedef HRESULT(WINAPI* GetDebugFn)(UINT, REFIID, void**);

static CreateFactoryFn  g_createFactory = nullptr;
static CreateFactoryFn  g_createFactory1 = nullptr;
static CreateFactory2Fn g_createFactory2 = nullptr;
static DeclareRemovalFn g_declareRemoval = nullptr;
static GetDebugFn       g_getDebug = nullptr;

static void Say(const wchar_t* msg)
{
    OutputDebugStringW(L"[BioshockVR loader] ");
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\n");
}

// Writes a breadcrumb into <game>\logs\. If the mod ever fails to appear, this
// file existing tells you the proxy WAS loaded and the problem is downstream --
// which is the single most useful thing to know when nothing happens at all.
//
// It goes in logs\ so every log this project produces lives in one folder and a
// support bundle is one directory. But that is a convenience, and this file's
// whole value is that it appears when NOTHING else does -- so if the folder
// cannot be created we fall back to sitting beside the proxy rather than
// writing nothing at all. A breadcrumb that can fail is not a breadcrumb.
//
// Appends rather than truncates, unlike the mod's own log: across runs this is
// a history of whether the proxy ever loaded, which is exactly the question it
// exists to answer.
static void Breadcrumb(const wchar_t* dir, const wchar_t* text)
{
    wchar_t path[MAX_PATH] = {};
    wcscpy_s(path, dir);
    wcscat_s(path, L"logs");

    // CreateDirectory failing because it already exists is the normal case.
    if (CreateDirectoryW(path, nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS)
    {
        wcscat_s(path, L"\\BioshockVR_loader.log");
    }
    else
    {
        wcscpy_s(path, dir);
        wcscat_s(path, L"BioshockVR_loader.log");
    }

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    char line[512];
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, line,
        (int)sizeof(line) - 2, nullptr, nullptr);
    if (n > 1)
    {
        line[n - 1] = '\n';
        DWORD written = 0;
        WriteFile(h, line, (DWORD)n, &written, nullptr);
    }
    CloseHandle(h);
}

static BOOL CALLBACK InitOnce(PINIT_ONCE, PVOID, PVOID*)
{
    // ---- our own directory, for both the breadcrumb and the mod --------
    wchar_t selfDir[MAX_PATH] = {};
    GetModuleFileNameW(g_self, selfDir, MAX_PATH);
    wchar_t* slash = wcsrchr(selfDir, L'\\');
    if (slash) *(slash + 1) = 0; else selfDir[0] = 0;

    // ---- the REAL dxgi, by absolute system path ------------------------
    wchar_t sys[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(sys, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 16)
    {
        Say(L"system directory lookup failed");
        Breadcrumb(selfDir, L"FAIL: system directory lookup");
        return TRUE;
    }
    wcscat_s(sys, L"\\dxgi.dll");

    g_realDxgi = LoadLibraryW(sys);
    if (!g_realDxgi)
    {
        Say(L"could not load the real system dxgi.dll");
        Breadcrumb(selfDir, L"FAIL: could not load system dxgi.dll");
        return TRUE;
    }

    g_createFactory = (CreateFactoryFn)GetProcAddress(g_realDxgi, "CreateDXGIFactory");
    g_createFactory1 = (CreateFactoryFn)GetProcAddress(g_realDxgi, "CreateDXGIFactory1");
    g_createFactory2 = (CreateFactory2Fn)GetProcAddress(g_realDxgi, "CreateDXGIFactory2");
    g_declareRemoval = (DeclareRemovalFn)GetProcAddress(g_realDxgi, "DXGIDeclareAdapterRemovalSupport");
    g_getDebug = (GetDebugFn)GetProcAddress(g_realDxgi, "DXGIGetDebugInterface1");

    // ---- the mod, by absolute path beside us ---------------------------
    wchar_t modPath[MAX_PATH] = {};
    wcscpy_s(modPath, selfDir);
    wcscat_s(modPath, L"BioshockVR.dll");

    g_mod = LoadLibraryW(modPath);
    if (g_mod)
    {
        Say(L"BioshockVR.dll loaded");
        Breadcrumb(selfDir, L"OK: real dxgi + BioshockVR.dll loaded");
    }
    else
    {
        // ---- SAY WHY, BECAUSE "NOT FOUND" IS USUALLY A LIE --------------
        // This branch fires on ANY LoadLibrary failure, and a genuinely missing
        // file is the LEAST likely of them -- the message sent people hunting
        // for a DLL sitting right there in the folder.
        //
        // The realistic causes are all distinguishable for free, and one of them
        // is our own bug rather than the user's: BioshockVR.dll statically
        // imports openxr_loader.dll, so calling any xr* function the active
        // loader does not export makes Windows refuse to load the mod. That has
        // happened twice in this project.
        const DWORD e = GetLastError();
        const wchar_t* why =
            (e == ERROR_MOD_NOT_FOUND)   ? L"FAIL: BioshockVR.dll could not load -- a DEPENDENCY is missing (126). Usually openxr_loader.dll or openvr_api.dll is absent from this folder."
          : (e == ERROR_PROC_NOT_FOUND)  ? L"FAIL: BioshockVR.dll could not load -- a required EXPORT is missing (127). The mod and openxr_loader.dll are mismatched versions; reinstall the package as a set."
          : (e == ERROR_BAD_EXE_FORMAT)  ? L"FAIL: BioshockVR.dll could not load -- WRONG BITNESS (193). This is a 64-bit DLL in a 32-bit game; get the x86 package."
          : (e == ERROR_FILE_NOT_FOUND)  ? L"FAIL: BioshockVR.dll is genuinely missing from this folder (2)."
          : (e == ERROR_ACCESS_DENIED)   ? L"FAIL: BioshockVR.dll could not load -- ACCESS DENIED (5). Antivirus may have quarantined it."
                                         : L"FAIL: BioshockVR.dll could not load. See the error code on the next line.";
        Say(L"BioshockVR.dll failed to load");
        Breadcrumb(selfDir, why);

        wchar_t code[128] = {};
        _snwprintf_s(code, _TRUNCATE, L"FAIL: LoadLibraryW GetLastError = %lu", e);
        Breadcrumb(selfDir, code);
    }
    return TRUE;
}

static bool Ready()
{
    InitOnceExecuteOnce(&g_once, InitOnce, nullptr, nullptr);
    return g_realDxgi != nullptr;
}

static HRESULT NoExport() { return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND); }

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** out)
{
    if (!Ready() || !g_createFactory) return NoExport();
    return g_createFactory(riid, out);
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** out)
{
    if (!Ready() || !g_createFactory1) return NoExport();
    return g_createFactory1(riid, out);
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    if (!Ready() || !g_createFactory2) return NoExport();
    return g_createFactory2(flags, riid, out);
}

extern "C" HRESULT WINAPI Proxy_DXGIDeclareAdapterRemovalSupport()
{
    if (!Ready() || !g_declareRemoval) return NoExport();
    return g_declareRemoval();
}

extern "C" HRESULT WINAPI Proxy_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** out)
{
    if (!Ready() || !g_getDebug) return NoExport();
    return g_getDebug(flags, riid, out);
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        // Nothing else here. See the note at the top of the file.
    }
    return TRUE;
}
