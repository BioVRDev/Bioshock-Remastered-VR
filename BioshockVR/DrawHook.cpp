// BioshockVR/DrawHook.cpp
//
// FINGERPRINT FIRST, SUPPRESS SECOND.
//
// The HUD, the reticle, the pause menu and the coronas are all just draw calls.
// We cannot suppress what we cannot name, and we cannot name anything by
// guessing -- so this file starts by doing NOTHING except counting.
//
// Every draw is bucketed by its index/vertex count AND by which entry point
// issued it.
//
//   Numpad 3  dump the table
//   Numpad *  CLEAR the table   <-- the important one, see below
//   Numpad 1  toggle suppression of SuppressIndexCounts
//   Numpad -  ISOLATE: step to the next candidate, suppressing ONLY that one
//   Numpad /  ISOLATE off
//
// HOW TO SAMPLE CORRECTLY (learned the hard way, S13):
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
// PERSISTENCE IS THE DISCRIMINATOR (S21): walk the level for a minute after a
// CLEAR and scenery falls below 100% as it streams in and out, while the
// weapon, HUD and reticle stay pinned. Isolate then builds its own candidate
// list from exactly those survivors.
//
// FOUR ENTRY POINTS (S24): only Draw and DrawIndexed were hooked for six
// sessions, and every one of the eleven persistent counts was eventually
// eliminated as the cursor. Something on screen every frame that appears in NO
// bucket is not being drawn through a hooked function. So the instanced entry
// points are hooked too, and buckets are typed by entry point rather than by a
// bool.
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

extern void  LogFile(const char* msg);
extern bool  g_cfgDrawHook;
extern char  g_cfgSuppressList[256];
extern char  g_cfgMenuList[256];
extern char  g_cfgIsolateList[256];
extern char  g_cfgWeaponList[256];
extern float g_cfgWeaponScale;
extern char  g_cfgHudList[256];
extern float g_cfgHudScale;
extern int   g_cfgMenuMaxIndexed;
extern int   g_cfgMenuMaxDraw;
extern bool  g_cfgHookInstanced;

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
typedef void(__stdcall* DrawIndexedInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(__stdcall* DrawInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

static DrawIndexedFn          g_origDrawIndexed = nullptr;
static DrawFn                 g_origDraw = nullptr;
static DrawIndexedInstancedFn g_origDrawIndexedInstanced = nullptr;
static DrawInstancedFn        g_origDrawInstanced = nullptr;

static void* g_addrDrawIndexed = nullptr;
static void* g_addrDraw = nullptr;
static void* g_addrDrawIndexedInstanced = nullptr;
static void* g_addrDrawInstanced = nullptr;

// ---- entry-point kinds ----------------------------------------------------
// A count alone is ambiguous: there are separate Draw and DrawIndexed buckets
// numbered 6, and hiding "6" hid both (128 draws/frame) when only one was
// wanted. Counts may carry a kind suffix:
//     6d -> Draw    6i -> DrawIndexed    6s -> DrawInstanced
//     6x -> DrawIndexedInstanced         6  -> any entry point
enum CountKind
{
    KIND_ANY = 0,
    KIND_DRAW,
    KIND_INDEXED,
    KIND_INST,
    KIND_IDXINST
};

static const char* KindName(int k)
{
    switch (k)
    {
    case KIND_DRAW:    return "Draw";
    case KIND_INDEXED: return "Indexed";
    case KIND_INST:    return "Inst";
    case KIND_IDXINST: return "IdxInst";
    default:           return "any";
    }
}

static char KindSuffix(int k)
{
    switch (k)
    {
    case KIND_DRAW:    return 'd';
    case KIND_INDEXED: return 'i';
    case KIND_INST:    return 's';
    case KIND_IDXINST: return 'x';
    default:           return '?';
    }
}

struct CountRef
{
    unsigned count;
    int      kind;
    int      texW;      // 0 == no texture filter
    int      texH;
};

// ---- texture discrimination (S27) -----------------------------------------
// Count 5 Draw is BOTH the reticle and every text glyph -- one shared Scaleform
// batch. Suppressing the count took out all the menu text along with the
// cursor. They cannot be told apart by count because they ARE the same
// signature, but they read different textures: a small sprite vs a big font
// atlas. Texture DIMENSIONS are the discriminator, and unlike pointers they are
// stable across runs, so they can live in the ini.
//
// The SRV -> size lookup is cached: this runs on 115 draws/frame at 236 fps, so
// a QueryInterface per draw would not be free.
struct TexSizeCache
{
    void* srv;
    int   w, h;
};

static const int kTexCacheN = 32;
static TexSizeCache g_texCache[kTexCacheN];
static int          g_texCacheUsed = 0;

static bool PSTexSize(ID3D11DeviceContext* ctx, int* w, int* h)
{
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);        // AddRefs
    if (!srv) return false;

    for (int i = 0; i < g_texCacheUsed; ++i)
        if (g_texCache[i].srv == srv)
        {
            *w = g_texCache[i].w; *h = g_texCache[i].h;
            srv->Release();
            return true;
        }

    bool ok = false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (res)
    {
        ID3D11Texture2D* t2d = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2d)) && t2d)
        {
            D3D11_TEXTURE2D_DESC td = {};
            t2d->GetDesc(&td);
            *w = (int)td.Width; *h = (int)td.Height;
            ok = true;

            if (g_texCacheUsed < kTexCacheN)
            {
                g_texCache[g_texCacheUsed].srv = srv;
                g_texCache[g_texCacheUsed].w = *w;
                g_texCache[g_texCacheUsed].h = *h;
                ++g_texCacheUsed;
            }
            t2d->Release();
        }
        res->Release();
    }
    srv->Release();
    return ok;
}

static inline bool RefMatches(const CountRef& r, unsigned count, int kind,
    ID3D11DeviceContext* ctx)
{
    if (r.count != count) return false;
    if (r.kind != KIND_ANY && r.kind != kind) return false;
    if (r.texW == 0 || !ctx) return true;

    int w = 0, h = 0;
    if (!PSTexSize(ctx, &w, &h)) return false;
    return (w == r.texW && h == r.texH);
}

// ---- the fingerprint table ------------------------------------------------
struct Bucket
{
    unsigned count;        // IndexCount / VertexCount (per instance)
    unsigned long long total;
    unsigned perFrame;     // calls in the frame just ended
    unsigned maxPerFrame;
    unsigned framesSeen;
    int      kind;
};

static const int kMaxBuckets = 256;
static Bucket   g_buckets[kMaxBuckets];
static int      g_bucketCount = 0;
static unsigned long long g_frames = 0;

// ---- suppression ----------------------------------------------------------
static CountRef g_suppress[32];
static int      g_suppressCount = 0;
// Per-entry hit counters. "Did my texture filter actually match anything?" is
// otherwise unanswerable from inside a headset, and guessing from symptoms cost
// two full sessions.
static unsigned long long g_suppressHits[32] = {};
static bool     g_suppressOn = false;   // Numpad 1. OFF until you ask for it.
static unsigned long long g_suppressed = 0;

// ---- isolate stepper ------------------------------------------------------
// g_isoIdx == -1 means OFF and nothing is hidden. Otherwise EXACTLY ONE ref is
// hidden. Independent of g_suppressOn -- isolate overrides it, so you cannot
// have both lists active and misattribute what disappeared.
static CountRef g_isoList[32];
static int      g_isoN = 0;
static int      g_isoIdx = -1;
static bool     g_isoAuto = false;      // list came from the table, not the ini
static unsigned long long g_isoHidden = 0;
static bool     g_cbDumpPending = false;   // one-shot VS constant capture

// One-frame texture survey for the isolated count. Answers "which textures does
// this draw signature actually use", which is how a shared batch gets split.
static bool     g_texProbe = false;
struct ProbeEntry { int w, h; unsigned calls; };
static ProbeEntry g_probe[16];
static int        g_probeN = 0;

// ---- weapon FOV correction (S23) ------------------------------------------
// MEASURED from the wrench's VS constants: rows [40] and [44] have lengths
// 2.4538 and 2.3094 == 1/tan(fov/2) for a 44.3 deg horizontal FOV, while the
// world renders at 110. tan(55)/tan(22.17) == 3.5, exactly how much too large
// the wrench looks.
//
// Scaling the projection's x/y by k is IDENTICAL to rendering into a viewport
// shrunk by k about the screen centre: both give ndc -> W*(k*ndc*0.5 + 0.5).
// Two RSSetViewports calls do it, with no constant-buffer Map per draw (which
// at 10 weapon draws per frame would be ~2360 GPU stalls a second).
static CountRef g_weapon[8];
static int      g_weaponN = 0;

// ---- HUD recentring (S25) -------------------------------------------------
// Same mechanism, opposite purpose. A viewport scaled about the centre pulls
// CORNER elements inward, which is exactly what health (23d) and EVE (11d)
// need -- they sit in the top-left corner of a 110-degree frame, well outside
// where a headset can comfortably look. The reticle is already centred, so
// scaling would not move it; that one gets suppressed instead.
static CountRef g_hud[8];
static int      g_hudN = 0;

// Returns the viewport scale to use for this draw, or 0 if it is not ours.
static float ViewportScaleFor(unsigned count, int kind, ID3D11DeviceContext* ctx);

// ---- menu detection -------------------------------------------------------
// ALL configured counts must appear in the SAME frame before we call it a menu.
// ANY-match is not selective enough: small counts (21, 63, 87...) are shared
// with particles and decals, and one stray match flipped the layer mid-turn.
//
// S20: different menus have DIFFERENT signatures, so the list is GROUPS:
//     MenuIndexCounts=1493;5143      ->  (1493) OR (5143)
//     MenuIndexCounts=12,34;56       ->  (12 AND 34) OR (56)
// AND inside a group, OR between groups. No ';' == one group == old behaviour.
//
// S23: the structural test below covers every pre-game menu at once, so groups
// are now only needed for menus drawn OVER a live world (the pause menu).
struct MenuGroup
{
    CountRef counts[8];
    int      n;
    unsigned mask;      // bit i == counts[i] seen this frame
};

// S29: BioShock has at least nine distinct menus that render OVER a live world
// (pause, load, save, options and its four sub-screens, weapon/plasmid select).
// The structural low-geometry test cannot see any of them because the world is
// still drawing behind, so each needs its own signature group.
static const int kMaxMenuGroups = 12;
static MenuGroup g_menuGroups[kMaxMenuGroups];
static int       g_menuGroupN = 0;
static int       g_menuHits = 0;
static int       g_menuMiss = 0;
static bool      g_menuUp = false;

// Pre-game menus are pure Scaleform: exactly ONE DrawIndexed per frame.
// Gameplay runs hundreds. No count list separates them -- 4d/5d/6d/36i appear
// in both -- but the indexed-draw COUNT separates them by two orders of
// magnitude. Structure beats enumeration, and it covers menus we have not seen.
static unsigned g_indexedThisFrame = 0;

// S30: and the in-game menus -- pause, load, save, options and its four
// sub-screens, weapon select -- render OVER a live world, so the indexed test
// above cannot see them. MEASURED across seven captures: those menus issue
// 20-31 non-indexed Draw calls per frame while gameplay issues 120+, because
// menus hide the HUD and gameplay does not. A 4x gap with no overlap.
//
// This replaces per-menu count signatures, which were a dead end: the counts
// that looked menu-specific were world geometry that happened to be in view
// because every menu was opened from the same spot.
static unsigned g_drawThisFrame = 0;

// EXACT per-frame draw statistics. The dump truncates to 64 rows, so summing a
// dump's totals gives a LOWER BOUND, not the real per-frame count -- which is
// how MenuMaxDraw got set to 60 when the true menu figure was near it. These
// counters are exact and untruncated. Set thresholds from THESE, never from a
// dump's column sums.
static unsigned g_drawMin = 0xFFFFFFFF, g_drawMax = 0;
static unsigned g_idxMin = 0xFFFFFFFF, g_idxMax = 0;
static unsigned long long g_drawSum = 0, g_idxSum = 0, g_statFrames = 0;

// Scaling is applied ONLY when this frame is positively confirmed as gameplay.
// Fail-safe direction matters: an unscaled HUD in the corner is a minor
// annoyance, a scaled menu is unreadable. Uncertainty must mean "do nothing".
static bool     g_gameplayConfirmed = false;
static int      g_gameplayRun = 0;

bool DrawHook_MenuUp() { return g_menuUp; }

static Bucket* FindBucket(unsigned count, int kind)
{
    for (int i = 0; i < g_bucketCount; ++i)
        if (g_buckets[i].count == count && g_buckets[i].kind == kind)
            return &g_buckets[i];

    if (g_bucketCount >= kMaxBuckets) return nullptr;
    Bucket* b = &g_buckets[g_bucketCount++];
    b->count = count;
    b->kind = kind;
    b->total = 0;
    b->perFrame = 0;
    b->maxPerFrame = 0;
    b->framesSeen = 0;
    return b;
}

// PERSISTENCE is the discriminator, not per-frame count (S21).
// The original sort was maxPerFrame-ascending, so with 173 distinct counts the
// top-48 window filled entirely with 1-per-frame draws and everything at 2-3
// per frame -- which is where a weapon with a depth prepass LIVES -- was cut
// off the bottom and never printed.
static int PersistPct(const Bucket& b)
{
    return g_frames ? (int)(b.framesSeen * 100ull / g_frames) : 0;
}

static void SortByPersistence(int* order)
{
    for (int i = 0; i < g_bucketCount; ++i) order[i] = i;

    for (int i = 0; i < g_bucketCount; ++i)
        for (int j = i + 1; j < g_bucketCount; ++j)
        {
            const Bucket& a = g_buckets[order[i]];
            const Bucket& b = g_buckets[order[j]];
            const int pa = PersistPct(a), pb = PersistPct(b);
            // Most persistent first; tie-break by FEWER total calls, so a
            // singleton object sorts above a bulk-geometry batch.
            const bool swap = (pb > pa) || (pb == pa && b.total < a.total);
            if (swap) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }
}

static bool IsSuppressed(unsigned count, int kind, ID3D11DeviceContext* ctx)
{
    // Isolate wins outright. One variable changing at a time is the whole point.
    if (g_isoIdx >= 0 && g_isoIdx < g_isoN)
    {
        if (RefMatches(g_isoList[g_isoIdx], count, kind, ctx)) { ++g_isoHidden; return true; }
        return false;
    }

    if (!g_suppressOn) return false;
    for (int i = 0; i < g_suppressCount; ++i)
        if (RefMatches(g_suppress[i], count, kind, ctx))
        {
            ++g_suppressHits[i];
            return true;
        }
    return false;
}

static float ViewportScaleFor(unsigned count, int kind, ID3D11DeviceContext* ctx)
{
    // S28: NEVER rescale a menu frame. 23d is the HUD icons in gameplay AND
    // menu text in menus -- same signature, different job. Scaling it dragged
    // "DIRECTOR'S COMMENTARY" inward at 0.6x until it landed on top of
    // "LOAD GAME", which looked like missing glyphs and got blamed on
    // suppression for two sessions. The HUD only matters while playing.
    if (g_menuUp || !g_gameplayConfirmed) return 0.0f;

    if (g_cfgWeaponScale > 0.0f && g_cfgWeaponScale <= 4.0f)
        for (int i = 0; i < g_weaponN; ++i)
            if (RefMatches(g_weapon[i], count, kind, ctx)) return g_cfgWeaponScale;

    if (g_cfgHudScale > 0.0f && g_cfgHudScale <= 4.0f)
        for (int i = 0; i < g_hudN; ++i)
            if (RefMatches(g_hud[i], count, kind, ctx)) return g_cfgHudScale;

    return 0.0f;
}

// Build the isolate list from what is ACTUALLY on screen persistently, rather
// than from a guess typed into the ini. Press CLEAR, walk the level for a
// minute so scenery streams in and out, then press isolate. Anything that
// survived that walk at ~100% is a permanent fixture and nothing else is.
static void IsolateBuildFromTable()
{
    int order[kMaxBuckets];
    SortByPersistence(order);

    g_isoN = 0;
    for (int i = 0; i < g_bucketCount && g_isoN < 32; ++i)
    {
        const Bucket& b = g_buckets[order[i]];
        if (PersistPct(b) < 99) break;          // sorted, so we are done
        g_isoList[g_isoN].count = b.count;
        g_isoList[g_isoN].kind = b.kind;
        ++g_isoN;
    }

    g_isoAuto = true;
    Log(">>> ISOLATE: built %d candidates from %llu frames of live table",
        g_isoN, g_frames);
    for (int i = 0; i < g_isoN; ++i)
        Log("   auto step %2d -> count %u %s", i + 1,
            g_isoList[i].count, KindName(g_isoList[i].kind));
}

// Step the isolate cursor. dir +1 forward, -1 back, 0 = turn it off.
static void IsolateStep(int dir)
{
    // No ini list (or we were reset) -> build one from the table right now.
    if (g_isoN <= 0 && dir != 0) IsolateBuildFromTable();

    if (g_isoN <= 0)
    {
        Log(">>> ISOLATE: nothing persistent yet. CLEAR, play a while, retry.");
        return;
    }

    if (dir == 0)
    {
        g_isoIdx = -1;
        if (g_isoAuto)
        {
            g_isoN = 0;             // next '-' rebuilds from the current table
            g_isoAuto = false;
            Log(">>> ISOLATE: OFF. Auto list dropped; next step rebuilds it.");
        }
        else Log(">>> ISOLATE: OFF. Nothing hidden.");
        return;
    }

    g_isoHidden = 0;
    g_isoIdx += dir;

    // Walk off either end -> OFF, so the cycle is  off, 1, 2, ... N, off, ...
    if (g_isoIdx >= g_isoN || g_isoIdx < -1) g_isoIdx = -1;

    if (g_isoIdx < 0) { Log(">>> ISOLATE: OFF (wrapped). Nothing hidden."); return; }

    g_cbDumpPending = true;     // capture this draw's constants once
    g_texProbe = true;          // and survey its textures for one frame
    g_probeN = 0;
    Log(">>> ISOLATE step %d of %d: hiding count %u %s   <<< IF SOMETHING JUST "
        "VANISHED, THIS IS IT", g_isoIdx + 1, g_isoN,
        g_isoList[g_isoIdx].count, KindName(g_isoList[g_isoIdx].kind));
}

static void DumpTable()
{
    Log("=== DRAW FINGERPRINT DUMP  (%llu frames) ===", g_frames);
    if (g_statFrames)
        Log("  PER-FRAME (exact, not truncated):  Draw min=%u avg=%llu max=%u |"
            "  Indexed min=%u avg=%llu max=%u  |  gameplay=%s",
            g_drawMin, g_drawSum / g_statFrames, g_drawMax,
            g_idxMin, g_idxSum / g_statFrames, g_idxMax,
            g_gameplayConfirmed ? "CONFIRMED (scaling ON)" : "no (scaling OFF)");
    Log("  count   kind        total   per-frame(max)  frames   seen%%");

    int order[kMaxBuckets];
    SortByPersistence(order);

    int shown = 0, persistent = 0;
    for (int i = 0; i < g_bucketCount && shown < 64; ++i)
    {
        const Bucket& b = g_buckets[order[i]];
        const int pct = PersistPct(b);
        if (pct >= 99) ++persistent;
        Log("  %5u%c  %-8s  %8llu   %3u          %6u   %3d%%%s",
            b.count, KindSuffix(b.kind), KindName(b.kind),
            b.total, b.maxPerFrame, b.framesSeen, pct,
            pct >= 99 ? "   <-- PERSISTENT" : "");
        ++shown;
    }
    Log("=== %d distinct counts, %d persistent, %llu suppressed so far ===",
        g_bucketCount, persistent, g_suppressed);

    if (g_isoIdx >= 0 && g_isoIdx < g_isoN)
    {
        Log("  !!! ISOLATE IS ACTIVE (count %u %s) -- it OVERRIDES the suppress",
            g_isoList[g_isoIdx].count, KindName(g_isoList[g_isoIdx].kind));
        Log("  !!! list entirely. Press Numpad / before testing SuppressIndexCounts.");
    }

    if (g_suppressCount)
    {
        Log("  SUPPRESS list (%s):", g_suppressOn ? "ARMED" : "not armed, Numpad 1");
        for (int i = 0; i < g_suppressCount; ++i)
        {
            if (g_suppress[i].texW)
                Log("    count %u %s @%dx%d  -> %llu hit(s)%s",
                    g_suppress[i].count, KindName(g_suppress[i].kind),
                    g_suppress[i].texW, g_suppress[i].texH, g_suppressHits[i],
                    g_suppressHits[i] ? "" : "   <-- MATCHED NOTHING");
            else
                Log("    count %u %s (no texture filter) -> %llu hit(s)",
                    g_suppress[i].count, KindName(g_suppress[i].kind),
                    g_suppressHits[i]);
        }
    }
}

// ---- vertex-shader constant dump (S22) ------------------------------------
// Suppression can only HIDE a draw. To move or resize one -- push the weapon
// away, drag the HUD in from the corner -- we have to see the transform the
// vertex shader reads. Step isolate onto a count and the FIRST matching draw
// copies VS constant slots 0..3 to a staging buffer and logs them as rows of
// four floats. A 4x4 transform shows up as four consecutive rows; the length of
// the first two rows is 1/tan(fov/2), which is how the weapon FOV was found.
//
// One shot per step: Map on the immediate context stalls the GPU, so this must
// never run per-frame.
static void DumpVSConstants(ID3D11DeviceContext* ctx, unsigned count, int kind)
{
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;

    ID3D11Buffer* cb[4] = {};
    ctx->VSGetConstantBuffers(0, 4, cb);          // AddRefs -- must Release

    Log("=== VS CONSTANTS for count %u %s ===", count, KindName(kind));

    for (int slot = 0; slot < 4; ++slot)
    {
        if (!cb[slot]) continue;

        D3D11_BUFFER_DESC bd = {};
        cb[slot]->GetDesc(&bd);

        D3D11_BUFFER_DESC sd = bd;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.StructureByteStride = 0;

        ID3D11Buffer* stage = nullptr;
        if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &stage)) && stage)
        {
            ctx->CopyResource(stage, cb[slot]);

            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m)) && m.pData)
            {
                const float* f = (const float*)m.pData;
                unsigned n = bd.ByteWidth / 4;
                if (n > 64) n = 64;              // first four matrices is plenty

                Log("  CB slot %d: %u bytes (%u floats, showing %u)",
                    slot, bd.ByteWidth, bd.ByteWidth / 4, n);
                for (unsigned i = 0; i + 3 < n; i += 4)
                    Log("    [%2u] %10.4f %10.4f %10.4f %10.4f",
                        i, f[i], f[i + 1], f[i + 2], f[i + 3]);

                ctx->Unmap(stage, 0);
            }
            else Log("  CB slot %d: Map failed", slot);

            stage->Release();
        }
        else Log("  CB slot %d: staging CreateBuffer failed", slot);

        cb[slot]->Release();
    }

    Log("=== end VS CONSTANTS ===");
    dev->Release();
}

static void NoteMenuCount(unsigned count, int kind, ID3D11DeviceContext* ctx)
{
    for (int g = 0; g < g_menuGroupN; ++g)
    {
        MenuGroup& mg = g_menuGroups[g];
        for (int i = 0; i < mg.n; ++i)
            if (RefMatches(mg.counts[i], count, kind, ctx)) { mg.mask |= (1u << i); break; }
    }
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
        g_texCacheUsed = 0;     // SRVs die on level change and addresses recycle
        g_drawMin = g_idxMin = 0xFFFFFFFF;
        g_drawMax = g_idxMax = 0;
        g_drawSum = g_idxSum = g_statFrames = 0;
        for (int i = 0; i < 32; ++i) g_suppressHits[i] = 0;
        if (g_isoAuto) { g_isoN = 0; g_isoIdx = -1; g_isoAuto = false; }
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
        if (g_suppressOn && g_isoIdx >= 0 && g_isoIdx < g_isoN)
        {
            Log("!!! but ISOLATE is active on count %u %s and OVERRIDES it.",
                g_isoList[g_isoIdx].count, KindName(g_isoList[g_isoIdx].kind));
            Log("!!! Press Numpad / to drop isolate, or this test means nothing.");
        }
        if (g_suppressOn && g_suppressCount == 0)
            Log("!!! suppress list is EMPTY -- nothing will be hidden.");
    }
    k1 = d1;

    // Numpad -  : next isolate candidate.  Numpad /  : isolate off.
    static bool kSub = false, kDiv = false;

    const bool dSub = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
    if (dSub && !kSub) IsolateStep(+1);
    kSub = dSub;

    const bool dDiv = (GetAsyncKeyState(VK_DIVIDE) & 0x8000) != 0;
    if (dDiv && !kDiv) IsolateStep(0);
    kDiv = dDiv;
}

// Shared front half of every hooked entry point. Returns true if the caller
// should SKIP the real draw.
static bool NoteDraw(ID3D11DeviceContext* ctx, unsigned count, int kind)
{
    Bucket* b = FindBucket(count, kind);
    if (b) { ++b->total; ++b->perFrame; }

    NoteMenuCount(count, kind, ctx);

    // Texture survey: match on count+kind ONLY (ignore any size filter), so the
    // probe reports every texture this signature touches.
    if (g_texProbe && g_isoIdx >= 0 && g_isoIdx < g_isoN &&
        g_isoList[g_isoIdx].count == count &&
        (g_isoList[g_isoIdx].kind == KIND_ANY || g_isoList[g_isoIdx].kind == kind))
    {
        int w = 0, h = 0;
        if (PSTexSize(ctx, &w, &h))
        {
            int slot = -1;
            for (int i = 0; i < g_probeN; ++i)
                if (g_probe[i].w == w && g_probe[i].h == h) { slot = i; break; }
            if (slot < 0 && g_probeN < 16)
            {
                slot = g_probeN++;
                g_probe[slot].w = w; g_probe[slot].h = h; g_probe[slot].calls = 0;
            }
            if (slot >= 0) ++g_probe[slot].calls;
        }
    }

    if (g_cbDumpPending && g_isoIdx >= 0 && g_isoIdx < g_isoN &&
        RefMatches(g_isoList[g_isoIdx], count, kind, ctx))
    {
        g_cbDumpPending = false;
        DumpVSConstants(ctx, count, kind);
    }

    // Count BEFORE suppression, so hiding a UI element cannot flip the menu
    // detector and cascade.
    if (kind == KIND_INDEXED || kind == KIND_IDXINST) ++g_indexedThisFrame;
    else                                              ++g_drawThisFrame;

    if (IsSuppressed(count, kind, ctx)) { ++g_suppressed; return true; }
    return false;
}

// Shrink the viewport about its centre. Returns false if the state could not be
// read, in which case the caller just draws normally.
static bool ViewportPush(ID3D11DeviceContext* ctx, D3D11_VIEWPORT* saved, float k)
{
    UINT n = 1;
    ctx->RSGetViewports(&n, saved);
    if (n != 1 || saved->Width <= 0.0f || saved->Height <= 0.0f) return false;

    D3D11_VIEWPORT s = *saved;
    s.Width = saved->Width * k;
    s.Height = saved->Height * k;
    s.TopLeftX = saved->TopLeftX + (saved->Width - s.Width) * 0.5f;
    s.TopLeftY = saved->TopLeftY + (saved->Height - s.Height) * 0.5f;

    ctx->RSSetViewports(1, &s);
    return true;
}

static void __stdcall hkDrawIndexed(ID3D11DeviceContext* ctx,
    UINT IndexCount, UINT StartIndex, INT BaseVertex)
{
    if (NoteDraw(ctx, IndexCount, KIND_INDEXED)) return;

    const float vs = ViewportScaleFor(IndexCount, KIND_INDEXED, ctx);
    if (vs > 0.0f)
    {
        D3D11_VIEWPORT vp = {};
        if (ViewportPush(ctx, &vp, vs))
        {
            g_origDrawIndexed(ctx, IndexCount, StartIndex, BaseVertex);
            ctx->RSSetViewports(1, &vp);
            return;
        }
    }

    g_origDrawIndexed(ctx, IndexCount, StartIndex, BaseVertex);
}

static void __stdcall hkDraw(ID3D11DeviceContext* ctx,
    UINT VertexCount, UINT StartVertex)
{
    if (NoteDraw(ctx, VertexCount, KIND_DRAW)) return;

    const float vs = ViewportScaleFor(VertexCount, KIND_DRAW, ctx);
    if (vs > 0.0f)
    {
        D3D11_VIEWPORT vp = {};
        if (ViewportPush(ctx, &vp, vs))
        {
            g_origDraw(ctx, VertexCount, StartVertex);
            ctx->RSSetViewports(1, &vp);
            return;
        }
    }

    g_origDraw(ctx, VertexCount, StartVertex);
}

static void __stdcall hkDrawIndexedInstanced(ID3D11DeviceContext* ctx,
    UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndex,
    INT BaseVertex, UINT StartInstance)
{
    if (NoteDraw(ctx, IndexCountPerInstance, KIND_IDXINST)) return;

    const float vs = ViewportScaleFor(IndexCountPerInstance, KIND_IDXINST, ctx);
    if (vs > 0.0f)
    {
        D3D11_VIEWPORT vp = {};
        if (ViewportPush(ctx, &vp, vs))
        {
            g_origDrawIndexedInstanced(ctx, IndexCountPerInstance, InstanceCount,
                StartIndex, BaseVertex, StartInstance);
            ctx->RSSetViewports(1, &vp);
            return;
        }
    }

    g_origDrawIndexedInstanced(ctx, IndexCountPerInstance, InstanceCount,
        StartIndex, BaseVertex, StartInstance);
}

static void __stdcall hkDrawInstanced(ID3D11DeviceContext* ctx,
    UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertex,
    UINT StartInstance)
{
    if (NoteDraw(ctx, VertexCountPerInstance, KIND_INST)) return;

    const float vs = ViewportScaleFor(VertexCountPerInstance, KIND_INST, ctx);
    if (vs > 0.0f)
    {
        D3D11_VIEWPORT vp = {};
        if (ViewportPush(ctx, &vp, vs))
        {
            g_origDrawInstanced(ctx, VertexCountPerInstance, InstanceCount,
                StartVertex, StartInstance);
            ctx->RSSetViewports(1, &vp);
            return;
        }
    }

    g_origDrawInstanced(ctx, VertexCountPerInstance, InstanceCount,
        StartVertex, StartInstance);
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

    if (g_texProbe)
    {
        g_texProbe = false;
        if (g_probeN > 0)
        {
            Log("  TEXTURES used by this count in one frame:");
            for (int i = 0; i < g_probeN; ++i)
                Log("    %4d x %-4d   %u call(s)   -> suffix @%dx%d",
                    g_probe[i].w, g_probe[i].h, g_probe[i].calls,
                    g_probe[i].w, g_probe[i].h);
            Log("  (a big atlas with many calls is text; a small sprite with few is the cursor)");
        }
    }

    // A menu is up if ANY group had ALL of its counts in this one frame.
    // S34: record WHICH rule fired. Three false positives during ordinary play
    // (01:57 log) and the log could not say whether it was a count group or the
    // structural test -- so the next false positive names itself.
    static char whyBuf[96] = {};
    bool all = false;
    for (int g = 0; g < g_menuGroupN; ++g)
    {
        MenuGroup& mg = g_menuGroups[g];
        const unsigned want = (mg.n > 0) ? ((1u << mg.n) - 1u) : 0u;
        if (mg.n > 0 && mg.mask == want)
        {
            all = true;
            if (!whyBuf[0])
                _snprintf_s(whyBuf, sizeof(whyBuf), _TRUNCATE,
                    "MenuIndexCounts group %d (first count %u)", g, mg.counts[0].count);
        }
        mg.mask = 0;
    }

    // Structural test: a frame with almost no indexed geometry is a menu.
    if (g_cfgMenuMaxIndexed > 0 &&
        g_indexedThisFrame <= (unsigned)g_cfgMenuMaxIndexed)
    {
        all = true;
        if (!whyBuf[0])
            _snprintf_s(whyBuf, sizeof(whyBuf), _TRUNCATE,
                "MenuMaxIndexed (%u indexed <= %d)",
                g_indexedThisFrame, g_cfgMenuMaxIndexed);
    }

    // ...and menus drawn over a live world are the low-Draw-call frames.
    if (g_cfgMenuMaxDraw > 0 &&
        g_drawThisFrame <= (unsigned)g_cfgMenuMaxDraw) all = true;

    // Positive confirmation of gameplay, needed before anything is rescaled.
    // Both counts must be comfortably ABOVE the menu thresholds, sustained for
    // 30 frames (~130ms), and one ambiguous frame drops it instantly. A menu
    // flickering across a threshold can therefore never enable scaling.
    {
        const bool looksLikePlay =
            (g_cfgMenuMaxIndexed <= 0 || g_indexedThisFrame > (unsigned)g_cfgMenuMaxIndexed * 2) &&
            (g_cfgMenuMaxDraw <= 0 || g_drawThisFrame > (unsigned)g_cfgMenuMaxDraw * 2);

        if (looksLikePlay && !all) { if (g_gameplayRun < 30) ++g_gameplayRun; }
        else { g_gameplayRun = 0; }

        g_gameplayConfirmed = (g_gameplayRun >= 30);
    }

    // Exact, untruncated per-frame statistics.
    if (g_drawThisFrame < g_drawMin) g_drawMin = g_drawThisFrame;
    if (g_drawThisFrame > g_drawMax) g_drawMax = g_drawThisFrame;
    if (g_indexedThisFrame < g_idxMin) g_idxMin = g_indexedThisFrame;
    if (g_indexedThisFrame > g_idxMax) g_idxMax = g_indexedThisFrame;
    g_drawSum += g_drawThisFrame;
    g_idxSum += g_indexedThisFrame;
    ++g_statFrames;

    g_indexedThisFrame = 0;
    g_drawThisFrame = 0;

    if (all) { ++g_menuHits; g_menuMiss = 0; }
    else { ++g_menuMiss; g_menuHits = 0; }

    // Hysteresis both ways (~8 frames == ~34ms at 236 Present/s).
    if (!g_menuUp && g_menuHits >= 8)
    {
        g_menuUp = true;
        Log(">>> DRAWHOOK: MENU DETECTED -- rule: %s",
            whyBuf[0] ? whyBuf : "(unknown)");
        whyBuf[0] = 0;
    }
    else if (g_menuUp && g_menuMiss >= 8)
    {
        g_menuUp = false;
        Log(">>> DRAWHOOK: menu closed");
    }

    PollKeys();
}

// "1493i", "6d", "4" ... kind suffix optional, default = any entry point.
static int ParseCounts(const char* s, CountRef* out, int maxOut)
{
    int n = 0;
    while (*s && n < maxOut)
    {
        while (*s == ' ' || *s == ',') ++s;
        // GetPrivateProfileString does NOT strip trailing ';' comments, so a
        // line like "SuppressIndexCounts=5d ; cursor off" arrives with the
        // comment attached and used to parse as an extra count 0. Stop here.
        if (!*s || *s == ';' || *s == '#') break;
        if (*s < '0' || *s > '9') break;

        char* endp = nullptr;
        const unsigned v = (unsigned)strtoul(s, &endp, 10);

        int kind = KIND_ANY;
        if (endp)
        {
            switch (*endp)
            {
            case 'd': case 'D': kind = KIND_DRAW;    break;
            case 'i': case 'I': kind = KIND_INDEXED; break;
            case 's': case 'S': kind = KIND_INST;    break;
            case 'x': case 'X': kind = KIND_IDXINST; break;
            default: break;
            }
        }

        out[n].count = v;
        out[n].kind = kind;
        out[n].texW = 0;
        out[n].texH = 0;

        // Optional texture filter: 5d@64x64
        const char* at = endp;
        while (at && *at && *at != ',' && *at != '@') ++at;
        if (at && *at == '@')
        {
            char* e2 = nullptr;
            const long tw = strtol(at + 1, &e2, 10);
            if (e2 && (*e2 == 'x' || *e2 == 'X'))
            {
                const long th = strtol(e2 + 1, nullptr, 10);
                if (tw > 0 && th > 0) { out[n].texW = (int)tw; out[n].texH = (int)th; }
            }
        }
        ++n;

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
            Log("   suppress count %u %s%s", g_suppress[i].count,
                KindName(g_suppress[i].kind),
                g_suppress[i].texW ? "  (texture-filtered)" : "");
    }
    else
    {
        Log("drawhook: suppress list EMPTY. Observing only, nothing hidden.");
    }

    g_isoN = ParseCounts(g_cfgIsolateList, g_isoList, 32);
    if (g_isoN)
    {
        Log("drawhook: %d ISOLATE candidates (Numpad - to step, Numpad / off):", g_isoN);
        for (int i = 0; i < g_isoN; ++i)
            Log("   isolate step %2d -> count %u %s", i + 1,
                g_isoList[i].count, KindName(g_isoList[i].kind));
    }
    else
    {
        Log("drawhook: isolate list empty -- will auto-build from the live table.");
    }

    g_weaponN = ParseCounts(g_cfgWeaponList, g_weapon, 8);
    if (g_weaponN && g_cfgWeaponScale > 0.0f)
    {
        Log("drawhook: %d weapon draw(s), viewport scale %.3f",
            g_weaponN, g_cfgWeaponScale);
        for (int i = 0; i < g_weaponN; ++i)
            Log("   weapon count %u %s", g_weapon[i].count, KindName(g_weapon[i].kind));
    }
    else Log("drawhook: weapon FOV correction OFF.");

    g_hudN = ParseCounts(g_cfgHudList, g_hud, 8);
    if (g_hudN && g_cfgHudScale > 0.0f)
    {
        Log("drawhook: %d HUD draw(s), viewport scale %.3f (pulls corners inward)",
            g_hudN, g_cfgHudScale);
        for (int i = 0; i < g_hudN; ++i)
            Log("   hud count %u %s", g_hud[i].count, KindName(g_hud[i].kind));
    }
    else Log("drawhook: HUD recentring OFF.");

    Log("drawhook: MenuMaxIndexed = %d %s", g_cfgMenuMaxIndexed,
        g_cfgMenuMaxIndexed > 0 ? "(low-geometry frames == menu)" : "(off)");
    Log("drawhook: MenuMaxDraw    = %d %s", g_cfgMenuMaxDraw,
        g_cfgMenuMaxDraw > 0 ? "(few Draw calls == in-game menu)" : "(off)");

    // Split MenuIndexCounts on ';' into OR-groups, each with AND semantics.
    g_menuGroupN = 0;
    {
        const char* s = g_cfgMenuList;
        while (*s && g_menuGroupN < kMaxMenuGroups)
        {
            char part[128];
            int  k = 0;
            while (*s && *s != ';' && k < (int)sizeof(part) - 1) part[k++] = *s++;
            part[k] = 0;
            if (*s == ';') ++s;

            MenuGroup& mg = g_menuGroups[g_menuGroupN];
            mg.mask = 0;
            mg.n = ParseCounts(part, mg.counts, 8);
            if (mg.n > 0) ++g_menuGroupN;
        }
    }

    if (g_menuGroupN)
    {
        Log("drawhook: %d menu group(s). ANY group matching == menu.", g_menuGroupN);
        for (int g = 0; g < g_menuGroupN; ++g)
        {
            const MenuGroup& mg = g_menuGroups[g];
            for (int i = 0; i < mg.n; ++i)
                Log("   group %d: count %u %s  (ALL %d must hit in one frame)",
                    g + 1, mg.counts[i].count, KindName(mg.counts[i].kind), mg.n);
        }
    }
    else
    {
        Log("drawhook: menu count list EMPTY (structural test only).");
    }
}

bool DrawHook_Install(void* pDrawIndexed, void* pDraw,
    void* pDrawIndexedInstanced, void* pDrawInstanced)
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

    // The instanced pair is NOT fatal if it fails -- we still fingerprint the
    // other two. But something on screen every frame that appears in no bucket
    // is most likely coming through here, so log loudly either way.
    if (!g_cfgHookInstanced)
    {
        Log("drawhook: instanced entry points NOT hooked (HookInstanced=0).");
        Log("drawhook: not needed -- the cursor turned out to be 5 Draw.");
    }
    else if (pDrawIndexedInstanced &&
        MH_CreateHook(pDrawIndexedInstanced, &hkDrawIndexedInstanced,
            (LPVOID*)&g_origDrawIndexedInstanced) == MH_OK &&
        MH_EnableHook(pDrawIndexedInstanced) == MH_OK)
    {
        g_addrDrawIndexedInstanced = pDrawIndexedInstanced;
        Log("drawhook: DrawIndexedInstanced hooked.");
    }
    else if (g_cfgHookInstanced) Log("!!! drawhook: DrawIndexedInstanced NOT hooked.");

    if (g_cfgHookInstanced && pDrawInstanced &&
        MH_CreateHook(pDrawInstanced, &hkDrawInstanced,
            (LPVOID*)&g_origDrawInstanced) == MH_OK &&
        MH_EnableHook(pDrawInstanced) == MH_OK)
    {
        g_addrDrawInstanced = pDrawInstanced;
        Log("drawhook: DrawInstanced hooked.");
    }
    else if (g_cfgHookInstanced) Log("!!! drawhook: DrawInstanced NOT hooked.");

    Log(">>> DRAW HOOK ARMED. Numpad 3 = dump, Numpad * = clear, Numpad 1 = suppress,");
    Log(">>> Numpad - = isolate next candidate, Numpad / = isolate off.");
    return true;
}

void DrawHook_Remove()
{
    if (g_addrDrawIndexed)          MH_DisableHook(g_addrDrawIndexed);
    if (g_addrDraw)                 MH_DisableHook(g_addrDraw);
    if (g_addrDrawIndexedInstanced) MH_DisableHook(g_addrDrawIndexedInstanced);
    if (g_addrDrawInstanced)        MH_DisableHook(g_addrDrawInstanced);
    g_addrDrawIndexed = g_addrDraw = nullptr;
    g_addrDrawIndexedInstanced = g_addrDrawInstanced = nullptr;
}