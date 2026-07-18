// BioshockVR/DrawHook.cpp
//
// FINGERPRINT FIRST, SUPPRESS SECOND.
//
// The HUD, the reticle, the pause menu and the coronas are all just draw calls.
// We cannot suppress what we cannot name, and we cannot name anything by
// guessing -- so this file starts by doing NOTHING except counting.
//
// Every DrawIndexed(n) and Draw(n) is bucketed by n.
//
//   Numpad 3  dump the table
//   Numpad *  CLEAR the table   <-- the important one, see below
//   Numpad 1  toggle suppression of SuppressIndexCounts
//
// HOW TO SAMPLE CORRECTLY (learned the hard way, §13):
// A cumulative dump tells you a count was never seen BEFORE now. It does NOT
// tell you the count belongs to whatever is on screen now. Diffing two
// cumulative dumps once produced six "menu" counts that were really particles
// coming into view on a turn -- the detector then fired ~10 times a second
// during normal play and threw the whole frame onto the menu quad.
//
// So: CLEAR (Numpad *) with the thing you want to sample already on screen,
// wait a second, then DUMP (Numpad 3). Everything in that dump was drawn during
// that window and nothing else.
//
// SAFETY: with an empty suppress list and an empty menu list this file changes
// NOTHING about what the game renders. It is a pure observer until told
// otherwise.

#include "DrawHook.h"

#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#include <MinHook.h>

extern void LogFile(const char* msg);
extern bool g_cfgDrawHook;
extern char g_cfgSuppressList[256];
extern char g_cfgMenuList[256];

static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

typedef void(__stdcall* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(__stdcall* DrawFn)(ID3D11DeviceContext*, UINT, UINT);

static DrawIndexedFn g_origDrawIndexed = nullptr;
static DrawFn        g_origDraw = nullptr;
static void* g_addrDrawIndexed = nullptr;
static void* g_addrDraw = nullptr;

// ---- the fingerprint table ------------------------------------------------
struct Bucket
{
    unsigned count;        // IndexCount / VertexCount
    unsigned long long total;
    unsigned perFrame;     // calls in the frame just ended
    unsigned maxPerFrame;
    unsigned framesSeen;
    bool     indexed;
};

static const int kMaxBuckets = 256;
static Bucket   g_buckets[kMaxBuckets];
static int      g_bucketCount = 0;
static unsigned long long g_frames = 0;

// ---- suppression ----------------------------------------------------------
static unsigned g_suppress[32];
static int      g_suppressCount = 0;
static bool     g_suppressOn = false;   // Numpad 1. OFF until you ask for it.
static unsigned long long g_suppressed = 0;

// ---- menu detection -------------------------------------------------------
// ALL configured counts must appear in the SAME frame before we call it a menu.
// ANY-match is not selective enough: small counts (21, 63, 87...) are shared
// with particles and decals, and one stray match flipped the layer mid-turn.
static unsigned g_menuCounts[16];
static int      g_menuCountN = 0;
static unsigned g_menuMask = 0;         // bit i == count i seen this frame
static int      g_menuHits = 0;
static int      g_menuMiss = 0;
static bool     g_menuUp = false;

bool DrawHook_MenuUp() { return g_menuUp; }

static Bucket* FindBucket(unsigned count, bool indexed)
{
    for (int i = 0; i < g_bucketCount; ++i)
        if (g_buckets[i].count == count && g_buckets[i].indexed == indexed)
            return &g_buckets[i];

    if (g_bucketCount >= kMaxBuckets) return nullptr;
    Bucket* b = &g_buckets[g_bucketCount++];
    b->count = count;
    b->indexed = indexed;
    b->total = 0;
    b->perFrame = 0;
    b->maxPerFrame = 0;
    b->framesSeen = 0;
    return b;
}

static bool IsSuppressed(unsigned count)
{
    if (!g_suppressOn) return false;
    for (int i = 0; i < g_suppressCount; ++i)
        if (g_suppress[i] == count) return true;
    return false;
}

static void NoteMenuCount(unsigned count)
{
    for (int i = 0; i < g_menuCountN; ++i)
        if (g_menuCounts[i] == count) { g_menuMask |= (1u << i); return; }
}

static void DumpTable()
{
    Log("=== DRAW FINGERPRINT DUMP  (%llu frames) ===", g_frames);
    Log("  count   kind      total     per-frame(max)  frames");

    int order[kMaxBuckets];
    for (int i = 0; i < g_bucketCount; ++i) order[i] = i;

    for (int i = 0; i < g_bucketCount; ++i)
        for (int j = i + 1; j < g_bucketCount; ++j)
        {
            const Bucket& a = g_buckets[order[i]];
            const Bucket& b = g_buckets[order[j]];
            const bool swap = (b.maxPerFrame < a.maxPerFrame) ||
                (b.maxPerFrame == a.maxPerFrame && b.framesSeen > a.framesSeen);
            if (swap) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }

    int shown = 0;
    for (int i = 0; i < g_bucketCount && shown < 48; ++i)
    {
        const Bucket& b = g_buckets[order[i]];
        const bool candidate = (b.maxPerFrame <= 4) &&
            (g_frames && b.framesSeen * 100 / g_frames > 50);
        Log("  %5u   %-8s  %8llu   %3u            %u  %s",
            b.count, b.indexed ? "Indexed" : "Draw",
            b.total, b.maxPerFrame, b.framesSeen,
            candidate ? "  <-- UI CANDIDATE" : "");
        ++shown;
    }
    Log("=== %d distinct counts, %llu suppressed so far ===",
        g_bucketCount, g_suppressed);
}

static void PollKeys()
{
    static bool k1 = false, k3 = false, kM = false;

    // Numpad * : CLEAR. Open the thing you want to sample, press *, wait, dump.
    const bool dM = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
    if (dM && !kM)
    {
        g_bucketCount = 0;
        g_frames = 0;
        Log(">>> DRAWHOOK: table CLEARED");
    }
    kM = dM;

    const bool d3 = (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) != 0;
    if (d3 && !k3) DumpTable();
    k3 = d3;

    const bool d1 = (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;
    if (d1 && !k1)
    {
        g_suppressOn = !g_suppressOn;
        Log(">>> DRAW SUPPRESSION %s  (%d counts in list)",
            g_suppressOn ? "ON" : "off", g_suppressCount);
    }
    k1 = d1;
}

static void __stdcall hkDrawIndexed(ID3D11DeviceContext* ctx,
    UINT IndexCount, UINT StartIndex, INT BaseVertex)
{
    Bucket* b = FindBucket(IndexCount, true);
    if (b) { ++b->total; ++b->perFrame; }

    NoteMenuCount(IndexCount);

    if (IsSuppressed(IndexCount)) { ++g_suppressed; return; }   // draw skipped

    g_origDrawIndexed(ctx, IndexCount, StartIndex, BaseVertex);
}

static void __stdcall hkDraw(ID3D11DeviceContext* ctx,
    UINT VertexCount, UINT StartVertex)
{
    Bucket* b = FindBucket(VertexCount, false);
    if (b) { ++b->total; ++b->perFrame; }

    NoteMenuCount(VertexCount);

    if (IsSuppressed(VertexCount)) { ++g_suppressed; return; }

    g_origDraw(ctx, VertexCount, StartVertex);
}

void DrawHook_EndFrame()
{
    ++g_frames;
    for (int i = 0; i < g_bucketCount; ++i)
    {
        Bucket& b = g_buckets[i];
        if (b.perFrame)
        {
            ++b.framesSeen;
            if (b.perFrame > b.maxPerFrame) b.maxPerFrame = b.perFrame;
            b.perFrame = 0;
        }
    }

    // ALL configured counts must have appeared in this one frame.
    const unsigned want = (g_menuCountN > 0) ? ((1u << g_menuCountN) - 1u) : 0u;
    const bool all = (g_menuCountN > 0) && (g_menuMask == want);
    g_menuMask = 0;

    if (all) { ++g_menuHits; g_menuMiss = 0; }
    else { ++g_menuMiss; g_menuHits = 0; }

    // Hysteresis both ways (~8 frames == ~34ms at 236 Present/s).
    if (!g_menuUp && g_menuHits >= 8)
    {
        g_menuUp = true;
        Log(">>> DRAWHOOK: MENU DETECTED");
    }
    else if (g_menuUp && g_menuMiss >= 8)
    {
        g_menuUp = false;
        Log(">>> DRAWHOOK: menu closed");
    }

    PollKeys();
}

static int ParseCounts(const char* s, unsigned* out, int maxOut)
{
    int n = 0;
    while (*s && n < maxOut)
    {
        while (*s == ' ' || *s == ',') ++s;
        if (!*s) break;
        out[n++] = (unsigned)strtoul(s, nullptr, 10);
        while (*s && *s != ',') ++s;
    }
    return n;
}

static void ParseConfigLists()
{
    g_suppressCount = ParseCounts(g_cfgSuppressList, g_suppress, 32);
    if (g_suppressCount)
    {
        Log("drawhook: %d suppress counts loaded (Numpad 1 to enable):",
            g_suppressCount);
        for (int i = 0; i < g_suppressCount; ++i)
            Log("   suppress count %u", g_suppress[i]);
    }
    else
    {
        Log("drawhook: suppress list EMPTY. Observing only, nothing hidden.");
    }

    g_menuCountN = ParseCounts(g_cfgMenuList, g_menuCounts, 16);
    if (g_menuCountN)
    {
        Log("drawhook: %d menu-detect counts (ALL must hit in one frame):",
            g_menuCountN);
        for (int i = 0; i < g_menuCountN; ++i)
            Log("   menu count %u", g_menuCounts[i]);
    }
    else
    {
        Log("drawhook: menu list EMPTY. Menu detection OFF.");
    }
}

bool DrawHook_Install(void* pDrawIndexed, void* pDraw)
{
    if (!g_cfgDrawHook)
    {
        Log("drawhook: DISABLED by ini (EnableDrawHook=0).");
        return false;
    }
    if (!pDrawIndexed || !pDraw)
    {
        Log("!!! drawhook: null vtable slots. Not installing.");
        return false;
    }

    ParseConfigLists();

    MH_STATUS s = MH_CreateHook(pDrawIndexed, &hkDrawIndexed,
        (LPVOID*)&g_origDrawIndexed);
    if (s != MH_OK) { Log("!!! MH_CreateHook(DrawIndexed) -> %d", (int)s); return false; }

    s = MH_CreateHook(pDraw, &hkDraw, (LPVOID*)&g_origDraw);
    if (s != MH_OK) { Log("!!! MH_CreateHook(Draw) -> %d", (int)s); return false; }

    if (MH_EnableHook(pDrawIndexed) != MH_OK) { Log("!!! MH_EnableHook(DrawIndexed)"); return false; }
    if (MH_EnableHook(pDraw) != MH_OK) { Log("!!! MH_EnableHook(Draw)"); return false; }

    g_addrDrawIndexed = pDrawIndexed;
    g_addrDraw = pDraw;

    Log(">>> DRAW HOOK ARMED. Numpad 3 = dump, Numpad * = clear, Numpad 1 = suppress.");
    return true;
}

void DrawHook_Remove()
{
    if (g_addrDrawIndexed) MH_DisableHook(g_addrDrawIndexed);
    if (g_addrDraw)        MH_DisableHook(g_addrDraw);
    g_addrDrawIndexed = g_addrDraw = nullptr;
}