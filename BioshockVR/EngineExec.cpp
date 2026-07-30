// BioshockVR/EngineExec.cpp
//
// See EngineExec.h for why this file breaks the project's no-hardcoded-offsets
// rule and what guards that decision.
//
// The call we are making, in the engine's own terms:
//
//     UBOOL UGameEngine::Exec(const TCHAR* Cmd, FOutputDevice& Ar)
//
// __thiscall, so `this` arrives in ECX and the two arguments are on the stack.
// __fastcall with a dummy EDX parameter is the standard way to express that from
// C++ without inline assembly.
//
// `this` is NOT the UGameEngine pointer: Exec lives on the FExec subobject, so
// the pointer is adjusted by EngineExecThis (0x40) first. Passing the unadjusted
// pointer is the classic way to make this fault.

#include "EngineExec.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

extern void LogFile(const char* msg);

// From dllmain.cpp -- all four ini-overridable, see EngineExec.h.
extern int g_cfgEngPtrRva;
extern int g_cfgEngVtRva;
extern int g_cfgEngExecRva;
extern int g_cfgEngExecThis;

static_assert(sizeof(void*) == 4, "BioShock Remastered is x86; build Win32.");

static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

typedef int(__fastcall* EngineExecFn)(void* execThis, void* edx,
    const wchar_t* cmd, void* outputDevice);

// A do-nothing FOutputDevice. The engine may call into it to report the result
// of the command; every slot returns zero and pops the same 8 bytes, which is
// what the inspected build expects. A `set` that succeeds typically never
// touches it at all.
static int __stdcall OutputNoop(void*, int) { return 0; }

static void* g_outVt[24] = {};
static void* g_outObj = nullptr;

static void EnsureOutputStub()
{
    if (g_outObj) return;
    for (int i = 0; i < 24; ++i) g_outVt[i] = (void*)&OutputNoop;
    static void* obj = nullptr;
    obj = (void*)g_outVt;      // the object is just a pointer to its vtable
    g_outObj = &obj;
}

static bool IsExecutable(const void* addr)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
        p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// Resolves the FExec `this` pointer, or null. Every failure is logged once so a
// user on a different build can see WHY the reticle did not disable.
static void* ResolveExecThis()
{
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!base || g_cfgEngPtrRva <= 0 || g_cfgEngVtRva <= 0) return nullptr;

    __try
    {
        uint8_t* engine = *(uint8_t**)(base + (unsigned)g_cfgEngPtrRva);
        if (!engine)
        {
            // This used to return SILENTLY. The engine pointer is legitimately
            // null for the first seconds of the process, so a single miss means
            // nothing -- but if it is STILL null after ~30 seconds of retries,
            // EnginePtrRva is simply not the UGameEngine pointer on this build.
            // MEASURED: that is exactly what happens on the Epic Games Store
            // version, whose data sections sit ~3 KB below the Steam version's.
            // Without this the reticle never dies and the log says nothing at
            // all, which is the most expensive kind of failure to support.
            static int misses = 0;
            if (++misses == 15)
            {
                Log("!!! exec: EnginePtrRva (module+0x%X) has read NULL 15 times.",
                    (unsigned)g_cfgEngPtrRva);
                Log("!!! exec: That address is not the UGameEngine pointer on this");
                Log("!!! exec: build -- most likely a different STORE version of the");
                Log("!!! exec: game (Epic vs Steam). The stock crosshair will stay.");
                Log("!!! exec: Set EnginePtrRva / EngineVtableRva / EngineExecRva in");
                Log("!!! exec: BioshockVR.ini, or DisableReticle=0 to silence this.");
            }
            return nullptr;
        }

        void* actualVt = *(void**)engine;
        void* expectVt = base + (unsigned)g_cfgEngVtRva;

        if (actualVt != expectVt)
        {
            static bool once = false;
            if (!once)
            {
                once = true;
                Log("!!! exec: UGameEngine vtable mismatch (actual %p, expected %p).",
                    actualVt, expectVt);
                Log("!!! exec: this is a DIFFERENT BUILD of the game. Set");
                Log("!!! exec: EnginePtrRva / EngineVtableRva / EngineExecRva in");
                Log("!!! exec: BioshockVR.ini, or DisableReticle=0 to silence this.");
            }
            return nullptr;
        }
        return engine + (unsigned)g_cfgEngExecThis;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static bool once = false;
        if (!once) { once = true; Log("!!! exec: fault resolving UGameEngine."); }
        return nullptr;
    }
}

bool EngineExec_Run(const char* command)
{
    if (!command || !command[0]) return false;

    EnsureOutputStub();

    void* execThis = ResolveExecThis();
    if (!execThis) return false;

    // ASCII -> wide by hand. The commands we issue are all plain ASCII, and this
    // avoids dragging in a locale-dependent conversion on the game thread.
    wchar_t wide[224] = {};
    size_t n = 0;
    while (n < 223 && command[n])
    {
        wide[n] = (wchar_t)(unsigned char)command[n];
        ++n;
    }
    while (n > 0 && (wide[n - 1] == L' ' || wide[n - 1] == L'\r' || wide[n - 1] == L'\n'))
        --n;
    wide[n] = L'\0';
    if (!n) return false;

    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    EngineExecFn fn = (EngineExecFn)(base + (unsigned)g_cfgEngExecRva);

    if (!IsExecutable((void*)fn))
    {
        static bool once = false;
        if (!once) { once = true; Log("!!! exec: EngineExecRva is not executable code."); }
        return false;
    }

    int result = 0;
    __try
    {
        result = fn(execThis, nullptr, wide, g_outObj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("!!! exec: engine faulted running '%ls'.", wide);
        return false;
    }

    Log("exec: '%ls' -> %s", wide, result ? "HANDLED" : "unhandled");
    return result != 0;
}
