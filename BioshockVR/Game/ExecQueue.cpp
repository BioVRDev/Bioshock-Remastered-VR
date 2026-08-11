// BioshockVR/Game/ExecQueue.cpp
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "Game/ExecQueue.h"
#include "Game/EngineExec.h"
#include "Core/Config.h"

// Per-file, matching every other translation unit here: LogFile is the one
// shared symbol and Log is a local varargs wrapper over it.
extern void LogFile(const char* msg);

static void Log(const char* fmt, ...)
{
    char b[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

// ---- the ring --------------------------------------------------------------
// FIXED SIZE, NO ALLOCATION. This is written from the XR thread, and a heap
// allocation there is a lock the frame loop does not need. 16 slots of 192 bytes
// is 3 KB of BSS -- cheaper than one texture and it can never fragment.
//
// A console command longer than 191 characters would be a `set` on a very long
// property path; none of the planned callers come close, and truncating one
// silently would issue a DIFFERENT command than intended, so an over-long string
// is refused and logged instead.
static const int  kExecRing = 16;
static const int  kExecLen = 192;

static char          g_ring[kExecRing][kExecLen] = {};
static volatile LONG g_head = 0;      // next slot to write   (producers)
static volatile LONG g_tail = 0;      // next slot to execute (game thread)
static volatile LONG g_dropped = 0;

// ONE LOCK, HELD ONLY FOR THE memcpy. A critical section rather than a lockless
// ring because the producers are rare (a holster draw, a startup burst) and the
// contention is therefore zero in practice -- and a wrong lockless ring is a bug
// that shows up once a month and cannot be reproduced.
static CRITICAL_SECTION g_cs;
static bool             g_csReady = false;

static void EnsureCs()
{
    // Race-free because the first caller is the game thread during startup,
    // long before any XR thread exists. Documented rather than defended: an
    // InitOnce here would imply the ordering is uncertain, and it is not.
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

bool ExecQueue_Post(const char* command)
{
    if (!command || !*command) return false;

    const size_t n = strlen(command);
    if (n >= (size_t)kExecLen)
    {
        Log(">>> EXECQ: refused a %u-char command (max %d). Not truncating -- a "
            "truncated command is a DIFFERENT command.",
            (unsigned)n, kExecLen - 1);
        return false;
    }

    EnsureCs();
    EnterCriticalSection(&g_cs);

    const LONG head = g_head;
    const LONG used = head - g_tail;
    bool ok = false;

    if (used < kExecRing)
    {
        memcpy(g_ring[head % kExecRing], command, n + 1);
        g_head = head + 1;
        ok = true;
    }
    else
    {
        ++g_dropped;
    }

    LeaveCriticalSection(&g_cs);

    if (!ok)
        Log(">>> EXECQ: ring full (%d), dropped '%s'", kExecRing, command);

    return ok;
}

void ExecQueue_Tick()
{
    if (g_tail == g_head) return;      // the common case, no lock taken

    EnsureCs();

    char cmd[kExecLen];
    bool have = false;

    EnterCriticalSection(&g_cs);
    if (g_tail != g_head)
    {
        memcpy(cmd, g_ring[g_tail % kExecRing], kExecLen);
        g_tail = g_tail + 1;
        have = true;
    }
    LeaveCriticalSection(&g_cs);

    if (!have) return;

    // The engine's own answer, not ours. EngineExec_Run returns true only when
    // the engine reports it HANDLED the command, so a typo'd property name shows
    // up here as `not handled` rather than as silence.
    const bool handled = EngineExec_Run(cmd);
    Log(">>> EXECQ: %s  -> %s", cmd, handled ? "handled" : "NOT handled");
}

void ExecQueue_RunStartupCommands()
{
    // ONE SHOT. Latched even on failure: the ini's commands are a deliberate
    // experiment the tester set up for this launch, and re-firing them every
    // frame after a failure would bury the log and hammer the engine.
    static bool fired = false;
    if (fired) return;

    int posted = 0;
    for (int i = 0; i < kExecCommands; ++i)
    {
        const char* c = g_cfg.execCommand[i];
        if (!c[0]) continue;
        if (ExecQueue_Post(c)) ++posted;
    }

    fired = true;

    if (posted)
        Log(">>> EXECQ: queued %d startup command(s) from the ini", posted);
}
