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

#include "Game/EngineExec.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include "Core/Config.h"

extern void LogFile(const char* msg);

// From dllmain.cpp -- all four ini-overridable, see EngineExec.h.

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
// ---- OUTPUT CAPTURE ------------------------------------------------------
// The engine reports command results through FOutputDevice::Serialize, which
// on x86 __thiscall is (this in ECX, const TCHAR* V, EName Event) -- two stack
// args, 8 bytes, callee-cleaned. That is EXACTLY the shape the old no-op stub
// already had, so pointing the slots at a capturing function of the same
// signature adds no new stack risk: it just stops throwing the text away.
//
// This is what makes `GET` usable. `SET` never printed anything, so a no-op
// device was fine; reading a property back requires actually keeping the text.
static wchar_t g_outBuf[512] = {};
static bool    g_outGot = false;
static int     g_outCalls = 0;   // did the engine touch our device AT ALL?
static void* g_outFirstArg = nullptr;

// MEASURED: calls=1 per `get`, firstArg=0x2F8 -- a small constant, not a
// pointer, and the same for every command. So the string is NOT the first stack
// argument; 0x2F8 is almost certainly the EName. Take four slots and find the
// one that actually points at readable wide text.
// MEASURED from the slot dump: (EName 0x2F8, flag 1, const TCHAR* text, ret).
// The text is the THIRD stack argument. Three args = 12 bytes popped, which is
// what the engine actually pushes -- the 16-byte version faulted every call.
// 8 bytes is the PROVEN pop size -- 12 and 16 both faulted, 8 did not. So the
// engine pushes exactly two stack args and neither holds the text, which means
// the slot being called is not the one we assumed. We fill all 24 slots with
// the same stub, so we have never known WHICH. One thunk per slot fixes that.
static int g_outSlot = -1;

static int __stdcall OutputCapture(void* a0, void* a1)
{
    ++g_outCalls;
    if (!g_outFirstArg) g_outFirstArg = a0;
    Log("exec: device SLOT %d  args %08X %08X", g_outSlot,
        (unsigned)(uintptr_t)a0, (unsigned)(uintptr_t)a1);

    void* text = nullptr;
    if (a0 && (uintptr_t)a0 > 0x10000 && !IsBadReadPtr(a0, 4)) text = a0;
    else if (a1 && (uintptr_t)a1 > 0x10000 && !IsBadReadPtr(a1, 4)) text = a1;

    if (text)

    if (text && !IsBadReadPtr(text, sizeof(wchar_t)))
    {
        __try
        {
            const wchar_t* s = (const wchar_t*)text;
            size_t n = 0;
            while (n < 511 && s[n]) { g_outBuf[n] = s[n]; ++n; }
            g_outBuf[n] = 0;
            g_outGot = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // MEASURED: slot 4 is called once per `get` with the SAME two args
    // (0x2F8, 1) no matter which property we ask for -- so it is not carrying
    // our text. It is invariant, which is the shape of a QUERY, not a write.
    // We have been answering 0 to it. If it means "will you accept output?",
    // 0 is us declining and the text never gets written. Say yes and see.
    return 1;
}

bool EngineExec_GetLastOutput(wchar_t* out, size_t chars)
{
    if (!out || !chars || !g_outGot) return false;
    size_t n = 0;
    while (n < chars - 1 && g_outBuf[n]) { out[n] = g_outBuf[n]; ++n; }
    out[n] = 0;
    return true;
}

#define MAKE_SLOT(N) static int __stdcall OutSlot##N(void* a, void* b) \
    { g_outSlot = N; return OutputCapture(a, b); }
MAKE_SLOT(0)  MAKE_SLOT(1)  MAKE_SLOT(2)  MAKE_SLOT(3)
MAKE_SLOT(4)  MAKE_SLOT(5)  MAKE_SLOT(6)  MAKE_SLOT(7)
MAKE_SLOT(8)  MAKE_SLOT(9)  MAKE_SLOT(10) MAKE_SLOT(11)
MAKE_SLOT(12) MAKE_SLOT(13) MAKE_SLOT(14) MAKE_SLOT(15)
MAKE_SLOT(16) MAKE_SLOT(17) MAKE_SLOT(18) MAKE_SLOT(19)
MAKE_SLOT(20) MAKE_SLOT(21) MAKE_SLOT(22) MAKE_SLOT(23)
#undef MAKE_SLOT

static void* const g_slotThunks[24] = {
    (void*)&OutSlot0,  (void*)&OutSlot1,  (void*)&OutSlot2,  (void*)&OutSlot3,
    (void*)&OutSlot4,  (void*)&OutSlot5,  (void*)&OutSlot6,  (void*)&OutSlot7,
    (void*)&OutSlot8,  (void*)&OutSlot9,  (void*)&OutSlot10, (void*)&OutSlot11,
    (void*)&OutSlot12, (void*)&OutSlot13, (void*)&OutSlot14, (void*)&OutSlot15,
    (void*)&OutSlot16, (void*)&OutSlot17, (void*)&OutSlot18, (void*)&OutSlot19,
    (void*)&OutSlot20, (void*)&OutSlot21, (void*)&OutSlot22, (void*)&OutSlot23,
};

static void* g_outVt[24] = {};
static void* g_outObj = nullptr;

static void EnsureOutputStub()
{
    if (g_outObj) return;
    for (int i = 0; i < 24; ++i) g_outVt[i] = g_slotThunks[i];
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
    if (!base || g_cfg.engPtrRva <= 0 || g_cfg.engVtRva <= 0) return nullptr;

    __try
    {
        uint8_t* engine = *(uint8_t**)(base + (unsigned)g_cfg.engPtrRva);
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
                    (unsigned)g_cfg.engPtrRva);
                Log("!!! exec: That address is not the UGameEngine pointer on this");
                Log("!!! exec: build -- most likely a different STORE version of the");
                Log("!!! exec: game (Epic vs Steam). The stock crosshair will stay.");
                Log("!!! exec: Set EnginePtrRva / EngineVtableRva / EngineExecRva in");
                Log("!!! exec: BioshockVR.ini, or DisableReticle=0 to silence this.");
            }
            return nullptr;
        }

        void* actualVt = *(void**)engine;
        void* expectVt = base + (unsigned)g_cfg.engVtRva;

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
        return engine + (unsigned)g_cfg.engExecThis;
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

    g_outGot = false;
    g_outBuf[0] = 0;
    g_outCalls = 0;
    g_outFirstArg = nullptr;

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
    EngineExecFn fn = (EngineExecFn)(base + (unsigned)g_cfg.engExecRva);

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

    Log("exec: '%ls' -> %s   device calls=%d firstArg=%08X%s%ls%s",
        wide, result ? "HANDLED" : "unhandled",
        g_outCalls, (unsigned)(uintptr_t)g_outFirstArg,
        g_outGot ? "   OUTPUT: [" : "",
        g_outGot ? g_outBuf : L"",
        g_outGot ? "]" : "");
    return result != 0;
}
