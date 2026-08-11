// BioshockVR/Game/EngineBridge.cpp
//
// M3-S1: LOCATE Object::GetPropertyTextByName. CALL NOTHING.
//
// ============================================================================
//  WHY THIS EXISTS
// ============================================================================
// M1-S2 came back NO: myHUD.bHideHUD never moves on the shipped game, so the
// cutscene signal four inert features are waiting on still does not exist.
// docs/ARCHITECTURE.md finding 2 is the next untried source -- Core.Object
// declares native GetPropertyTextByName(name) -> string, and retail script
// calls it ON LIVE INSTANCES. Console `get` reads the class default because the
// console resolves a CLASS; that measurement stands, the conclusion drawn from
// it did not.
//
// Locating and calling have completely different risk profiles. This file only
// locates: the worst case is a log line saying it found nothing.
//
// ============================================================================
//  HOW THE SCAN WORKS, AND WHY IT IS NOT FindCalcView'S SIX STAGES
// ============================================================================
// MEASURED, statically, from both shipped executables before a line of this was
// written. BioshockHD.exe contains the wide string
//
//     L"intUObjectexecGetPropertyTextByName"
//
// exactly once, in .rdata. That is UE2's IMPLEMENT_FUNCTION registration symbol
// name -- `int` + class + func -- and exactly one DWORD in the image points at
// it. That pointer is the first field of a 12-byte row in a table of natives
// whose neighbouring rows name execGetPropertyText, execSetPropertyText,
// execSetPropertyTextByName, execGotoState, execEnable, execDisable and
// execSaveConfig. So the four property accessors are four adjacent rows of one
// table, and the name string points straight at the row that owns the function
// pointer.
//
// FindCalcView needs six stages because an FName literal only gets it to a
// cached-index global, and it has to bridge from there to a function body. Here
// the anchor lands on the function's own table row, so three stages do it.
// That is fewer stages and fewer ways to be confidently wrong -- but it is a
// DEVIATION from what .planning/sessions/M3.md § M3-S1 specified, so it is
// written down here rather than left to be discovered.
//
// FindCalcView itself is untouched. It carries a "Do not touch it" banner and
// it works on Steam, Epic and GOG. The region and scan helpers below are
// deliberate copies of its shapes, not a refactor of them.
//
// ============================================================================
//  THE ONE REAL UNKNOWN
// ============================================================================
// The row's two trailing DWORDs are ZERO ON DISK. The whole result depends on
// them being filled at runtime -- which is what a table in writable .data with
// null function slots implies, but implication is not measurement. So: retry
// with backoff, and if they are still null at the end, DUMP THE RAW ROWS. One
// cycle then settles the structure instead of a second cycle guessing at it.
//
// DO NOT hardcode an RVA here if this fails. The row sits at 0x11BE684 on Steam
// and 0x11BD6B4 on Epic -- a hardcoded address would already be wrong on one of
// the two builds we can check. That is the whole argument of
// docs/ENGINE-MAP.md § Storefront divergence, made concrete.

#include "Game/EngineBridge.h"
#include "Core/Config.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>          // wcstombs_s, for the raw-row dump
#include <cwchar>

#pragma comment(lib, "psapi.lib")

extern void LogFile(const char* msg);

static void Log(const char* fmt, ...)
{
    char b[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

// ---------------------------------------------------------------- module

static uint8_t* g_modBase = nullptr;
static size_t   g_modSize = 0;

static bool ModuleBounds()
{
    if (g_modBase) return true;

    HMODULE h = GetModuleHandleW(nullptr);
    MODULEINFO mi = {};
    if (!h || !GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi)))
        return false;

    g_modBase = (uint8_t*)mi.lpBaseOfDll;
    g_modSize = mi.SizeOfImage;
    return true;
}

static bool InModule(const void* p)
{
    const uint8_t* a = (const uint8_t*)p;
    return g_modBase && a >= g_modBase && a < g_modBase + g_modSize;
}

static unsigned Rva(const void* p)
{
    return (unsigned)((const uint8_t*)p - g_modBase);
}

static bool Readable(const void* p, size_t n)
{
    if (!p || !n) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

    switch (mbi.Protect & 0xFF)
    {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        break;
    default: return false;
    }

    const uint8_t* rs = (const uint8_t*)mbi.BaseAddress;
    const uint8_t* re = rs + mbi.RegionSize;
    const uint8_t* a = (const uint8_t*)p;
    return (a >= rs) && (a + n <= re);
}

static bool IsExecutable(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
        prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_WRITECOPY;
}

// Bound the two scans to the sections that can hold their target: the symbol
// strings are const data, the table is writable data. Purely an optimisation --
// it turns two 21 MB passes into ~3 MB and ~5 MB, which matters because this
// runs on the game thread and a long pass is a dropped frame. If the headers
// do not parse, fall back to the whole module and lose nothing but time.
struct Span { uint8_t* base; size_t size; const char* what; };

static Span ModuleSection(const char* name)
{
    Span all = { g_modBase, g_modSize, "whole module" };
    if (!g_modBase) return all;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_modBase;
    if (!Readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return all;

    const IMAGE_NT_HEADERS32* nt =
        (const IMAGE_NT_HEADERS32*)(g_modBase + dos->e_lfanew);
    if (!Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return all;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const unsigned n = nt->FileHeader.NumberOfSections;
    if (!Readable(sec, n * sizeof(*sec))) return all;

    for (unsigned i = 0; i < n; ++i)
    {
        char nm[9] = {};
        memcpy(nm, sec[i].Name, 8);
        if (strcmp(nm, name) != 0) continue;

        const size_t size = sec[i].Misc.VirtualSize;
        uint8_t* base = g_modBase + sec[i].VirtualAddress;
        if (!size || !InModule(base)) break;
        return { base, size, name };
    }
    return all;
}

// ---------------------------------------------------------------- the targets
//
// All four accessors, not just the one the card names. They are four rows of
// the same table reached by the same code, so the extra three are free -- and
// four distinct executable addresses from adjacent rows is a far stronger
// result than one address alone. It also pre-answers the card's own fallback,
// which says to try the `string` overload if the `name` one fails.

struct Accessor
{
    const wchar_t* symbol;    // the IMPLEMENT_FUNCTION registration name
    const char*    label;     // what it is called in the log
    uint8_t*       str;       // stage 1: where the symbol string lives
    int            strHits;   // stage 1: how many copies of it exist
    uint8_t*       row;       // stage 2: the table row naming it
    int            rowHits;   // stage 2: how many rows named it
    void*          fn;        // stage 4: the winner, or null
    int            slot;      // stage 4: which DWORD it came out of
    bool           dumpCode;  // log enough bytes to read a field offset out
};

static Accessor g_acc[] =
{
    { L"intUObjectexecGetPropertyTextByName", "GetPropertyTextByName" },
    { L"intUObjectexecGetPropertyText",       "GetPropertyText"       },
    { L"intUObjectexecSetPropertyTextByName", "SetPropertyTextByName" },
    { L"intUObjectexecSetPropertyText",       "SetPropertyText"       },

    // ---- M6-S5: WHICH INTERFACE SCREEN IS UP -----------------------------
    // The mod needs FlashGUIController, because the whole interface is
    // addressed by NAME there -- 'HUD', 'Pause', one per screen -- which is an
    // exact answer to "which screen is up" instead of the draw-signature guess
    // that false-fires during play.
    //
    // LevelInfo declares the getter as a NATIVE:
    //     native final function FlashGUIController GetFlashGUIController();
    // and M3-S1 proved ANY registered native is findable by its
    // int<prefix><Class>exec<Func> string. So this row costs nothing but a
    // needle -- provided the needle is spelled right, which cost a cycle.
    //
    // THE PREFIX IS PART OF THE NEEDLE, AND IT IS THE CLASS'S OWN. UObject
    // subclasses take U; ACTOR subclasses take A. LevelInfo is an Actor. Both
    // rows below were written with U and both came back "NOT FOUND -- no symbol
    // string" in Run 1 -- a stage-1 miss, which is why the four accessors above
    // located normally in the same run and nothing looked broken.
    //
    // VERIFY A NEEDLE OFFLINE BEFORE SPENDING A HEADSET CYCLE ON IT. The whole
    // table is 1,823 int<..>exec<..> wide strings in .rdata; scanning the
    // shipped exe for one takes seconds and answers exactly this question.
    // Measured 2026-08-11 in Build/Final/BioshockHD.exe (Steam):
    //     0x00E06638  intALevelInfoexecGetFlashGUIController
    //     0x00E064E0  intALevelInfoexecFlashGUIControllerExists
    //
    // WE DO NOT NEED TO CALL IT, which matters because calling still needs a
    // constructed bytecode frame (M3-S2, never run). A getter this simple just
    // loads a field off `this` -- so DUMP ITS CODE and read the offset straight
    // out of the instruction. Static analysis of four instructions, the same
    // move that located these functions in the first place.
    { L"intALevelInfoexecGetFlashGUIController", "GetFlashGUIController",
      nullptr, 0, nullptr, 0, nullptr, 0, true },
    { L"intALevelInfoexecFlashGUIControllerExists", "FlashGUIControllerExists",
      nullptr, 0, nullptr, 0, nullptr, 0, true },

    // ---- THE MOVIE API -- M6-S5's real target, and a CUTSCENE lead --------
    // Found while checking the two rows above: UFlashGUIController registers 51
    // natives, and the interface is driven entirely through named Flash movies.
    //
    // GetTopPlayingMovie is literally "which screen is up". HideMovie is the
    // mechanism STATE.md lead 3 has been suspecting since M1-S2 falsified
    // bHideHUD: the HUD demonstrably hides while that bit never moves, and
    // ShockPlayer.uc calls exactly this during the Little Sister rescue.
    // IsPausedInterfaceActive answers pause without the draw-signature
    // heuristic that flipped 12 times in one spot during gameplay.
    //
    // LOCATE ONLY. CALLED: NONE. Every one of these is an exec native taking an
    // FFrame, so calling any of them is M3-S2 -- which has a second-source
    // negative and is not attempted here. Their addresses are worth having
    // before there is anything to do with them, because the locate is free and
    // proves the needles while a run is happening anyway.
    { L"intUFlashGUIControllerexecGetTopPlayingMovie", "GetTopPlayingMovie",
      nullptr, 0, nullptr, 0, nullptr, 0, true },
    { L"intUFlashGUIControllerexecHideMovie", "HideMovie",
      nullptr, 0, nullptr, 0, nullptr, 0, false },
    { L"intUFlashGUIControllerexecIsPausedInterfaceActive",
      "IsPausedInterfaceActive",
      nullptr, 0, nullptr, 0, nullptr, 0, false },
};
static const int kAccessors = (int)(sizeof(g_acc) / sizeof(g_acc[0]));

// The M3 card's four. Counted separately so a GUI row that does not resolve
// cannot make M3-S1's standing result read like a regression.
static const int kM3Accessors = 4;

// THE TERMINATOR IS PART OF THE NEEDLE, and that is not a detail.
// "intUObjectexecGetPropertyText" is a prefix of
// "intUObjectexecGetPropertyTextByName" -- matching without the terminator
// makes the shorter name hit inside the longer one and hands S2 the wrong
// function. This cost a wrong answer during the static analysis that preceded
// this file; it is not a hypothetical.
static size_t NeedleBytes(const wchar_t* s)
{
    return (wcslen(s) + 1) * sizeof(wchar_t);
}

// ---------------------------------------------------------------- stage 1

static void FindSymbols(const Span& sp)
{
    size_t len[kAccessors];
    size_t widest = 0;
    for (int i = 0; i < kAccessors; ++i)
    {
        len[i] = NeedleBytes(g_acc[i].symbol);
        if (len[i] > widest) widest = len[i];
    }
    if (sp.size < widest) return;

    // Step 2: these are wide string literals, which MSVC never emits at an odd
    // address. Halves the pass.
    uint8_t* e = sp.base + sp.size - widest;
    for (uint8_t* p = sp.base; p <= e; p += 2)
    {
        if (*(const uint16_t*)p != (uint16_t)L'i') continue;   // cheap filter
        for (int i = 0; i < kAccessors; ++i)
        {
            if (memcmp(p, g_acc[i].symbol, len[i]) != 0) continue;
            if (++g_acc[i].strHits == 1) g_acc[i].str = p;
            Log(">>> NATIVE: S1 %-22s symbol @ 0x%08X (rva 0x%X)",
                g_acc[i].label, (unsigned)(uintptr_t)p, Rva(p));
        }
    }
}

// ---------------------------------------------------------------- stage 2

static void FindRows(const Span& sp)
{
    if (sp.size < 4) return;

    // Step 4: a compiler-emitted table of pointers is DWORD aligned. Unlike
    // FindCalcView's byte-by-byte walk we are not looking for an immediate
    // embedded in an instruction stream, so alignment is safe to assume here.
    uint8_t* e = sp.base + sp.size - 4;
    for (uint8_t* p = sp.base; p <= e; p += 4)
    {
        const uint32_t v = *(const uint32_t*)p;
        if (!v) continue;
        for (int i = 0; i < kAccessors; ++i)
        {
            if (!g_acc[i].str) continue;
            if (v != (uint32_t)(uintptr_t)g_acc[i].str) continue;
            if (++g_acc[i].rowHits == 1) g_acc[i].row = p;
            Log(">>> NATIVE: S2 %-22s row    @ 0x%08X (rva 0x%X)",
                g_acc[i].label, (unsigned)(uintptr_t)p, Rva(p));
        }
    }
}

// ---------------------------------------------------------------- stage 3
//
// Validate the row against the TABLE'S OWN REGULARITY, not against arithmetic.
// A row is only believable if its neighbours are rows too, and the stride is
// MEASURED from those neighbours rather than assumed to be the 12 bytes the
// static analysis saw -- that is what keeps this portable to a build nobody
// here can check.

static bool LooksLikeSymbol(uint32_t v)
{
    const void* p = (const void*)(uintptr_t)v;
    if (!v || !InModule(p)) return false;
    if (!Readable(p, 10 * sizeof(wchar_t))) return false;
    return memcmp(p, L"intUObject", 10 * sizeof(wchar_t)) == 0;
}

static bool RowNames(const uint8_t* row)
{
    return Readable(row, 4) && LooksLikeSymbol(*(const uint32_t*)row);
}

// Smallest gap between neighbouring rows within +/-64 bytes, or 0 if the row
// stands alone -- which would mean we are not looking at a table at all.
static size_t MeasureStride(const uint8_t* row)
{
    size_t prev = 0;
    bool   have = false;
    size_t best = 0;

    for (int d = -64; d <= 64; d += 4)
    {
        const uint8_t* q = row + d;
        if (!InModule(q) || !RowNames(q)) continue;

        const size_t here = (size_t)(q - g_modBase);
        if (have)
        {
            const size_t gap = here - prev;
            if (!best || gap < best) best = gap;
        }
        prev = here;
        have = true;
    }
    return best;
}

static int CountNeighbours(const uint8_t* row, size_t stride)
{
    int ok = 0;
    for (int k = -4; k <= 4; ++k)
    {
        if (!k) continue;
        const uint8_t* q = row + (ptrdiff_t)k * (ptrdiff_t)stride;
        if (InModule(q) && RowNames(q)) ++ok;
    }
    return ok;
}

// ---------------------------------------------------------------- stage 4
//
// Score both trailing DWORDs and LOG BOTH. The card is explicit that a single
// confident wrong answer is worse than two ranked ones, and we genuinely do not
// know which slot holds the function pointer -- only that the name is in the
// first.
//
// Deliberately NOT gated on FindCalcView's `55 8B EC` prologue. An optimised
// exec native need not keep a frame pointer, and rejecting the real function
// for failing a test it was never obliged to pass is how this costs a cycle.
// The bytes are logged instead; S2 verifies against what S1 recorded.

static int ScoreSlot(uint32_t v, char* out, size_t outSz)
{
    if (!v)
    {
        _snprintf_s(out, outSz, _TRUNCATE, "null");
        return 0;
    }

    const void* p = (const void*)(uintptr_t)v;
    int score = 1;
    char why[192] = {};

    const bool inMod = InModule(p);
    const bool exec = inMod && IsExecutable(p);
    const bool pad = inMod && Readable((const uint8_t*)p - 2, 2) &&
        ((const uint8_t*)p)[-1] == 0xCC && ((const uint8_t*)p)[-2] == 0xCC;

    if (inMod) ++score;
    if (exec)  ++score;
    if (pad)   ++score;

    _snprintf_s(why, sizeof(why), _TRUNCATE,
        "0x%08X %s%s%s", v,
        inMod ? "in-module " : "OUTSIDE-MODULE ",
        exec ? "exec " : "not-exec ",
        pad ? "CC-padded " : "");

    if (inMod && Readable(p, 16))
    {
        const uint8_t* b = (const uint8_t*)p;
        char hex[64] = {};
        for (int i = 0; i < 8; ++i)
            _snprintf_s(hex + i * 3, 4, _TRUNCATE, "%02X ", b[i]);
        _snprintf_s(out, outSz, _TRUNCATE, "%s(rva 0x%X) bytes %s",
            why, Rva(p), hex);
    }
    else
    {
        _snprintf_s(out, outSz, _TRUNCATE, "%s", why);
    }
    return score;
}

// ---------------------------------------------------------------- the dump
//
// The fallback that makes a NO cost one cycle instead of two. If the function
// slots never fill, print the raw rows so the next session can see the real
// layout instead of theorising about it.

static void DumpRows(const uint8_t* row, size_t stride)
{
    Log(">>> NATIVE: ---- raw table rows (stride %u) ----", (unsigned)stride);
    for (int k = -8; k <= 8; ++k)
    {
        const uint8_t* q = row + (ptrdiff_t)k * (ptrdiff_t)stride;
        if (!InModule(q) || !Readable(q, 12))
        {
            Log(">>> NATIVE:   [%+d] unreadable", k);
            continue;
        }

        const uint32_t d0 = *(const uint32_t*)q;
        const uint32_t d1 = *(const uint32_t*)(q + 4);
        const uint32_t d2 = *(const uint32_t*)(q + 8);

        char nm[96] = "<not a symbol>";
        if (LooksLikeSymbol(d0))
        {
            const wchar_t* w = (const wchar_t*)(uintptr_t)d0;
            size_t used = 0;
            wcstombs_s(&used, nm, sizeof(nm), w, _TRUNCATE);
        }
        Log(">>> NATIVE:   [%+d] rva 0x%X  %08X %08X  %s",
            k, Rva(q), d1, d2, nm);
    }
}

// ---------------------------------------------------------------- the session

static const int kAttemptAt[] = { 60, 240, 600, 1200, 2400 };
static const int kAttempts = (int)(sizeof(kAttemptAt) / sizeof(kAttemptAt[0]));

static int    g_tick = 0;
static int    g_attempt = 0;
static bool   g_located = false;   // stages 1-3 done; they are static, do once
static bool   g_done = false;
static size_t g_stride = 0;

static void Locate()
{
    const Span rdata = ModuleSection(".rdata");
    const Span data = ModuleSection(".data");

    Log(">>> NATIVE: module 0x%08X size 0x%X | strings in %s (0x%X) | "
        "table in %s (0x%X)",
        (unsigned)(uintptr_t)g_modBase, (unsigned)g_modSize,
        rdata.what, (unsigned)rdata.size, data.what, (unsigned)data.size);

    const DWORD t0 = GetTickCount();
    FindSymbols(rdata);
    FindRows(data);
    Log(">>> NATIVE: scan took %lu ms", GetTickCount() - t0);

    for (int i = 0; i < kAccessors; ++i)
    {
        const Accessor& a = g_acc[i];
        if (!a.strHits)
            Log(">>> NATIVE: S1 %-22s NOT FOUND -- no symbol string", a.label);
        else if (a.strHits > 1)
            Log(">>> NATIVE: S1 %-22s %d copies of the symbol; using the first",
                a.label, a.strHits);
        if (a.strHits && !a.rowHits)
            Log(">>> NATIVE: S2 %-22s symbol found but NO ROW points at it",
                a.label);
        else if (a.rowHits > 1)
            Log(">>> NATIVE: S2 %-22s %d rows name it -- AMBIGUOUS",
                a.label, a.rowHits);
    }

    // Stage 3 runs against the card's named target. The other three rows are
    // corroboration; this is the one whose identity has to hold up.
    const Accessor& t = g_acc[0];
    if (t.row && t.rowHits == 1)
    {
        g_stride = MeasureStride(t.row);
        if (!g_stride)
        {
            Log(">>> NATIVE: S3 FAIL -- the row has no neighbours. Not a "
                "table. Locking nothing.");
        }
        else
        {
            // Not `near` -- windows.h still #defines that.
            const int agree = CountNeighbours(t.row, g_stride);
            Log(">>> NATIVE: S3 stride %u measured, %d of 8 neighbours are "
                "rows", (unsigned)g_stride, agree);
            if (agree < 5)
            {
                Log(">>> NATIVE: S3 FAIL -- too few neighbours agree. "
                    "Locking nothing.");
                g_stride = 0;
            }
        }
    }
}

// Cheap: re-reads the two trailing DWORDs of rows already found. This is the
// part that repeats on backoff, because it is the part that can legitimately
// change -- the slots are null on disk and filled at runtime.
static bool Resolve()
{
    bool any = false;

    for (int i = 0; i < kAccessors; ++i)
    {
        Accessor& a = g_acc[i];
        if (a.fn) { any = true; continue; }
        if (!a.row || a.rowHits != 1) continue;
        if (!Readable(a.row, 12)) continue;

        const uint32_t d1 = *(const uint32_t*)(a.row + 4);
        const uint32_t d2 = *(const uint32_t*)(a.row + 8);

        char w1[256] = {}, w2[256] = {};
        const int s1 = ScoreSlot(d1, w1, sizeof(w1));
        const int s2 = ScoreSlot(d2, w2, sizeof(w2));

        Log(">>> NATIVE: S4 %-22s slot1 score %d  %s",
            a.label, s1, w1);
        Log(">>> NATIVE: S4 %-22s slot2 score %d  %s",
            a.label, s2, w2);

        // A function pointer must be non-null, inside the module and on an
        // executable page. CC padding is a hint, never a gate.
        const bool ok1 = (s1 >= 3);
        const bool ok2 = (s2 >= 3);

        if (ok1 && ok2)
        {
            Log(">>> NATIVE: S4 %-22s BOTH slots look callable -- ambiguous, "
                "failing closed", a.label);
            continue;
        }
        if (!ok1 && !ok2) continue;      // still null: the backoff handles it

        a.fn = (void*)(uintptr_t)(ok1 ? d1 : d2);
        a.slot = ok1 ? 1 : 2;
        any = true;

        const uint8_t* b = (const uint8_t*)a.fn;
        char hex[64] = {};
        if (Readable(b, 8))
            for (int k = 0; k < 8; ++k)
                _snprintf_s(hex + k * 3, 4, _TRUNCATE, "%02X ", b[k]);

        Log(">>> NATIVE: %s @ 0x%08X (rva 0x%X) prologue %s[slot %d]",
            a.label, (unsigned)(uintptr_t)a.fn, Rva(a.fn), hex, a.slot);

        // The field offset lives in the first few instructions of the getter.
        // Printed as bytes rather than disassembled: the mod has no
        // disassembler, and four lines of hex are enough to read a
        // mov reg,[reg+imm] by eye. Read-only.
        if (a.dumpCode && Readable(b, 64))
        {
            for (int line = 0; line < 4; ++line)
            {
                char row[80] = {};
                for (int k = 0; k < 16; ++k)
                    _snprintf_s(row + k * 3, 4, _TRUNCATE, "%02X ", b[line * 16 + k]);
                Log(">>> NATIVE:   code +0x%02X  %s", line * 16, row);
            }
        }
    }

    // Four distinct addresses out of four adjacent rows is the corroboration
    // that makes this more than a lucky pattern match. Say so either way.
    if (any)
    {
        int found = 0, distinct = 0;
        for (int i = 0; i < kM3Accessors; ++i)
        {
            if (!g_acc[i].fn) continue;
            ++found;
            bool dup = false;
            for (int j = 0; j < i; ++j)
                if (g_acc[j].fn == g_acc[i].fn) dup = true;
            if (!dup) ++distinct;
        }
        Log(">>> NATIVE: %d of %d accessors located, %d distinct addresses",
            found, kM3Accessors, distinct);

        // Reported apart from M3's four on purpose: these are M6-S5's, and one
        // of them failing says nothing about the property accessors.
        for (int i = kM3Accessors; i < kAccessors; ++i)
            Log(">>> NATIVE: M6-S5 %-26s %s", g_acc[i].label,
                g_acc[i].fn ? "LOCATED" : "not found");
    }

    // The card's target is the one that decides whether M3 continues.
    return g_acc[0].fn != nullptr;
}

void EngineBridge_Tick()
{
    if (g_done) return;

    if (!g_cfg.nativeScan)
    {
        Log(">>> NATIVE: EnableNativeScan=0 -- not scanning.");
        g_done = true;
        return;
    }
    if (!ModuleBounds())
    {
        Log(">>> NATIVE: no module bounds. Not scanning.");
        g_done = true;
        return;
    }

    // Every path below sets g_done on the last attempt, so this cannot be
    // reached with g_attempt == kAttempts today. It is here so that a future
    // edit which misses one of those paths stops instead of indexing off the
    // end of kAttemptAt.
    if (g_attempt >= kAttempts) { g_done = true; return; }

    // Backoff, not a standing scan: ~1s, 4s, 10s, 20s, 40s at 60fps. The
    // expensive pass runs once; only the slot re-read repeats.
    if (++g_tick < kAttemptAt[g_attempt]) return;
    const bool last = (++g_attempt >= kAttempts);

    if (!g_located)
    {
        Locate();
        g_located = true;
    }

    if (!g_stride || !g_acc[0].row)
    {
        Log(">>> NATIVE: nothing to resolve -- see S1/S2/S3 above. STOP.");
        g_done = true;
        return;
    }

    if (Resolve())
    {
        g_done = true;
        return;
    }

    if (last)
    {
        Log(">>> NATIVE: the function slots never filled after %d attempts. "
            "Dumping raw rows so the next session does not have to guess.",
            kAttempts);
        DumpRows(g_acc[0].row, g_stride);
        Log(">>> NATIVE: RESULT = NO. Do not hardcode an RVA here; the card "
            "says try the string overload, then open M5.");
        g_done = true;
    }
}

void* EngineBridge_GetPropertyTextByName() { return g_acc[0].fn; }
void* EngineBridge_GetPropertyText()       { return g_acc[1].fn; }
