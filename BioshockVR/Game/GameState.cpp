// BioshockVR/Game/GameState.cpp
//
// See GameState.h for why this is a structural scan and not a reflection walk.
//
// S37 IDENTIFICATION STRATEGY -- elimination, not guessing.
//
// ShockPlayerController declares exactly EIGHT strings, and seven of them are
// `config localized` with their values printed in defaultproperties:
//
//     PacifyText        "HARVEST"
//     SaveText          "RESCUE"
//     ReRollText        "Search Again"
//     WhatIsThisText    "WHAT IS THIS?"
//     PCWhatIsThisText  "<Mapping=ShowContextHelp> WHAT IS THIS?"
//     LocalizedHackText "HACK"
//     CollectText       "Rescue"
//
// The eighth is LastPlayerInputContext. So: scan for FString-shaped fields,
// cross off the seven known constants, and whatever is left is the field we
// want -- regardless of what it currently holds. Finding those seven is also
// proof we are reading the right object with the right struct layout, which a
// bare "does this look like a string" test could never give us.
//
// THREADING: Observe() runs on the GAME thread (inside CalcView). Readers run
// on the RENDER thread. The classification is one aligned long; readers only
// ever touch our snapshot, never the game's memory.

#include "Game/GameState.h"
#include "Game/EngineExec.h"
#include "Game/EngineBridge.h"
#include "Core/Keybinds.h"

#include <windows.h>
#include <intrin.h>
#include <psapi.h>          // MYHUD probe: module bounds for the vtable check
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "Core/Config.h"

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

// ---------------------------------------------------------------- classes
//
// Every context in User.ini's [Engine.Input] Contexts list, plus the section
// headers. Anything outside this set means we locked onto the wrong field, and
// we say so rather than quietly treating it as gameplay.

enum ContextClass
{
    CTX_UNKNOWN = 0,
    CTX_GAMEPLAY,      // world renders, player steers
    CTX_RADIAL,        // weapon/plasmid wheel over a live world
    CTX_SCRIPTED,      // world renders, input suppressed (cutscene, forced move)
    CTX_MENU,          // full-screen UI -- the one that wants a flat quad
};

struct ContextEntry { const char* name; ContextClass cls; };

static const ContextEntry kContexts[] = {
    { "Default",                           CTX_GAMEPLAY },
    { "MovementOnly",                      CTX_GAMEPLAY },
    { "NoMovement",                        CTX_GAMEPLAY },
    { "NoJump",                            CTX_GAMEPLAY },
    { "NoPlasmids",                        CTX_GAMEPLAY },
    { "OnlyMedHypo",                       CTX_GAMEPLAY },
    { "OnlyMedHypoAndMovement",            CTX_GAMEPLAY },
    { "EverythingExceptWeaponAndPlasmids", CTX_GAMEPLAY },
    { "GathererChoice",                    CTX_GAMEPLAY },
    { "InResurrectionStation",             CTX_GAMEPLAY },

    { "RadialActive",                      CTX_RADIAL },

    // NullInput is what Use() and StartForcePlayerMove() push -- the forced
    // camera signal, handed over by the game instead of inferred from rotation
    // divergence the way the FORCEDCAM probe was going to.
    { "NullInput",                         CTX_SCRIPTED },
    { "NullAllInputExceptHarvestRelease",  CTX_SCRIPTED },

    // ⚠ THE GAME MISSPELLS THIS. "Excorcising", not "Exorcising". Copied
    // verbatim from the Contexts list in User.ini, which is the authority --
    // the script corpus spells the ANIMATION name correctly
    // (ExorcisingGathererAnimationName), so anyone adding this from memory or
    // from ShockPlayer.uc will spell it right and it will never match.
    //
    // This table was diffed against User.ini on 2026-08-10 and this was the
    // ONLY context of the game's 30 that was missing -- and it is the Little
    // Sister rescue, which is exactly the sequence M3-S3 exists to detect.
    //
    // CTX_SCRIPTED is INFERRED, not measured: nothing in the corpus pushes it
    // (PUSHINPUTCONTEXT is a native Exec handler), so the classification comes
    // from the name and from its sibling above. Confirm it against a real
    // logged value the first time the context read actually works.
    { "ExcorcisingGatherer",               CTX_SCRIPTED },

    { "BathysphereUIActive",               CTX_SCRIPTED },

    { "PauseUIActive",                     CTX_MENU },
    { "InventoryUIActive",                 CTX_MENU },

    // NOT CTX_MENU. This context is entered on PROXIMITY to a lootable
    // container, not just when the panel opens -- and CTX_MENU closes the HUD
    // capture gate, which dumps the whole interface back onto the eye image.
    { "ContainerUIActive",                 CTX_GAMEPLAY },
    { "PlasmidEquipUIActive",              CTX_MENU },
    { "VendingMachineUIActive",            CTX_MENU },
    { "CraftingUIActive",                  CTX_MENU },
    { "InGameManualUIActive",              CTX_MENU },
    { "DNAUIActive",                       CTX_MENU },
    { "CalibrationUIActive",               CTX_MENU },
    { "HackingUIActive",                   CTX_MENU },
    { "MapsUIActive",                      CTX_MENU },
    { "ComboLockUIActive",                 CTX_MENU },
    { "WeaponStationUIActive",             CTX_MENU },
    { "WarningUIActive",                   CTX_MENU },
    { "PhotoGradingUIActive",              CTX_MENU },
};
static const int kNumContexts = (int)(sizeof(kContexts) / sizeof(kContexts[0]));

static ContextClass ClassifyContext(const char* s)
{
    if (!s || !s[0]) return CTX_UNKNOWN;
    for (int i = 0; i < kNumContexts; ++i)
        if (_stricmp(s, kContexts[i].name) == 0) return kContexts[i].cls;
    return CTX_UNKNOWN;
}

// The seven localized constants from ShockPlayerController's defaultproperties.
// Matching one identifies a field as NOT the one we want -- that is the point.
// Substring match, since these are localized: a non-English install has
// different text but the same layout, and a miss only leaves a harmless extra
// candidate for the recheck pass to sort out.
static const char* kKnownLocalized[] = {
    // --- ShockPlayerController (found at +0x7A0..+0x7D0 on the controller) ---
    "Quick Saving",
    "Game is not pauseable",
    "Now viewing from",
    "Game Plus:",
    // --- ShockPlayer, MEASURED on the pawn at 18:39 -----------------------
    // These bracket the field we want: LastPlayerInputContext is the FIRST
    // string in ShockPlayer's own block, so it sits between HandsClassString
    // (the last ShockPawn string) and GPSDestinationArrivedMessage (the first
    // ShockPlayer localized string).
    "Arrived at goal destination.",
    "No location information is available",
    "This goal's location is on a different level.",
    "Select a Plasmid to place",
    "Select a Physical Tonic",
    "Select an Engineering Tonic",
    "Select a Combat Tonic",
    "PLACE IN SLOT",
    "SELECT PLASMID",
    "SELECT TONIC",
    "Replaced %s with %s.",
    "The machine has OVERLOADED.",
    "The machine has short circuited.",
    "Bots released due to security alarm",
    "No hack attempted.",
    "Hack successful!",
    "Your speed is bioShocking",
    "A little sister can go faster",
    "No Subject in view",
    "Subject was mostly out of frame",
    "Subject is friendly",
    "Score too low",
    "Subject research complete",
    "Resistance To: ",
    "Enemy weakness information",
};
static const int kNumKnownLocalized =
(int)(sizeof(kKnownLocalized) / sizeof(kKnownLocalized[0]));

// The LOW anchor. ShockPawn's HandsClassString is the last string before
// ShockPlayer's block begins, and LastPlayerInputContext is the first string
// inside it -- so the field we want is immediately above this one.
static const char* kLowAnchor = "FirstPersonHands";

static const char* MatchKnownLocalized(const char* s)
{
    for (int i = 0; i < kNumKnownLocalized; ++i)
        if (_stricmp(s, kKnownLocalized[i]) == 0 || strstr(s, kKnownLocalized[i]))
            return kKnownLocalized[i];
    return nullptr;
}

// ---------------------------------------------------------------- published

static volatile long g_class = CTX_UNKNOWN;
static volatile long g_locked = 0;
static volatile long g_nameSeq = 0;
static char          g_name[64] = {};

static void PublishName(const char* s)
{
    _InterlockedIncrement(&g_nameSeq);
    MemoryBarrier();
    strncpy_s(g_name, s, _TRUNCATE);
    MemoryBarrier();
    _InterlockedIncrement(&g_nameSeq);
}

bool GameState_Valid() { return g_locked != 0; }
bool GameState_MenuUp() { return g_locked && g_class == CTX_MENU; }
bool GameState_RadialOpen() { return g_locked && g_class == CTX_RADIAL; }
bool GameState_ScriptedSequence() { return g_locked && g_class == CTX_SCRIPTED; }

const char* GameState_Context()
{
    static char out[64];
    for (int t = 0; t < 8; ++t)
    {
        const long s0 = g_nameSeq;
        if (s0 & 1) continue;
        MemoryBarrier();
        strncpy_s(out, g_name, _TRUNCATE);
        MemoryBarrier();
        if (g_nameSeq == s0) return out;
    }
    return "";
}

// ---------------------------------------------------------------- memory

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
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        break;
    default: return false;
    }

    const uint8_t* rs = (const uint8_t*)mbi.BaseAddress;
    const uint8_t* re = rs + mbi.RegionSize;
    const uint8_t* a = (const uint8_t*)p;
    return (a >= rs) && (a + n <= re);
}

// UE2 FString == TArray<TCHAR>. x86, UNICODE build:
//     +0x0  wchar_t* Data
//     +0x4  int32_t  Count   (INCLUDES the null terminator)
//     +0x8  int32_t  Max
struct FStringLike
{
    wchar_t* Data;
    int32_t  Count;
    int32_t  Max;
};

// An EMPTY FString is Data==null, Count==0, Max==0. That is still a real string
// field and we must track it: LastPlayerInputContext may well be empty at scan
// time, and skipping it would lose the one field that matters.
static bool IsEmptyFString(const uint8_t* obj, size_t off)
{
    if (!Readable(obj + off, sizeof(FStringLike))) return false;
    const FStringLike* fs = (const FStringLike*)(obj + off);
    return fs->Data == nullptr && fs->Count == 0 && fs->Max == 0;
}

// Strict on purpose. A false lock means classifying the entire VR presentation
// off a garbage field, which is worse than not locking at all.
static bool ReadFStringAt(const uint8_t* obj, size_t off, char* out, size_t outSz)
{
    if (!Readable(obj + off, sizeof(FStringLike))) return false;

    const FStringLike* fs = (const FStringLike*)(obj + off);

    if (fs->Count < 2 || fs->Count > 96) return false;
    if (fs->Max < fs->Count || fs->Max > 512) return false;
    if (!Readable(fs->Data, (size_t)fs->Count * sizeof(wchar_t))) return false;

    if (fs->Data[fs->Count - 1] != 0) return false;
    for (int i = 0; i < fs->Count - 1; ++i)
    {
        const wchar_t c = fs->Data[i];
        if (c < 0x20 || c > 0x7E) return false;
    }

    const int n = fs->Count - 1;
    if ((size_t)n + 1 > outSz) return false;
    for (int i = 0; i < n; ++i) out[i] = (char)fs->Data[i];
    out[n] = 0;
    return true;
}

// ---------------------------------------------------------------- the scan

static size_t g_offset = 0;
static bool   g_scanDone = false;

static int    g_observeCalls = 0;

// Fields that survived elimination: string-shaped, not a known constant.
// Watched until one holds a recognisable context name.
// S46: 12 was too few. The 18:49 scan found 64 empty slots in the window and
// we watched only the lowest 12 -- LastPlayerInputContext (predicted +0x740 by
// var arithmetic) was inside that range by luck, not by design. Watch them all;
// the recheck is three reads per candidate, twice a second.
static const int kMaxCand = 96;
static size_t g_cand[kMaxCand] = {};
static int    g_nCand = 0;
static int    g_recheck = 0;

// S44: ShockPlayer (the Pawn) declares LastPlayerInputContext as its 4th var.
// The controller does not carry a live copy -- 16KB of scanning there found
// only PlayerController's five localized strings. HandsProbe hands us the Pawn
// once STAGE A resolves it, and we re-scan against that instead.
static void* g_pawn = nullptr;

void GameState_SetPawn(void* pawn)
{
    if (!pawn || pawn == g_pawn) return;
    g_pawn = pawn;
    Log(">>> GAMESTATE: Pawn 0x%08X received. Re-scanning there -- ShockPlayer "
        "is where LastPlayerInputContext actually lives.", (unsigned)(uintptr_t)pawn);
    _InterlockedExchange(&g_locked, 0);
    _InterlockedExchange(&g_class, CTX_UNKNOWN);
    g_scanDone = false;
    g_observeCalls = 0;
    g_nCand = 0;
}

// S38: 0x1000 was NOT enough. The 13:58 scan found PlayerController's localized
// strings at +0x7A0..+0x7D0 and nothing above them -- ShockPlayerController's
// own block starts past there and ran off the end of the scan. Doubled.
static const size_t kScanMax = 0x2000;

static void ScanForContextField(const uint8_t* obj)
{
    Log(">>> GAMESTATE: scanning %s 0x%08X for FString fields...",
        g_pawn ? "PAWN" : "controller", (unsigned)(uintptr_t)obj);

    int known = 0, live = 0, empties = 0;
    size_t loAnchor = 0;        // HandsClassString -- ShockPlayer's block starts here
    size_t hiAnchor = 0;        // lowest known ShockPlayer localized constant
    g_nCand = 0;

    // ---- PASS 1: live strings. Establishes the WINDOW. ------------------
    for (size_t off = 0; off + sizeof(FStringLike) <= kScanMax; off += 4)
    {
        char val[128] = {};
        if (!ReadFStringAt(obj, off, val, sizeof(val))) continue;

        ++live;
        const ContextClass c = ClassifyContext(val);
        const char* kn = MatchKnownLocalized(val);
        const bool isLow = (strstr(val, kLowAnchor) != nullptr);

        Log(">>> GAMESTATE:   +0x%03X = \"%s\"%s", (unsigned)off, val,
            c != CTX_UNKNOWN ? "   <-- CONTEXT NAME"
            : isLow ? "   (LOW anchor)"
            : kn ? "   (known constant)" : "");

        if (isLow) { loAnchor = off; continue; }

        if (kn)
        {
            ++known;
            if (!hiAnchor || off < hiAnchor) hiAnchor = off;
            continue;
        }

        if (c != CTX_UNKNOWN && !g_locked)
        {
            g_offset = off;
            _InterlockedExchange(&g_locked, 1);
        }
        else if (g_nCand < kMaxCand) g_cand[g_nCand++] = off;
    }

    // ---- PASS 2: empty fields, WINDOWED. --------------------------------
    // Twelve zero bytes look exactly like an empty FString, so an unbounded
    // sweep produced 500+ hits and buried the real field. Bounded to the gap
    // between the two anchors it is a very short list -- and LastPlayerInputContext
    // is empty until the first context push, so this is the only way to see it.
    if (loAnchor && hiAnchor && hiAnchor > loAnchor)
    {
        Log(">>> GAMESTATE: window +0x%03X .. +0x%03X (between HandsClassString and "
            "the first ShockPlayer localized string)",
            (unsigned)loAnchor, (unsigned)hiAnchor);

        for (size_t off = loAnchor + 4; off + sizeof(FStringLike) <= hiAnchor; off += 4)
        {
            if (!IsEmptyFString(obj, off)) continue;
            ++empties;
            if (g_nCand < kMaxCand)
            {
                g_cand[g_nCand++] = off;
                Log(">>> GAMESTATE:   +0x%03X = \"\"  (empty, IN WINDOW -- watching)",
                    (unsigned)off);
            }
        }
    }
    else
    {
        Log(">>> GAMESTATE: !!! could not bracket the window "
            "(lo +0x%03X, hi +0x%03X). Not watching empties.",
            (unsigned)loAnchor, (unsigned)hiAnchor);
    }

    Log(">>> GAMESTATE: %d live string(s), %d known constant(s), %d empty in "
        "window, %d candidate(s)", live, known, empties, g_nCand);

    if (known >= 4)
        Log(">>> GAMESTATE: layout CONFIRMED");
    else
        Log(">>> GAMESTATE: !!! few known constants -- layout unconfirmed.");

    if (g_locked)
    {
        char val[128] = {};
        ReadFStringAt(obj, g_offset, val, sizeof(val));
        Log(">>> GAMESTATE: LOCKED on +0x%03X, context \"%s\"", (unsigned)g_offset, val);
    }
    else if (g_nCand)
        Log(">>> GAMESTATE: watching %d candidate(s) -- open a menu and I will "
            "lock on the one that changes.", g_nCand);
    else
        Log(">>> GAMESTATE: !!! nothing to watch.");
}

// Poll the survivors until one holds a recognisable context name. This is what
// catches LastPlayerInputContext when it was empty (or held something unlisted)
// at scan time.
static void RecheckCandidates(const uint8_t* obj)
{
    for (int i = 0; i < g_nCand; ++i)
    {
        char val[96] = {};
        if (!ReadFStringAt(obj, g_cand[i], val, sizeof(val))) continue;
        if (MatchKnownLocalized(val)) continue;

        if (ClassifyContext(val) != CTX_UNKNOWN)
        {
            g_offset = g_cand[i];
            _InterlockedExchange(&g_locked, 1);
            Log(">>> GAMESTATE: LOCKED on +0x%03X (candidate became \"%s\")",
                (unsigned)g_offset, val);
            return;
        }
    }
}

// Defined below, next to the probe. Declared here because Observe is the only
// caller and lives above them.
static void PollProbeKeys(const uint8_t* obj);
static void ApplyForegroundFov(const uint8_t* obj);
static void MyHudTick(const uint8_t* controller);
static void MyHudReset();
static void CineTick(const uint8_t* controller);
static void CineReset();
static void ForcedMoveTick(const uint8_t* controller);
static void ForcedMoveReset();
static void ScriptedAnimTick();
static void BathysphereTick();

// ---- WORLD FOV CEILING ---------------------------------------------------
// MEASURED: a vita chamber respawn drives controller+0x45C from 75 to 139.9,
// and +0x648 mirrors it. The value is saved with the game, so it survives a
// reload -- which is why it looked permanent.
//
// A CEILING, not a fixed write. Hands::FadeFOV animates this same field
// DOWNWARD when a weapon zooms, so anything below the threshold is left alone
// and scoping keeps working. Only the runaway value is caught.
// Two-sided now. The runaway-wide case was already handled; the bathysphere
// descent proved the narrow side matters just as much -- MEASURED 75.0 -> 60.0
// on BOTH fields while we kept reporting 100 to OpenXR. Rendering narrower than
// we report reads as a zoom, and no cutscene detection is needed to catch it
// because the value itself is the evidence.
static void ClampWorldFov(const uint8_t* obj)
{
    if (g_cfg.worldFovOff <= 0) return;
    if (g_cfg.worldFovMax <= 0.0f && g_cfg.worldFovMin <= 0.0f) return;

    const unsigned offs[2] = { (unsigned)g_cfg.worldFovOff,
                               (unsigned)g_cfg.worldFovOff2 };
    for (int i = 0; i < 2; ++i)
    {
        if (!offs[i]) continue;
        if (!Readable(obj + offs[i], 4)) continue;

        float* const p = (float*)(obj + offs[i]);

        const bool tooWide = (g_cfg.worldFovMax > 0.0f && *p > g_cfg.worldFovMax);
        const bool tooNarrow = (g_cfg.worldFovMin > 0.0f && *p < g_cfg.worldFovMin);
        if (!tooWide && !tooNarrow) continue;

        static DWORD lastLog = 0;
        const DWORD t = GetTickCount();
        if (t - lastLog >= 2000)
        {
            lastLog = t;
            Log(">>> WORLDFOV: +0x%03X was %.1f (%s) -- snapping to %.1f",
                offs[i], *p, tooWide ? "too wide" : "too narrow",
                g_cfg.worldFovVal);
        }
        *p = g_cfg.worldFovVal;
    }
}

static void FovAutoDiff(const uint8_t* obj);
static void ClampWorldFov(const uint8_t* obj);

// ============================================================================
// PAUSE / FULL-MENU DETECTION via Level.Pauser  (S67)
//   Level : Actor::Level is a LevelInfo shared by every actor, and a LevelInfo's
//           own Level member points to ITSELF -- so the offset Lo where
//           *(controller+Lo) == *(pawn+Lo) == L and *(L+Lo) == L pins both.
//   Pauser: null in play, an object while paused; found by watching for the slot
//           that goes null -> object -> null across one open/close of a menu.
// ============================================================================
static bool GsLooksLikeObject(const void* p)
{
    if (!p || ((uintptr_t)p & 3) || (uintptr_t)p < 0x10000) return false;
    if (!Readable(p, 4)) return false;
    const void* vt = *(const void* const*)p;
    if (!vt || ((uintptr_t)vt & 3) || !Readable(vt, 4)) return false;
    const void* fn = *(const void* const*)vt;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!fn || VirtualQuery(fn, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    switch (mbi.Protect & 0xFF) {
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

static void* g_level = nullptr;
static size_t g_levelOff = 0;
static size_t g_pauserOff = 0x668;
static volatile long g_paused = 0;

static const size_t kLvlScan = 0x1000;
static void* g_lvlSnap[kLvlScan / 4] = {};
static bool g_lvlBaseline = false;
static int  g_lvlMiss = 0;

static void FindLevel(const void* controller, const void* pawn)
{
    if (!pawn) return;
    for (size_t off = 0; off + 4 <= kLvlScan; off += 4)
    {
        if (!Readable((const uint8_t*)controller + off, 4)) continue;
        if (!Readable((const uint8_t*)pawn + off, 4)) continue;
        void* pc = *(void**)((const uint8_t*)controller + off);
        if (!pc || pc != *(void**)((const uint8_t*)pawn + off)) continue;
        if (!GsLooksLikeObject(pc)) continue;
        if (!Readable((const uint8_t*)pc + off, 4)) continue;
        if (*(void**)((const uint8_t*)pc + off) != pc) continue;   // self-referential
        g_level = pc; g_levelOff = off;
        Log(">>> PAUSE: Level = 0x%08X  (Actor::Level = +0x%X, self-referential)",
            (unsigned)(uintptr_t)pc, (unsigned)off);
        return;
    }
    if (++g_lvlMiss == 300)
        Log(">>> PAUSE: no self-referential Level pointer found yet -- still trying.");
}

static void HuntPauser()
{
    const uint8_t* L = (const uint8_t*)g_level;
    if (!g_lvlBaseline)
    {
        for (size_t off = 0; off + 4 <= kLvlScan; off += 4)
        {
            g_lvlSnap[off / 4] = nullptr;
            if (Readable(L + off, 4)) g_lvlSnap[off / 4] = *(void**)(L + off);
        }
        g_lvlBaseline = true;
        Log(">>> PAUSE: baseline set over 0x%X bytes. Open a full menu, then close it.",
            (unsigned)kLvlScan);
        return;
    }
    for (size_t off = 0; off + 4 <= kLvlScan; off += 4)
    {
        if (!Readable(L + off, 4)) continue;
        void* v = *(void**)(L + off);
        if (v == g_lvlSnap[off / 4]) continue;
        if (v && GsLooksLikeObject(v))
            Log(">>> PAUSE: slot +0x%X changed 0x%08X -> 0x%08X",
                (unsigned)off, (unsigned)(uintptr_t)g_lvlSnap[off / 4], (unsigned)(uintptr_t)v);
        g_lvlSnap[off / 4] = v;
    }
}

static void ObservePause(const void* controller)
{
    if (!g_level)
    {
        // Throttle: during a level load there IS no Level yet, and FindLevel is
        // a 0x1000-byte double scan. Running it every CalcView cost ~35ms/frame
        // (~30fps) for the whole load. Once every 30 calls is plenty.
        static int s_findTick = 0;
        if (++s_findTick >= 30) { s_findTick = 0; FindLevel(controller, g_pawn); }
        return;
    }
    // Level is recreated on a level load; re-find if the self-ref invariant breaks.
    if (!Readable((const uint8_t*)g_level + g_levelOff, 4) ||
        *(void**)((const uint8_t*)g_level + g_levelOff) != g_level)
    {
        g_level = nullptr; g_lvlBaseline = false; return;
    }   // keep g_pauserOff (class const)

    if (!g_pauserOff) { HuntPauser(); return; }

    bool paused = Readable((const uint8_t*)g_level + g_pauserOff, 4) &&
        *(void**)((const uint8_t*)g_level + g_pauserOff) != nullptr;
    const long prev = g_paused;
    _InterlockedExchange(&g_paused, paused ? 1 : 0);
    if (paused != (prev != 0)) Log(">>> PAUSE: %s", paused ? "PAUSED" : "unpaused");
}

void* GameState_Pawn() { return g_pawn; }

bool GameState_Paused() { return g_paused != 0; }

// TRUE only while a gameplay Level is locked (a player pawn exists). On the main
// menu there is no pawn, so this is false -- which is how we tell a real full-
// screen menu from a false draw-signature hit during play.
bool GameState_InGame() { return g_level != nullptr; }

bool GameState_GetPawnEyePoint(float outPos[3])
{
    if (!outPos || !g_pawn) return false;

    const unsigned kLocOff = 0x1D8;    // CONFIRMED by the 6-DOF hands probe
    const unsigned kEyeOff = 0x550;    // claimed; see the EYEHEIGHT log below

    const uint8_t* p = (const uint8_t*)g_pawn;
    if (!Readable(p + kLocOff, 12)) return false;
    if (!Readable(p + kEyeOff, 4))  return false;

    const float* loc = (const float*)(p + kLocOff);
    const float  eye = *(const float*)(p + kEyeOff);

    for (int i = 0; i < 3; ++i)
        if (!(loc[i] == loc[i]) || loc[i] < -1e8f || loc[i] > 1e8f) return false;
    if (!(eye == eye) || eye < 0.0f || eye > 250.0f) return false;

    // Is this BaseEyeHeight (static) or EyeHeight (animated)? If it is the
    // animated one the bob is INSIDE it and replacing the camera with it
    // achieves nothing. One second of min/max settles it.
    {
        static float lo = 1e9f, hi = -1e9f;
        static DWORD last = 0;
        if (eye < lo) lo = eye;
        if (eye > hi) hi = eye;
        const DWORD t = GetTickCount();
        if (t - last >= 1000)
        {
            last = t;
            Log("EYEHEIGHT: +0x550 min %.2f max %.2f  (spread %.2f)", lo, hi, hi - lo);
            lo = 1e9f; hi = -1e9f;
        }
    }

    outPos[0] = loc[0];
    outPos[1] = loc[1];
    outPos[2] = loc[2] + eye;
    return true;
}

// ============================================================================
// CUTSCENE DETECTION via PlayerController.ViewTarget  (S68)
//   During play ViewTarget == Pawn. During a scripted camera / bathysphere it
//   points at a different actor while Pawn is unchanged. So the controller slot
//   that matched the pawn but diverges to another object is ViewTarget. We watch
//   only those slots -> a handful of reads, no lag.
// ============================================================================
static size_t g_vtCand[48] = {};
static int    g_nVtCand = 0;
static void* g_vtPawn = nullptr;
static size_t g_viewTargetOff = 0;        // pin here once the log shows it
static volatile long g_cutscene = 0;

// ============================================================================
// CUTSCENE FOV HOLD  (S69)
// A cutscene scripts a narrow view FOV, so the backbuffer we reproject looks
// zoomed. Learn the steady FOV-band floats during play (throttled -> no scan
// lag), then on the first cutscene pin the ONE that zoomed the most as the world
// FOV and hold it for the duration. One targeted write; only during cutscenes.
// ============================================================================
static const size_t kFovScan = 0x1000;
static float         g_fovBase[kFovScan / 4] = {};
static unsigned char g_fovHold[kFovScan / 4] = {};   // consecutive steady samples
static size_t        g_fovOff = 0;                   // the world FOV field
static float         g_fovVal = 0.0f;
static int           g_fovTick = 0;

// ============================================================================
// CUTSCENE FOV HOLD  (S70)
// MEASURED: the world FOV lives on the controller at +0x45C and +0x648 -- both
// read 75.0 at rest and both drop to 55.0 when the pistol zooms. A scripted
// camera narrows them, which zooms the backbuffer we reproject. Two reads while
// playing, two writes during a cutscene. No scan.
// ============================================================================
static const size_t kFovA = 0x45C;
static const size_t kFovB = 0x648;
static float g_fovRest = 0.0f;

static void CutsceneFovHold(const uint8_t* obj)
{
    if (!Readable(obj + kFovA, 4)) return;
    float* a = (float*)(obj + kFovA);

    if (!g_cutscene)
    {
        // Learn the resting FOV: the LARGEST sane value seen, so an ADS dip
        // never becomes the baseline.
        const float v = *a;
        if (v > 40.0f && v < 140.0f && v > g_fovRest) g_fovRest = v;
        return;
    }

    if (g_fovRest < 40.0f) return;                  // never learned -- do nothing
    if (*a < g_fovRest - 0.5f) *a = g_fovRest;      // undo the zoom only
    if (Readable(obj + kFovB, 4))
    {
        float* b = (float*)(obj + kFovB);
        if (*b < g_fovRest - 0.5f) *b = g_fovRest;
    }
}

static void ObserveCutscene(const void* controller)
{
    if (!g_pawn) return;

    // (Re)baseline when the pawn changes: collect the controller slots that
    // currently point AT the pawn (Pawn, ViewTarget, and their aliases). During
    // a scripted camera one or more of these swings to the camera actor.
    // Find the slots ONCE. They are class offsets, so they survive level loads
    // and pawn changes -- and re-scanning during a cutscene poisoned the list
    // (the slots point at the camera actor then, not the pawn) which silently
    // cancelled the cutscene handling partway through.
    if (g_vtPawn != g_pawn)
    {
        g_vtPawn = g_pawn;
        g_nVtCand = 0;
        for (size_t off = 0x100; off + 4 <= 0x1000 && g_nVtCand < 48; off += 4)
            if (Readable((const uint8_t*)controller + off, 4) &&
                *(void**)((const uint8_t*)controller + off) == g_pawn)
                g_vtCand[g_nVtCand++] = off;
        Log(">>> CUTSCENE: watching %d controller slot(s) at the pawn.", g_nVtCand);
        _InterlockedExchange(&g_cutscene, 0);
        return;
    }

    // Cutscene == any of those slots has left the pawn for another live actor.
    bool cut = false;
    for (int i = 0; i < g_nVtCand; ++i)
    {
        if (!Readable((const uint8_t*)controller + g_vtCand[i], 4)) continue;
        void* v = *(void**)((const uint8_t*)controller + g_vtCand[i]);
        if (v && v != g_pawn && GsLooksLikeObject(v)) { cut = true; break; }
    }
    // S72: the ViewTarget signal is DEAD. Measured across a full session
    // covering the bathysphere AND the balcony: +0x450, +0x620 and +0x914 all
    // track the pawn exactly and never diverge, so this test can never fire.
    // The flag is now driven by injected pitch -- see GameState_PitchSample.
    // Kept as a diagnostic only: it no longer writes g_cutscene.
    static bool loggedDivergence = false;
    if (cut && !loggedDivergence)
    {
        loggedDivergence = true;
        Log(">>> CUTSCENE: a watched slot diverged from the pawn (diagnostic only).");
    }

    // 1 Hz diagnostic: what the watched slots actually hold. Delete once the
    // cutscene signal is confirmed.
    {
        static DWORD lastLog = 0;
        const DWORD t = GetTickCount();
        if (t - lastLog >= 1000)
        {
            lastLog = t;
            char b[256]; int n = 0;
            n += _snprintf_s(b + n, sizeof(b) - n, _TRUNCATE,
                "pawn 0x%08X", (unsigned)(uintptr_t)g_pawn);
            for (int i = 0; i < g_nVtCand; ++i)
            {
                void* v = nullptr;
                if (Readable((const uint8_t*)controller + g_vtCand[i], 4))
                    v = *(void**)((const uint8_t*)controller + g_vtCand[i]);
                n += _snprintf_s(b + n, sizeof(b) - n, _TRUNCATE,
                    "  +0x%X=0x%08X", (unsigned)g_vtCand[i], (unsigned)(uintptr_t)v);
            }
            Log(">>> CUTSCENE dbg: %s", b);
        }
    }
}

bool GameState_Cutscene() { return g_cutscene != 0; }

bool GameState_Theater()
{
    // A deliberate WHITELIST rather than the whole CTX_SCRIPTED class: that
    // class also covers forced-movement sequences where the world should stay
    // immersive. Add names here after testing them one at a time.
    if (GameState_Valid())
    {
        const char* c = GameState_Context();
        return strcmp(c, "NullInput") == 0 ||
            strcmp(c, "BathysphereUIActive") == 0;
    }
    return GameState_Cutscene();   // context not locked yet
}

// ============================================================================
// CUTSCENE DETECTION via INJECTED PITCH  (S72 -- HANDOFF_6 section 8, "Plan B")
// MEASURED, one session, both cutscenes:
//   normal play           0.0 deg/s, worst stray sample 9.4
//   bathysphere          17.4 .. 67.6 deg/s for 11 consecutive seconds
//   balcony              17.9 .. 57.1 deg/s for 10 consecutive seconds
// The bands do not overlap, so 12 deg/s separates them with margin. Two
// consecutive samples in each direction, so one stray second cannot flip it.
// Called once a second from the camera heartbeat. No scan, two ints.
// ============================================================================
void GameState_PitchSample(double degThisSecond)
{
    static int hi = 0, lo = 0;

    if (degThisSecond > 12.0) { ++hi; lo = 0; }
    else { ++lo; hi = 0; }

    const long prev = g_cutscene;

    // TELEMETRY ONLY. MEASURED: this latched ON during ordinary combat at
    // 39.1 deg/s for 4 consecutive seconds, freezing g_aimBase so the game kept
    // turning the player while the view did not follow -- then released two
    // seconds later with a snap. Injected pitch cannot separate combat from a
    // scripted camera, and the duration rule was not enough.
    //
    // g_cutscene is now only set by the ViewTarget path (currently dormant), so
    // in practice cutscene handling is off. That is the pre-1.0.2 behaviour.
    if (hi >= 4 && !prev)
        Log("  PITCH: %.1f deg/s, %d consecutive (telemetry only, not latching)",
            degThisSecond, hi);
}

// ============================================================================
// PAWN FRESHNESS (S71)
// MEASURED: AController::Pawn is controller+0x450 (STAGE A found it; the 1 Hz
// dbg confirms it tracks the pawn exactly). Loading a save rebuilds the pawn and
// the Level, but our cached copies did not follow -- g_pawn read 0x3FF114E0
// while the controller read 0x9CDD2AB0, which pinned CUTSCENE ON and PAUSE
// PAUSED forever and left the menu quad over everything. Read the pawn from the
// controller every call and reset everything downstream the moment it changes.
// ============================================================================
void HandsProbe_Reset();          // HandsProbe.cpp
void* HandsProbe_Get();           // HandsProbe.cpp -- the Hands actor, or null

static const size_t kPawnSlot = 0x450;

static void RefreshPawn(const void* controller)
{
    if (!Readable((const uint8_t*)controller + kPawnSlot, 4)) return;
    void* p = *(void**)((const uint8_t*)controller + kPawnSlot);
    if (!p || p == g_pawn || !GsLooksLikeObject(p)) return;

    Log(">>> GAMESTATE: pawn changed 0x%08X -> 0x%08X. Resetting level, cutscene, "
        "hands.", (unsigned)(uintptr_t)g_pawn, (unsigned)(uintptr_t)p);
    GameState_SetPawn(p);                   // sets g_pawn AND resets the context. scan, so it re-runs on the NEW pawn
    g_level = nullptr;                      // force Level re-find; Pauser was stale
    _InterlockedExchange(&g_paused, 0);
    _InterlockedExchange(&g_cutscene, 0);
    g_fovRest = 0.0f;
    HandsProbe_Reset();                     // re-lock hands/gun on the new pawn

    // THE BUG THAT RUINED THE LAST EXORCISM PROBE. Its snapshot was not reset
    // here, so the pawn-lock burst reported 18 transitions in a run that
    // contained no rescue at all. Drop the baselines and retake them.
    ForcedMoveReset();
}

// ---- reticle upkeep --------------------------------------------------------
// `set` writes the class default as well as the live instance, so this survives
// respawn and level change -- but game script can still turn it back on, so the
// state is re-asserted every 15 seconds rather than trusted once. Failures are
// retried every 2 seconds, because the engine object does not exist yet during
// the first moments of the process.
static void Reticle_Tick()
{
    const int want = g_cfg.disableReticle ? 1 : 0;
    const DWORD now = GetTickCount();

    static int   applied = -1;
    static DWORD lastTry = 0;
    static DWORD lastOk = 0;

    bool due = false;
    if (want != applied)                        due = (now - lastTry >= 2000);
    else if (want && (now - lastOk >= 15000))   due = true;
    if (!due) return;

    lastTry = now;

    const char* cmd = want ? "set ShockPlayer bReticleDisabled True"
        : "set ShockPlayer bReticleDisabled False";

    if (EngineExec_Run(cmd))
    {
        applied = want;
        lastOk = now;
        if (want != applied || applied == want)
            Log(">>> RETICLE: %s via engine SET", want ? "disabled" : "enabled");
    }
}

void GameState_Observe(void* playerController)
{
    if (!playerController) return;

    // S49: the FOV probe and the ForegroundFovAngle write run REGARDLESS of
    // EnableGameState. They only ever lived in this function because the
    // controller pointer was handy here; gating them on the (dead) context scan
    // meant EnableGameState=0 silently reverted the weapon to 60-degree
    // foreground FOV. Different feature, different switch.
    PollProbeKeys((const uint8_t*)playerController);
    FovAutoDiff((const uint8_t*)playerController);
    ApplyForegroundFov((const uint8_t*)playerController);
    ClampWorldFov((const uint8_t*)playerController);

    // M1-S1. Read-only and one-shot, and it runs on the same reasoning as the
    // FOV probes above: the controller pointer is already here, and it is a
    // different feature from the (dead) context scan, so it does not belong
    // behind EnableGameState.
    MyHudTick((const uint8_t*)playerController);

    // M1-S2. Reads the flag MyHudTick just located. Gates nothing.
    CineTick((const uint8_t*)playerController);

    // M7-S2. Publishes the measured scripted-animation signal. Runs
    // unconditionally for the same reason MyHudTick does: one DWORD read,
    // read-only, and it gates nothing by itself.
    ScriptedAnimTick();

    // M7-S4. Bathysphere mode, so the gameplay rotation freeze can exclude it.
    BathysphereTick();

    // M7-S1. Differential probe for a scripted-event flag. Read-only, gates
    // nothing, and default-off -- it is the only periodic diff in the mod.
    ForcedMoveTick((const uint8_t*)playerController);

    // M3-S1. Locates the native property accessors. Same reasoning as the two
    // probes above: read-only, one-shot, gates nothing, and a different feature
    // from the (dead) context scan -- so it does not belong behind
    // EnableGameState. It takes no argument because the engine's native table
    // is process-static; it has no pawn or controller lifetime to track.
    EngineBridge_Tick();

    // Before the EnableGameState early-return: the reticle should still go away
    // when the context scan is switched off. Different feature, different switch.
    Reticle_Tick();

    if (!g_cfg.gameState) return;
    RefreshPawn(playerController);

    // S76: DRIVE THE CONTEXT SCAN. ScanForContextField and RecheckCandidates
    // have existed since S38 with NO CALLER -- which is why the "definitive"
    // detector never produced one line of log. The decompile confirms
    // ShockPlayer declares LastPlayerInputContext, and kContexts already maps
    // "NullInput" to CTX_SCRIPTED. This only ever needed to be run.
    {
        const uint8_t* target = (const uint8_t*)(g_pawn ? g_pawn : playerController);
        if (!g_scanDone)
        {
            if (++g_observeCalls >= 600)          // let the level settle first
            {
                g_scanDone = true;
                ScanForContextField(target);
            }
        }
        else if (!g_locked && g_nCand)
        {
            if ((++g_observeCalls & 0x3F) == 0)   // ~4x/sec, not per frame
                RecheckCandidates(target);
        }
        else if (g_locked)
        {
            static char lastCtx[96] = {};
            char val[96] = {};
            if (ReadFStringAt(target, g_offset, val, sizeof(val)))
            {
                // PUBLISH IT. This was reading the context, logging it, and
                // throwing it away -- g_class stayed CTX_UNKNOWN forever, so
                // GameState_MenuUp / RadialOpen / ScriptedSequence could never
                // return true and GameState_Theater always fell through to the
                // pitch heuristic. The whole context detector was inert.
                const ContextClass c = ClassifyContext(val);
                _InterlockedExchange(&g_class, (long)c);
                PublishName(val);

                if (strcmp(val, lastCtx) != 0)
                {
                    strncpy_s(lastCtx, val, _TRUNCATE);
                    Log(">>> CONTEXT: \"%s\"  class=%d", val, (int)c);
                }
            }
        }
    }

    ObservePause(playerController);
    ObserveCutscene(playerController);
    CutsceneFovHold((const uint8_t*)playerController);
}

// ============================================================================
// S39: FLOAT SNAPSHOT / DIFF PROBE
//
// The config route to the weapon is dead -- neither [ShockGame.Hands]
// PlayerViewOffset nor a broken HandsOffscreenAnimationName changed anything,
// so Weapons.ini is not being read (baked into the .U packages, most likely).
//
// But FadeFOV names exactly what we need, on an object we already hold:
//
//     PlayerController(...).DesiredFOV         = ...
//     PlayerController(...).ForegroundFovAngle = ...
//
// Two floats. Finding them by value alone is ambiguous -- a controller is full
// of floats. Finding them by CHANGE is not: zooming the pistol drives BOTH to
// 55.0 (ZoomedFOVAngle=55, ZoomedForegroundFOVAngle=55), and almost nothing
// else in the object moves at the same instant.
//
//   Numpad 4  snapshot every float in the object
//   Numpad 5  dump only the floats that changed since the snapshot
//
// Procedure: equip the pistol, stand still, Numpad 4. Zoom (right stick click).
// Numpad 5. Two of the reported floats will now read 55.0 -- the one that was
// ~110 at rest is DesiredFOV, the other is ForegroundFovAngle.

static const size_t kFloatScanMax = 0x1000;
static float  g_snap[kFloatScanMax / 4] = {};
static bool   g_snapValid = false;

static bool PlausibleFloat(float f)
{
    if (f != f) return false;                       // NaN
    const float a = (f < 0.0f) ? -f : f;
    return a > 0.001f && a < 100000.0f;
}

static void SnapshotFloats(const uint8_t* obj)
{
    int n = 0;
    for (size_t off = 0; off + 4 <= kFloatScanMax; off += 4)
    {
        g_snap[off / 4] = 0.0f;
        if (!Readable(obj + off, 4)) continue;
        const float f = *(const float*)(obj + off);
        if (!PlausibleFloat(f)) continue;
        g_snap[off / 4] = f;
        ++n;
    }
    g_snapValid = true;
    Log(">>> FOVPROBE: snapshot taken (%d plausible floats). Now change something "
        "-- zoom the pistol -- and press PGDN.", n);
}

static void DiffFloats(const uint8_t* obj)
{
    if (!g_snapValid)
    {
        Log(">>> FOVPROBE: no snapshot yet. Press PGUP first.");
        return;
    }

    Log(">>> FOVPROBE: ---- floats that changed since the snapshot ----");
    int shown = 0;
    for (size_t off = 0; off + 4 <= kFloatScanMax; off += 4)
    {
        if (!Readable(obj + off, 4)) continue;
        const float now = *(const float*)(obj + off);
        const float was = g_snap[off / 4];

        if (!PlausibleFloat(now) && was == 0.0f) continue;

        float d = now - was;
        if (d < 0.0f) d = -d;
        if (d < 0.01f) continue;

        // An FOV is an angle. Flagging the plausible band makes the two we want
        // stand out from animation timers and velocities in the same dump.
        const bool fovish = (now > 10.0f && now < 150.0f) &&
            (was > 10.0f && was < 150.0f);

        Log(">>> FOVPROBE:   +0x%03X  %10.3f -> %10.3f%s",
            (unsigned)off, was, now, fovish ? "   <-- FOV-SHAPED" : "");

        if (++shown >= 60) { Log(">>> FOVPROBE:   ...truncated at 60"); break; }
    }
    if (!shown) Log(">>> FOVPROBE:   (nothing changed)");
    Log(">>> FOVPROBE: -------------------------------------------------");
}

// Once the offset is known, write it every CalcView. Same object, same thread,
// same place we already write the rotator -- so it lands before the game builds
// the foreground projection for this frame.
static void ApplyForegroundFov(const uint8_t* obj)
{
    if (g_cfg.fgFovOffset <= 0) return;

    const size_t off = (size_t)g_cfg.fgFovOffset;
    if (!Readable(obj + off, 4)) return;

    float* p = (float*)(obj + off);

    // Preference order:
    //   1. COPY from another float in the same object (DesiredFOV / FovAngle).
    //      MEASURED 16:01: world FOV is 75 and the foreground is 60 -- and the
    //      world value is NOT the 110 in Bioshock.ini, which is BioShock's own
    //      user setting in a different convention. Hardcoding 110 here would
    //      overcorrect by miles. Copying also keeps zoom working: FadeFOV drives
    //      the source, and the foreground follows it.
    //   2. An explicit constant, for experimenting.
    //   3. GameFovDegrees, which is almost certainly wrong -- last resort.
    float want;
    if (g_cfg.fgFovSrc > 0 && Readable(obj + (size_t)g_cfg.fgFovSrc, 4))
        want = *(const float*)(obj + (size_t)g_cfg.fgFovSrc);
    else if (g_cfg.fgFovValue > 1.0f)
        want = g_cfg.fgFovValue;
    else
        want = g_cfg.fovDeg;

    if (want < 5.0f || want > 170.0f) return;      // never write nonsense

    static bool announced = false;
    if (!announced)
    {
        announced = true;
        Log(">>> FOVPROBE: writing ForegroundFov at +0x%03X = %.1f (was %.1f)%s",
            (unsigned)off, want, *p,
            g_cfg.fgFovSrc > 0 ? "  [copied from src]" : "");
    }

    if (*p != want) *p = want;
}

// ---- FOV AUTO-DIFF (read only) -------------------------------------------
// Unattended version of the manual snapshot/diff: remember every FOV-plausible
// float on the controller and log the ones that MOVE. Die in a vita chamber and
// the change lands in the log by itself, with its offset.
//
// CORRECTED 2026-08-09: this comment used to justify itself with "PGUP/PGDN
// does not register on this keyboard". False -- the tester uses both routinely.
// The auto-diff is still worth having, because a probe that needs a keypress
// produces nothing when the tester is busy playing; that reason stands and the
// dead-key one never did. M1-S2 acted on the false version and lost a detour.
static void FovAutoDiff(const uint8_t* obj)
{
    static float prev[256] = {};
    static bool  have = false;
    static DWORD last = 0;

    const DWORD t = GetTickCount();
    if (last && (t - last) < 500) return;
    last = t;

    const unsigned lo = 0x300, hi = 0x700;
    int i = 0;
    for (unsigned o = lo; o + 4 <= hi && i < 256; o += 4, ++i)
    {
        if (!Readable(obj + o, 4)) { prev[i] = 0.0f; continue; }
        const float v = *(const float*)(obj + o);
        const bool plausible = (v > 5.0f && v < 170.0f);

        if (have && plausible && prev[i] > 5.0f && prev[i] < 170.0f &&
            (v - prev[i] > 0.5f || prev[i] - v > 0.5f))
        {
            Log(">>> FOVDIFF: +0x%03X  %.1f -> %.1f", o, prev[i], v);
        }
        prev[i] = plausible ? v : 0.0f;
    }
    have = true;
}

// ============================================================================
// M1-S1: MYHUD PROBE -- pin PlayerController.myHUD, self-validated
//
// DIAGNOSTIC ONLY. It reads engine memory, writes none of it, and gates no
// behaviour. One shot: it locks and stops. No standing scan.
//
// WHY THIS FIELD. docs/ARCHITECTURE.md finding 1: Engine.HUD.bHideHUD is
// written from exactly two places in all 1,765 script classes --
// ActionCinematicEnter and ActionCinematicExit -- which makes it an EXACT
// cinematic-mode flag rather than another state inferred from a side effect.
// Eight inferred detectors are already in docs/INVARIANTS.md as falsified.
// Reading it is two pointer hops off the controller this function already
// receives every frame. This session pins the first hop; M1-S2 reads the flag.
//
// ---- ROUTE A: PREDICTED +0x710 -------------------------------------------
// By declaration order from the AActor base 0x450 -- UE2 lays properties out
// in declaration order (docs/UNREALSCRIPT.md). The working, with the
// docs/ENGINE-MAP.md anchors that check it:
//
//   Controller.uc        +0x450  Pawn             <-- anchor
//                        +0x45C  FovAngle         <-- anchor, world FOV
//                        +0x460  ForegroundFov    <-- anchor, foreground FOV
//                        +0x468  SEVENTEEN bools -> ONE dword, not seventeen
//                        +0x46C  six input bytes -> 6 bytes, pad to +0x474
//                        ...     ends at +0x590
//   PlayerController.uc  +0x594  THIRTY-EIGHT bools -> TWO dwords (32 + 6)
//                        +0x5C0  aForward         <-- anchor
//                        +0x5C8  aStrafe          <-- anchor
//                        +0x5D4  xForward         <-- anchor
//                        +0x620  ViewTarget       <-- anchor
//                        +0x648  DesiredFOV       <-- anchor, "world FOV mirror"
//                        +0x674  RenderWorldToCamera, FMatrix, 64 bytes
//                        +0x710  myHUD
//
// SEVEN anchor hits, three of them downstream of both bool packs and of the
// byte padding -- so the arithmetic is checked through every construct that
// could have gone wrong BUT ONE. If FMatrix is 16-byte aligned on this build,
// RenderWorldToCamera starts at +0x680 instead, everything after shifts +0xC,
// and myHUD is +0x71C. The window below covers both and the live read decides.
// That is exactly what route B is for.
//
// TWO CORRECTIONS TO docs/ENGINE-MAP.md, free from this arithmetic:
//   The "acceleration request +0x5C0/+0x5C8" is aForward/aStrafe -- the RAW
//   INPUT AXES. That independently explains its own measured note that the
//   value still read ~875 while pinned in a corner, provably not moving.
//   The +0x620 "Pawn alias" is ViewTarget -- which is why grave 1 (ViewActor
//   divergence) watched it track the pawn for whole sessions.
//
// ---- ROUTE B: THE LIVE READ ----------------------------------------------
// A pointer that merely looks like an object proves nothing. A pointer whose
// target points BACK at the controller we started from proves identity. Same
// three-stage positional trick HandsProbe uses (docs/modules/hands.md).
//
// The back-reference offset is SEARCHED, not assumed, so HUD.PlayerOwner
// (route A predicts +0x470) gets validated in the same pass -- M1-S2 depends
// on the HUD-side arithmetic just as much as on this one.
//
// EXPECT MORE THAN ONE PASSING CANDIDATE. ViewTarget at +0x620 is the pawn,
// and Pawn.Controller points straight back here, so it passes the same test.
// That is a working test, not a broken one. We reject the two objects we can
// already name (the controller itself, and the pawn) and then FAIL CLOSED:
// lock only when the survivor is unambiguous, otherwise log everything and
// lock nothing.
// ============================================================================

static const size_t kMyHudPredicted = 0x710;    // route A, banner above
static const size_t kMyHudLo        = 0x600;    // covers +0x710 and +0x71C
static const size_t kMyHudHi        = 0x800;

// Bounds the back-reference SEARCH inside a candidate. HUD's own fields start
// at the AActor base and PlayerOwner is the 7th of them; this range only says
// where to look, never what to find.
static const size_t kBackrefLo = 0x450;
static const size_t kBackrefHi = 0x550;

static void*  g_myHud       = nullptr;
static size_t g_myHudOff    = 0;
static size_t g_backrefOff  = 0;
static void*  g_myHudOwner  = nullptr;   // controller it was resolved against
static int    g_myHudSettle = 0;
static bool   g_myHudDone   = false;
static int    g_myHudTick   = 0;
static int    g_myHudChecks = 0;

static void MyHudReset()
{
    g_myHud = nullptr;
    g_myHudOff = 0;
    g_backrefOff = 0;
    g_myHudOwner = nullptr;
    g_myHudSettle = 0;
    g_myHudDone = false;
    g_myHudTick = 0;
    g_myHudChecks = 0;
    CineReset();        // the flag reader has the same lifetime as the pointer
}

// The vtable must live inside BioshockHD.exe. GsLooksLikeObject already proves
// the first vtable entry is executable; this rules out a heap block that merely
// happens to begin with a pointer into somewhere executable.
static bool InMainModule(const void* p)
{
    static uint8_t* base = nullptr;
    static size_t   size = 0;
    if (!base)
    {
        HMODULE h = GetModuleHandleW(nullptr);
        MODULEINFO mi = {};
        if (!h || !GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi)))
            return false;
        base = (uint8_t*)mi.lpBaseOfDll;
        size = mi.SizeOfImage;
    }
    const uint8_t* a = (const uint8_t*)p;
    return a >= base && a < base + size;
}

// Lowest offset inside `obj` that points back at `owner`, or 0 for none.
// Offset 0 is the vtable slot, so 0 is unambiguous as "no back-reference".
static size_t FindBackref(const uint8_t* obj, const void* owner)
{
    for (size_t off = kBackrefLo; off + 4 <= kBackrefHi; off += 4)
    {
        if (!Readable(obj + off, 4)) continue;
        if (*(void* const*)(obj + off) == owner) return off;
    }
    return 0;
}

// M1-S2 reads bHideHUD out of a 6-bool DWORD predicted at HUD+0x490 -- and
// bHideHUD is bit 0 of it, sharing with bShowScores, bShowDebugInfo,
// bHideCenterMessages, bBadConnectionAlert and bMessageBeep. Dumping the head
// of the object here is free, read-only, and lets that prediction be checked
// from THIS session's log rather than costing S2 a cycle to discover the HUD
// layout is shifted too. No bit is interpreted here -- that is S2's job.
static void DumpHudHead(const uint8_t* hud)
{
    Log(">>> MYHUD:   object head (for M1-S2; bHideHUD dword predicted +0x490):");
    for (size_t row = 0x450; row < 0x4A0; row += 0x10)
    {
        if (!Readable(hud + row, 0x10))
        {
            Log(">>> MYHUD:   +0x%03X  <unreadable>", (unsigned)row);
            continue;
        }
        const uint32_t* d = (const uint32_t*)(hud + row);
        Log(">>> MYHUD:   +0x%03X  %08X %08X %08X %08X",
            (unsigned)row, d[0], d[1], d[2], d[3]);
    }
}

static void MyHudProbe(const uint8_t* controller)
{
    Log(">>> MYHUD: ---- controller 0x%08X, window +0x%03X..+0x%03X, "
        "route A predicts +0x%03X ----",
        (unsigned)(uintptr_t)controller, (unsigned)kMyHudLo,
        (unsigned)kMyHudHi, (unsigned)kMyHudPredicted);

    const void* pawn = g_pawn;

    size_t hitOff[8] = {};
    void*  hitPtr[8] = {};
    size_t hitBack[8] = {};
    int    nHit = 0;
    int    noise = 0;

    for (size_t off = kMyHudLo; off + 4 <= kMyHudHi; off += 4)
    {
        if (!Readable(controller + off, 4)) continue;
        void* p = *(void* const*)(controller + off);
        if (!p) continue;
        if (!GsLooksLikeObject(p)) continue;

        // Named already, so not myHUD. RealViewTarget is a Controller and can
        // be this very object; ViewTarget is normally the pawn.
        if (p == (const void*)controller)
        {
            Log(">>> MYHUD:   +0x%03X  0x%08X  skip: the controller itself",
                (unsigned)off, (unsigned)(uintptr_t)p);
            continue;
        }
        if (pawn && p == pawn)
        {
            Log(">>> MYHUD:   +0x%03X  0x%08X  skip: the pawn (ViewTarget)",
                (unsigned)off, (unsigned)(uintptr_t)p);
            continue;
        }

        void* vt = *(void* const*)p;
        if (!InMainModule(vt))
        {
            if (++noise <= 32)
                Log(">>> MYHUD:   +0x%03X  0x%08X  reject: vtable 0x%08X is "
                    "outside the module",
                    (unsigned)off, (unsigned)(uintptr_t)p,
                    (unsigned)(uintptr_t)vt);
            continue;
        }

        const size_t back = FindBackref((const uint8_t*)p, controller);
        if (!back)
        {
            if (++noise <= 32)
                Log(">>> MYHUD:   +0x%03X  0x%08X  vt=0x%08X  reject: no "
                    "back-reference to this controller",
                    (unsigned)off, (unsigned)(uintptr_t)p,
                    (unsigned)(uintptr_t)vt);
            continue;
        }

        Log(">>> MYHUD:   +0x%03X  0x%08X  vt=0x%08X  backref=+0x%03X  PASS%s",
            (unsigned)off, (unsigned)(uintptr_t)p, (unsigned)(uintptr_t)vt,
            (unsigned)back, (off == kMyHudPredicted) ? "  <-- ROUTE A" : "");

        if (nHit < 8)
        {
            hitOff[nHit] = off; hitPtr[nHit] = p; hitBack[nHit] = back;
        }
        ++nHit;
    }

    if (noise > 32)
        Log(">>> MYHUD:   ...%d further rejects not logged", noise - 32);

    // FAIL CLOSED. Route A wins outright if it is among the survivors; a lone
    // survivor is accepted as the FMatrix-alignment case; anything else is
    // reported and nothing is locked. Guessing between two plausible offsets
    // is how this project loses cycles.
    int pick = -1;
    const int listed = (nHit < 8) ? nHit : 8;
    for (int i = 0; i < listed; ++i)
        if (hitOff[i] == kMyHudPredicted) pick = i;
    if (pick < 0 && nHit == 1) pick = 0;

    if (pick >= 0)
    {
        g_myHud = hitPtr[pick];
        g_myHudOff = hitOff[pick];
        g_backrefOff = hitBack[pick];

        Log(">>> MYHUD: predicted=+0x%03X confirmed=+0x%03X vtable=0x%08X "
            "backref=%s ok%s",
            (unsigned)kMyHudPredicted, (unsigned)g_myHudOff,
            (unsigned)(uintptr_t)*(void* const*)g_myHud,
            (g_backrefOff == 0x470) ? "+0x470" : "other",
            (g_myHudOff == kMyHudPredicted)
                ? "   ROUTES AGREE"
                : "   route B only -- FMatrix alignment, shift later offsets");

        if (g_backrefOff != 0x470)
            Log(">>> MYHUD: NOTE backref landed at +0x%03X, not the predicted "
                "+0x470 -- the HUD-side arithmetic is off and M1-S2's +0x490 "
                "bHideHUD dword must be re-derived from the dump below.",
                (unsigned)g_backrefOff);

        DumpHudHead((const uint8_t*)g_myHud);
    }
    else if (!nHit)
    {
        Log(">>> MYHUD: NO CANDIDATE PASSED. Route A predicts +0x%03X and "
            "route B found nothing pointing back. Do NOT widen the window and "
            "retry blind -- see .planning/sessions/M1.md, M1-S1 'If it fails'.",
            (unsigned)kMyHudPredicted);
    }
    else
    {
        Log(">>> MYHUD: %d candidates passed and NONE is at the predicted "
            "+0x%03X. Locking nothing -- adjudicate from the list above "
            "before M1-S2.", nHit, (unsigned)kMyHudPredicted);
    }

    Log(">>> MYHUD: ---- end ----");
}

// Drives the probe. One shot per controller, then a short stability watch,
// then silent. Re-runs by itself when the controller changes -- fix lifetime,
// not range (docs/INVARIANTS.md).
static void MyHudTick(const uint8_t* controller)
{
    if (g_myHudOwner && g_myHudOwner != (const void*)controller)
    {
        Log(">>> MYHUD: controller changed 0x%08X -> 0x%08X. Re-resolving.",
            (unsigned)(uintptr_t)g_myHudOwner,
            (unsigned)(uintptr_t)controller);
        MyHudReset();
    }

    if (!g_myHudDone)
    {
        // Let the level settle first, for the same reason the context scan
        // waits: the HUD does not exist during load.
        if (++g_myHudSettle < 600) return;
        g_myHudDone = true;
        g_myHudOwner = (void*)controller;
        MyHudProbe(controller);
        return;
    }

    // "Stable across several seconds of play" -- ten samples about a second
    // apart, logging only a change, then it stops for good.
    if (!g_myHud || g_myHudChecks >= 10) return;
    if ((++g_myHudTick & 0x3F) != 0) return;

    void* now = Readable(controller + g_myHudOff, 4)
              ? *(void* const*)(controller + g_myHudOff) : nullptr;
    if (now != g_myHud)
    {
        Log(">>> MYHUD: UNSTABLE -- +0x%03X was 0x%08X, now 0x%08X, after %d "
            "checks. The offset is wrong or the HUD was replaced.",
            (unsigned)g_myHudOff, (unsigned)(uintptr_t)g_myHud,
            (unsigned)(uintptr_t)now, g_myHudChecks);
        g_myHudChecks = 10;                 // said once, never spammed
        return;
    }

    if (++g_myHudChecks >= 10)
        Log(">>> MYHUD: stable -- 0x%08X unchanged over 10 checks (~10 s).",
            (unsigned)(uintptr_t)g_myHud);
}

// ============================================================================
// M1-S2: THE CINEMATIC FLAG -- read myHUD.bHideHUD, log transitions only
//
// DIAGNOSTIC ONLY. Reads, never writes. GATES NOTHING -- M2-S1 is the first
// session allowed to change what the user sees, and wiring this into the HUD
// or the camera now would make a false positive invisible instead of obvious.
//
// WRITING to bHideHUD is specifically forbidden: it would fight the game's own
// ActionCinematicEnter/Exit logic and confound M2-S2.
//
// THE CLAIM UNDER TEST. docs/ARCHITECTURE.md finding 1 says Engine.HUD.bHideHUD
// is written from exactly two places in all 1,765 script classes --
// ActionCinematicEnter and ActionCinematicExit. If that holds live, this bit is
// an EXACT cinematic-mode flag. Eight previous detectors inferred the state
// from a side effect (pitch rate, draw counts, view-target identity, timing)
// and all eight are in docs/INVARIANTS.md as falsified. This one reads the
// state the game itself keeps, which is the class of approach never tried.
//
// A CLEAN NO IS A SUCCESSFUL SESSION. If the dword never moves, or moves
// constantly in ordinary play, that is the deliverable -- record it and open
// M3-S1. Do not start deriving a signal from pitch or timing again.
//
// WHERE THE BIT IS -- measured M1-S1, not predicted:
//   myHUD           = controller+0x71C   (back-reference at myHUD+0x470)
//   the bool dword  = myHUD+0x490, read 0x00000020 in ordinary play
//   bit 0 bHideHUD          bit 3 bHideCenterMessages
//   bit 1 bShowScores       bit 4 bBadConnectionAlert
//   bit 2 bShowDebugInfo    bit 5 bMessageBeep   <-- the set bit in that 0x20
//
// LOG THE WHOLE DWORD, never just the bit. If a neighbouring bool turns out to
// be what moves, the raw value is the only thing that can tell us.
//
// EXPECTED NOT TO FIRE FOR THE LITTLE SISTER RESCUE. ShockPlayer.uc:2442 pushes
// the NullInput context instead of entering cinematic mode (finding 3). A
// non-firing rescue CONFIRMS the model; it does not break it.
// ============================================================================

static const size_t   kHudBoolDword = 0x490;    // measured M1-S1
static const uint32_t kBitHideHud   = 0x00000001;

static uint32_t g_cineDword = 0;
static bool     g_cineHave  = false;
static bool     g_cineDead  = false;   // identity failed; silent until re-lock
static int      g_cineTick  = 0;
static int      g_markN     = 0;

static void CineReset()
{
    g_cineDword = 0;
    g_cineHave = false;
    g_cineDead = false;
    g_cineTick = 0;
}

// Seconds since the first tick. Every log line is already timestamped; this
// exists only so a CINE line and a MARK line can be compared by eye without
// doing arithmetic on wall clock times.
static float CineTime()
{
    static DWORD t0 = 0;
    const DWORD now = GetTickCount();
    if (!t0) t0 = now;
    return (now - t0) / 1000.0f;
}

// Per-tick guard. Readable() is a VirtualQuery, and this runs on the game
// thread once per CalcView -- doing the full identity check every frame would
// be a few hundred VirtualQuery calls a second for a diagnostic. So: SEH here
// every tick, which cannot fault and costs nothing, and the real identity
// check once a second below.
static bool SafeReadDword(const void* p, uint32_t* out)
{
    __try { *out = *(const uint32_t*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Non-null, vtable plausible and inside the module, back-reference intact --
// the same three stages that locked the pointer in the first place.
static bool CineIdentityOk(const uint8_t* controller)
{
    if (!g_myHud || !g_myHudOff) return false;
    if (!Readable(controller + g_myHudOff, 4)) return false;
    if (*(void* const*)(controller + g_myHudOff) != g_myHud) return false;
    if (!GsLooksLikeObject(g_myHud)) return false;
    if (!InMainModule(*(void* const*)g_myHud)) return false;
    if (!Readable((const uint8_t*)g_myHud + g_backrefOff, 4)) return false;
    return *(void* const*)((const uint8_t*)g_myHud + g_backrefOff)
           == (const void*)controller;
}

static void CineTick(const uint8_t* controller)
{
    if (g_cineDead || !g_myHud) return;

    // FAIL CLOSED: one line, then silence until the controller changes and
    // MyHudReset re-arms us. A detector that keeps reading a stale object is
    // how the CurrentExorcismTarget probe produced 18 meaningless transitions.
    if ((++g_cineTick & 0x3F) == 0 && !CineIdentityOk(controller))
    {
        Log(">>> CINE: identity check FAILED -- myHUD 0x%08X no longer "
            "validates. Reads stopped until the controller changes.",
            (unsigned)(uintptr_t)g_myHud);
        g_cineDead = true;
        return;
    }

    uint32_t d = 0;
    if (!SafeReadDword((const uint8_t*)g_myHud + kHudBoolDword, &d))
    {
        Log(">>> CINE: read faulted at myHUD+0x%03X. Reads stopped.",
            (unsigned)kHudBoolDword);
        g_cineDead = true;
        return;
    }

    if (!g_cineHave)
    {
        g_cineHave = true;
        g_cineDword = d;
        Log(">>> CINE: baseline dword 0x%08X  bHideHUD=%d  (t=%.1f)",
            d, (d & kBitHideHud) ? 1 : 0, CineTime());
        return;
    }

    if (d == g_cineDword) return;               // the common case: silence

    const uint32_t was = g_cineDword;
    g_cineDword = d;

    const char* what;
    if ((was ^ d) & kBitHideHud)
        what = (d & kBitHideHud) ? "   bHideHUD 0->1  CINEMATIC ENTER"
                                 : "   bHideHUD 1->0  CINEMATIC EXIT";
    else
        what = "   (a neighbouring bool moved, NOT bHideHUD)";

    Log(">>> CINE: dword 0x%08X -> 0x%08X  (t=%.1f)%s", was, d,
        CineTime(), what);
}

static void PollProbeKeys(const uint8_t* obj)
{
    // HOME / END -- the numpad is fully taken (grip tuning owns 0,2,4,5,6,7,8;
    // DrawHook owns 1,3,*,/,-; CameraHook owns 9,+,.).
    static bool kH = false, kE = false;

    const bool dH = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    if (dH && !kH) SnapshotFloats(obj);
    kH = dH;

    // CONSOLE GET TEST. END fires four probes: one SET we know works (the
    // control), then three GETs. If any GET comes back with OUTPUT: [...] then
    // the engine can hand us LastPlayerInputContext directly -- an authoritative
    // cutscene signal with no offsets, no scanning and no heuristics.
    static bool kG = false;
    const bool dG = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (dG && !kG)
    {
        Log(">>> GETTEST: ---- console GET probe ----");
        EngineExec_Run("get ShockPlayer bReticleDisabled");
        EngineExec_Run("get ShockPlayer Health");
        EngineExec_Run("get ShockPlayer Location");
        EngineExec_Run("get ShockPlayerController DontUpdateFocus");
        EngineExec_Run("get ShockPlayer HudElementsDisabled");
        Log(">>> GETTEST: ---- end ----");
    }
    kG = dG;

    const bool dE = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
    if (dE && !kE) DiffFloats(obj);
    kE = dE;

    // M1-S1: re-run the myHUD probe. A FALLBACK, not the trigger -- MyHudTick
    // fires by itself once the level settles, because a probe that only fires
    // on a keypress can silently produce nothing and cost the whole cycle.
    // That is not hypothetical here: PGUP and PGDN have never once registered
    // on the tester's board (see the banner in Core/Keybinds.cpp).
    static bool kProbe = false;
    const bool dProbe = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (dProbe && !kProbe)
    {
        Log(">>> MYHUD: manual re-probe (F2).");
        MyHudReset();
        g_myHudSettle = 600;         // no settle wait on a deliberate re-run
    }
    kProbe = dProbe;

    // M1-S2 MARKER. The tester cannot see log timestamps live, so this is the
    // only way a visual event gets pinned to a log line. Numbered, so "the
    // third mark" in a verbal report is findable without counting.
    //
    // MEASURED M1-S2, THE HARD WAY: this was written as Key_Fired(KEY_CINE_MARK)
    // and produced ZERO marks across a full 16-minute test run, because
    // Key_Init is never called and every binding in Core/Keybinds.cpp resolves
    // to VK 0. Direct GetAsyncKeyState, like every hotkey that works. Do not
    // "improve" this back onto the Key_* API until Key_Init has a caller.
    static bool kMark = false;
    const bool dMark = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (dMark && !kMark)
        Log(">>> ===== MARK %d =====  (t=%.1f)", ++g_markN, CineTime());
    kMark = dMark;
}

// ============================================================================
// M7-S1: SCRIPTED-EVENT PROBE -- what brackets a scripted hand animation?
//
// ---- THE HANDS ACTOR IS THE TARGET (added after the corpus research) -------
// Hands.uc:82 declares
//
//     var private bool CurrentlyExecutingScriptedHandAnimationSequence;
//
// set by StartScriptedHandAnimationSequence() and cleared by Stop...(), and
// driven from LEVEL SCRIPTS (ActionStart/StopScriptedHandAnimationSequence) and
// from ANIMATION NOTIFIES (AnimNotify_Start/StopScriptedHandSequence). That is
// precisely "the game is deliberately animating the player's hands as part of a
// scripted moment" -- the condition for unhiding the arms and stopping the
// controller from driving them.
//
// PREDICTED hands+0x594, BIT 2. UE2 packs consecutive bools into one DWORD and
// Hands.uc lines 80-82 are three in a row:
//     bit 0  bFinishedStateAnimations
//     bit 1  AbilityHasBeenReleased
//     bit 2  CurrentlyExecutingScriptedHandAnimationSequence   <-- target
//
// The walk starts from an anchor that WORKING SHIPPING CODE already proves:
// docs/ENGINE-MAP.md records hands+0x494 as the bool DWORD holding
// bIsInWeaponMode (lines 35-38) and hands+0x498 as HandsOffscreenAnimationName
// (line 39) -- and HandsProbe's IdleAnimMode=2 reads +0x498 successfully.
// Lines 39..79 are all name/pointer/int/float, no interfaces and no ambiguity,
// which lands the bool DWORD at +0x594.
//
// A PREDICTION IS NEVER ENOUGH TO TRUST. HandsAnchorOk() below checks the
// computed FName slots read as plausible names before any computed offset is
// believed, and says so in the log when they do not. Same discipline as the
// myHUD back-reference: prove identity, never assume it.
//
// ANIMATION GRANULARITY COMES FREE. CurrentScriptedAnimationName (line 66) and
// the per-animation config names -- InjectingEveAnimationName (line 43, the
// plasmid injection) and ExorcisingGathererAnimationName (line 47, the rescue)
// -- are ALL FNames ON THE SAME OBJECT. So "which animation is playing" is an
// index comparison and never needs the engine name table or a string at all.
// HandsProbe already reads raw FName {Index, Number} pairs; this is that same
// established pattern, not a new mechanism.
//
// ---- the original question, kept ------------------------------------------
// What brackets a Little Sister rescue?
//
// DIAGNOSTIC ONLY. Read-only, default-off, gates nothing. It exists to find ONE
// boolean, because everything downstream of a scripted-event signal is already
// written and already wired -- ArmHide_Update() takes a `hide` flag and restores
// the engine pose when it is false, DriveHands() already early-returns on
// GameState_Cutscene(), InputHook already drops head-relative movement on
// GameState_Theater(). All of it is inert because the signal is always false.
//
// THE NAMED CANDIDATE is ShockPlayerController.bIsForcingPlayerMove -- a lone
// bool, so it gets its own DWORD with no packing ambiguity. ShockPlayerController
// sets it on the line IMMEDIATELY AFTER ConsoleCommand("PUSHINPUTCONTEXT
// NullInput"), which makes it a proxy for the exact state nine graves have
// chased. docs/ARCHITECTURE.md already flagged it as the obvious Tier 0 target.
//
// WHY A DIFFERENTIAL SCAN AND NOT A COMPUTED OFFSET. Two reasons, either alone
// sufficient:
//
//   1. The offset cannot be computed reliably. SIX interface-typed fields
//      (ICanBeUsed, ICanBeFocused, ICanBeHacked) plus a TArray sit between the
//      class base and the flag, and interface size in this fork is UNKNOWN.
//      That unknown compounds six times.
//   2. The rescue may not set it at all. The StartForcePlayerMove callers in the
//      corpus are object-use placement and the Atlas Adam drain -- the rescue is
//      NOT among them. Reading one predicted field could return a flat line and
//      spend the cycle proving nothing.
//
// A differential is immune to both: it needs no offset, and if the named flag
// never moves it still reports whatever did.
//
// IT ALSO TESTS A SECOND OPEN LEAD FOR FREE. ShockPlayer.CurrentExorcismTarget
// is a POINTER set at the start of a rescue and cleared at the end
// (docs/modules/gamestate.md), unresolved with +0xEA4 and +0xB58 surviving. The
// pawn window below covers both, so one run with an actual rescue in it tests
// them too.
//
// THE BUG THAT RUINED THE LAST PROBE: the previous exorcism probe produced 18
// transitions from a run containing no rescue, because its snapshot was not
// reset when the pawn changed and the pawn-lock burst polluted everything.
// ForcedMoveReset() is called from RefreshPawn for exactly that reason, and the
// owner pointers below are re-checked every sample.
// ============================================================================

// ShockPlayerController's own fields begin after PlayerController's, and
// PlayerController's myHUD is measured at +0x71C (M1-S1). The pawn window
// brackets both surviving CurrentExorcismTarget candidates.
// M7-S5: WIDENED. ShockPlayerController's own fields start at the END of
// PlayerController, and myHUD at +0x71C is NOT the last of those -- so the old
// +0xB00 ceiling may well have stopped short of the class we care about.
static const size_t kFmCtlLo = 0x700, kFmCtlHi = 0x1000;   // 320 dwords
static const size_t kFmPawnLo = 0xA80, kFmPawnHi = 0xF00;   // 288 dwords
static const size_t kFmHndLo = 0x480, kFmHndHi = 0x600;    // 96 dwords

// Computed from the +0x494/+0x498 anchor, all on the Hands actor.
static const size_t kHndOffscreenName = 0x498;   // line 39, the proven anchor
static const size_t kHndInjectEveName = 0x4B8;   // line 43, plasmid injection
static const size_t kHndExorcName = 0x4D8;   // line 47, the rescue
static const size_t kHndCurScriptName = 0x558;   // line 66, what is playing NOW
static const size_t kHndScriptedBits = 0x594;   // lines 80-82, bit 2 is ours
static const uint32_t kScriptedBit = 1u << 2;

static const int kFmCtlN = (int)((kFmCtlHi - kFmCtlLo) / 4);
static const int kFmPawnN = (int)((kFmPawnHi - kFmPawnLo) / 4);
static const int kFmHndN = (int)((kFmHndHi - kFmHndLo) / 4);
// ---- M7-S5: PER-WINDOW CAPS, AND BOOLS ONLY ------------------------------
// MEASURED last run: a single shared 200-line budget was exhausted in SIX
// SECONDS by the controller and pawn windows (117/256 and 206/288 fields
// non-zero and churning), so the differential was dead long before the event it
// was built to catch. One noisy window must not be able to spend another
// window's budget.
//
// And the target is a LONE BOOL, so `othr` transitions are pure noise here.
// Logging only 0<->1 removes almost all of it at a stroke; set
// ForcedMoveProbeAll=1 to see everything again.
static const int kFmMaxLogPerWindow = 120;

static uint32_t g_fmCtlSnap[kFmCtlN] = {};
static uint32_t g_fmPawnSnap[kFmPawnN] = {};
static uint32_t g_fmHndSnap[kFmHndN] = {};
static const void* g_fmCtlOwner = nullptr;
static const void* g_fmPawnOwner = nullptr;
static const void* g_fmHndOwner = nullptr;
static uint32_t    g_fmScriptedPrev = 0;
static bool        g_fmAnchorOk = false;
static bool        g_fmAnchorLogged = false;
static int    g_fmSettle = 0;
static int    g_fmLogged = 0;
static DWORD  g_fmLastSample = 0;
static DWORD  g_fmLastBeat = 0;
static bool   g_fmWarnedCtl = false;
static bool   g_fmWarnedPawn = false;
static bool   g_fmWarnedHnd = false;

static void ForcedMoveReset()
{
    g_fmCtlOwner = nullptr;
    g_fmPawnOwner = nullptr;
    // The Hands actor is DESTROYED on level and save load -- ArmHide already
    // carries that scar (ArmHide_Reset deliberately does not restore first,
    // because the old actor may already be freed and its address reused).
    g_fmHndOwner = nullptr;
    g_fmScriptedPrev = 0;
    g_fmAnchorOk = false;
    g_fmAnchorLogged = false;
    g_fmSettle = 0;
    g_fmWarnedCtl = false;
    g_fmWarnedPawn = false;
    g_fmWarnedHnd = false;
    // g_fmLogged deliberately NOT reset -- the cap is per RUN, not per pawn, so
    // a level load cannot buy another 200 lines of noise.
}

// A bool DWORD is exactly 0 or exactly 1. Anything else is a float, a counter or
// a pointer, and is ranked below.
static bool FmBoolish(uint32_t v) { return v == 0 || v == 1; }

static const char* FmKind(uint32_t prev, uint32_t cur)
{
    if (FmBoolish(prev) && FmBoolish(cur)) return "BOOL";

    const bool pPtr = prev && GsLooksLikeObject((void*)(uintptr_t)prev);
    const bool cPtr = cur && GsLooksLikeObject((void*)(uintptr_t)cur);
    if ((prev == 0 && cPtr) || (pPtr && cur == 0)) return "PTR ";

    return "othr";
}

// Diff one window. Returns how many fields are currently non-zero, for the beat.
static int FmScanWindow(const uint8_t* base, size_t lo, size_t hi,
    uint32_t* snap, int n, const void** owner, bool* warned,
    const char* tag, int* logged)
{
    if (!base) { *owner = nullptr; return -1; }

    if (!Readable(base + lo, hi - lo))
    {
        if (!*warned)
        {
            *warned = true;
            Log(">>> SCRIPTED: %s window +0x%03X..+0x%03X not readable on "
                "0x%08X. Skipping it.",
                tag, (unsigned)lo, (unsigned)hi, (unsigned)(uintptr_t)base);
        }
        *owner = nullptr;
        return -1;
    }

    const uint32_t* live = (const uint32_t*)(base + lo);

    // First sight of this object: take the baseline, report nothing. A fresh
    // object's every field would otherwise read as a transition.
    if (*owner != (const void*)base)
    {
        for (int i = 0; i < n; ++i) snap[i] = live[i];
        *owner = (const void*)base;
        Log(">>> SCRIPTED: %s baseline taken on 0x%08X (+0x%03X..+0x%03X)",
            tag, (unsigned)(uintptr_t)base, (unsigned)lo, (unsigned)hi);
        return -1;
    }

    int nonZero = 0;
    for (int i = 0; i < n; ++i)
    {
        const uint32_t cur = live[i];
        if (cur) ++nonZero;

        const uint32_t prev = snap[i];
        if (cur == prev) continue;
        snap[i] = cur;

        const char* kind = FmKind(prev, cur);

        // The target is a lone bool, so anything else is noise that would eat
        // this window's budget before the event arrives -- which is exactly
        // what happened last run.
        if (!g_cfg.forcedMoveProbeAll && kind[0] != 'B') continue;

        if (*logged >= kFmMaxLogPerWindow) continue;
        ++(*logged);
        ++g_fmLogged;                    // total, for the beat line only

        Log(">>> SCRIPTED: %s %s +0x%03X  %08X -> %08X",
            kind, tag, (unsigned)(lo + (size_t)i * 4), prev, cur);
    }
    return nonZero;
}

// An FName is {Index, Number}. A real one has a non-zero index inside the name
// table and, for the plain authored names these config slots hold, Number == 0.
// Index 0 is 'None' -- and writing it is what hangs the game thread, per the
// IDLE guard in HandsProbe, so treating it as "not a name" is consistent.
static bool FmPlausibleName(const uint8_t* hands, size_t off)
{
    if (!Readable(hands + off, 8)) return false;
    const uint32_t idx = *(const uint32_t*)(hands + off);
    const uint32_t num = *(const uint32_t*)(hands + off + 4);
    return idx != 0 && idx < 0x00100000 && num == 0;
}

// PROVE THE WALK BEFORE BELIEVING ANY COMPUTED OFFSET. Every slot below is a
// config'd FName that the shipped game fills in, so on a correct walk they all
// read as plausible names. If they do not, the declaration walk is shifted and
// hands+0x594 is pointing at something else entirely -- say so loudly and fall
// back to the raw differential, which needs no offsets to be right.
static void HandsAnchorCheck(const uint8_t* hands)
{
    if (g_fmAnchorLogged) return;
    g_fmAnchorLogged = true;

    const size_t slots[4] = { kHndOffscreenName, kHndInjectEveName,
                              kHndExorcName, kHndCurScriptName };
    const char* names[4] = { "HandsOffscreen(+0x498, the anchor)",
                              "InjectingEve(+0x4B8)",
                              "ExorcisingGatherer(+0x4D8)",
                              "CurrentScripted(+0x558)" };
    int ok = 0;
    for (int i = 0; i < 4; ++i)
    {
        const bool good = FmPlausibleName(hands, slots[i]);
        uint32_t idx = 0, num = 0;
        if (Readable(hands + slots[i], 8))
        {
            idx = *(const uint32_t*)(hands + slots[i]);
            num = *(const uint32_t*)(hands + slots[i] + 4);
        }
        Log(">>> SCRIPTED: anchor %-38s idx %u num %u  %s",
            names[i], idx, num, good ? "ok" : "IMPLAUSIBLE");
        if (good) ++ok;
    }

    // CurrentScripted is legitimately 'None' when nothing is playing, so it is
    // not required. The three config'd names are.
    g_fmAnchorOk = FmPlausibleName(hands, kHndOffscreenName) &&
        FmPlausibleName(hands, kHndInjectEveName) &&
        FmPlausibleName(hands, kHndExorcName);

    if (g_fmAnchorOk)
    {
        Log(">>> SCRIPTED: anchor PASSED (%d/4). Watching hands+0x%03X bit 2 "
            "for CurrentlyExecutingScriptedHandAnimationSequence.",
            ok, (unsigned)kHndScriptedBits);
    }
    else
    {
        Log(">>> SCRIPTED: anchor FAILED (%d/4). The declaration walk is "
            "SHIFTED -- ignoring every computed offset. The raw differential "
            "below still stands; it needs no offset to be right.", ok);
    }
}

// The M7-S1 bit watch lived here. It has been promoted into ScriptedAnimTick
// below, which runs unconditionally and publishes the result -- the signal is
// production now rather than a probe finding. Its per-transition dump of bits 0
// and 1 was what confirmed the bit layout; that job is done.
//
// MEASURED and NOT kept: CurrentScriptedAnimationName (+0x558) read 'None'
// (index 0) for the entire run, including throughout the scripted sequence. So
// animation-level naming does NOT come from that field, and the index-comparison
// idea it was going to enable is unproven. Do not re-add it without new evidence.

// ============================================================================
// M7-S2: THE PUBLISHED SCRIPTED-ANIMATION SIGNAL
//
// MEASURED M7-S1, 2026-08-10: hands+0x594 bit 2 set 0.8s after the tester
// marked a scripted scene starting and cleared 0.75s before they marked it
// ending -- both inside reaction time on the marker key -- and fired EXACTLY
// ONCE in six minutes covering weapon fire, plasmid fire, four gene-machine
// opens, a Little Sister rescue and walking. Zero false positives.
//
// WHAT IT DOES NOT COVER, measured in the same run: the Little Sister RESCUE
// and the EVE INJECTION both left this bit clear. They are Hands *states*
// (ExorcisingGatherer, InjectingEve), not scripted sequences -- which is why
// ShockPlayer.uc:2091 tests two separate conditions. Widening to those needs
// the Hands state, not this bit. Do not assume this covers them.
//
// THIS RUNS UNCONDITIONALLY, like MyHudTick/CineTick: it is one DWORD read
// after a one-time anchor check, it is read-only, and it gates nothing by
// itself. Consumers gate on g_cfg.scriptedQol.
//
// THREADING: written here on the GAME thread, read from the RENDER thread by
// CameraHook_LateHandsWrite. One aligned long through _InterlockedExchange, the
// same channel g_paused and g_cutscene use.
// ============================================================================

static long g_scriptedAnim = 0;

bool GameState_ScriptedAnim() { return g_scriptedAnim != 0; }

// ============================================================================
// M7-S4: BATHYSPHERE MODE -- computed, with a two-bit oracle
//
// ActionEnableBathysphereModeForPlayer sets THREE fields on ShockPlayer for the
// duration of a ride, and two of them land in the same DWORD:
//
//     Player.bUseHavokRigidBodyCapsuleCollisions = false;
//     Player.bUseHavokPhantomCollisions          = false;
//     Player.bCannotFall                         = true;
//
// bCannotFall is Engine/Classes/Pawn.uc:46, and Pawn's own fields start at the
// AActor base 0x450. UE2 packs consecutive bools, and lines 13..44 are EXACTLY
// 32 of them -- one full DWORD -- so the next three start a fresh one:
//
//   +0x450 Controller   +0x458 LastRealViewer
//   +0x454 NetRelevancy +0x45C LastViewer
//   +0x460 lines 13..44, thirty-two bools, one DWORD exactly
//   +0x464 bit 0 ShouldNotTakeDamageOnNextLanding
//          bit 1 bCannotFall                            <-- the target
//          bit 2 bUseHavokRigidBodyCapsuleCollisions
//   +0x468 HavokRigidBodyCapsuleCollisionExtraRadius (float, ends the run)
//
// THE ORACLE: ShockPlayer defaults bUseHavokRigidBodyCapsuleCollisions to TRUE
// (ShockPlayer.uc:7094), and bathysphere mode clears it in the same call that
// sets bCannotFall. So entering a ride must flip BIT 1 UP AND BIT 2 DOWN IN THE
// SAME WRITE. Two bits moving in opposite directions at once is not something a
// wrong offset produces by chance.
//
// Read-only, and it gates only FreezeGameplayRotation -- which ships at 0 until
// this has been seen to work in a log.
// ============================================================================

static const size_t kPawnFlagsB = 0x464;
static const uint32_t kCannotFallBit = 1u << 1;
static const uint32_t kHavokCapsuleBit = 1u << 2;

static long g_bathysphere = 0;

bool GameState_Bathysphere() { return g_bathysphere != 0; }

static void BathysphereTick()
{
    const uint8_t* pawn = (const uint8_t*)g_pawn;
    if (!pawn || !Readable(pawn + kPawnFlagsB, 4))
    {
        if (g_bathysphere) _InterlockedExchange(&g_bathysphere, 0);
        return;
    }

    const uint32_t b = *(const uint32_t*)(pawn + kPawnFlagsB);
    const long want = (b & kCannotFallBit) ? 1 : 0;

    // Sanity, once per pawn: in ordinary play the capsule bit should be SET and
    // bCannotFall clear. If that does not hold the walk is shifted and this
    // offset is pointing at something else -- say so rather than quietly
    // gating behaviour on nonsense.
    static const void* s_checked = nullptr;
    if (s_checked != (const void*)pawn)
    {
        s_checked = (const void*)pawn;
        Log(">>> BATHY: pawn+0x%03X = %08X  bCannotFall=%d capsule=%d  %s",
            (unsigned)kPawnFlagsB, b,
            (b & kCannotFallBit) ? 1 : 0, (b & kHavokCapsuleBit) ? 1 : 0,
            (b & kHavokCapsuleBit) ? "(capsule set, as expected on foot)"
            : "!!! capsule CLEAR on foot -- offset may be wrong");
    }

    if (want != g_bathysphere)
    {
        _InterlockedExchange(&g_bathysphere, want);
        Log(">>> BATHY: %s  (pawn+0x%03X = %08X, capsule=%d)",
            want ? "*** BATHYSPHERE MODE ON ***" : "--- bathysphere mode off ---",
            (unsigned)kPawnFlagsB, b, (b & kHavokCapsuleBit) ? 1 : 0);
    }
}

static void ScriptedAnimTick()
{
    const uint8_t* hands = (const uint8_t*)HandsProbe_Get();
    if (!hands)
    {
        // Hands actor gone (level load, save reload). Fail closed: a stale
        // "scripted" would leave the arms unhidden and the hands frozen with
        // nothing on screen to explain why.
        if (g_scriptedAnim) _InterlockedExchange(&g_scriptedAnim, 0);
        return;
    }

    // ITS OWN owner tracker, deliberately NOT g_fmHndOwner. That one is only
    // written by the probe's window scan, so with the probe off it would stay
    // null, the actor would compare "changed" every single tick, and the anchor
    // block would re-log four lines four times a second forever.
    static const void* s_owner = nullptr;
    if (hands != s_owner)
    {
        s_owner = hands;
        g_fmAnchorLogged = false;
        g_fmAnchorOk = false;
    }
    HandsAnchorCheck(hands);
    if (!g_fmAnchorOk)
    {
        if (g_scriptedAnim) _InterlockedExchange(&g_scriptedAnim, 0);
        return;
    }

    if (!Readable(hands + kHndScriptedBits, 4)) return;
    const uint32_t bits = *(const uint32_t*)(hands + kHndScriptedBits);
    const long want = (bits & kScriptedBit) ? 1 : 0;

    if (want != g_scriptedAnim)
    {
        _InterlockedExchange(&g_scriptedAnim, want);
        Log(">>> SCRIPTED: %s  (hands+0x%03X = %08X)",
            want ? "*** SCRIPTED ANIMATION BEGAN ***"
            : "--- scripted animation ended ---",
            (unsigned)kHndScriptedBits, bits);
    }

    // ============================================================================
    //  FALSIFIED, M7-S3: bit 0 IS NOT "an animation is playing".
    //
    // It is bFinishedStateAnimations, and it looked perfect: Hands.uc sets it
    // false immediately before PlayAnimation... and true right after
    // FinishAnimation returns. Gating the arms on it produced the OPPOSITE
    // failure in two different scenes, which is what gave it away:
    //
    //   Little Sister crawl  arms hidden the WHOLE time, bottle catch included
    //   Plasmid balcony      arms visible, then stuck visible and frozen
    //
    // The corpus explains both. `state PlayingScriptedHandAnimation` has an
    // EMPTY BODY -- it never touches the flag -- so during the crawl scene the
    // flag kept the `true` left over from the last weapon state. `state
    // InjectingEve` DOES set it false, so the balcony scene showed arms and then
    // held them once the flag was left low.
    //
    // So bit 0 tracks the Hands STATE MACHINE's own animations and says nothing
    // about scripted ones. There is no script-side flag for those either:
    // ScriptedHandsAnimationHandle is only ever assigned (Hands.uc:1002), never
    // cleared or validity-tested.
    //
    // REPLACED BY ArmHide_HandMotion() -- measuring whether the rig is actually
    // moving, which answers the real question regardless of mechanism.
    // DO NOT re-add a gate on this bit.
    // ============================================================================
}

static void ForcedMoveTick(const uint8_t* controller)
{
    if (!g_cfg.forcedMoveProbe) return;
    if (!controller) return;

    // Let the level settle before taking a baseline, the same way MyHudTick
    // does. A baseline captured mid-load is a baseline of a half-built object.
    if (g_fmSettle < 600) { ++g_fmSettle; return; }

    // 4 Hz. This is a 2KB read of two bounded windows, not a memory scan, and it
    // is default-off -- but it is still the only periodic diff in the mod, so it
    // is throttled hard and says so.
    const DWORD now = GetTickCount();
    if (now - g_fmLastSample < 250) return;
    g_fmLastSample = now;

    static int s_ctlLogged = 0, s_pawnLogged = 0, s_hndLogged = 0;

    const int ctlNz = FmScanWindow(controller, kFmCtlLo, kFmCtlHi,
        g_fmCtlSnap, kFmCtlN, &g_fmCtlOwner, &g_fmWarnedCtl, "ctl",
        &s_ctlLogged);
    const int pawnNz = FmScanWindow((const uint8_t*)g_pawn, kFmPawnLo, kFmPawnHi,
        g_fmPawnSnap, kFmPawnN, &g_fmPawnOwner, &g_fmWarnedPawn, "pwn",
        &s_pawnLogged);

    // THE HANDS ACTOR -- the target this probe was refocused onto. Ordinary
    // weapon handling moves fields here, so this window proves the probe is
    // alive and looking at the right object before any cutscene is involved.
    // The anchor check and the bit watch moved to ScriptedAnimTick, which runs
    // unconditionally -- the signal is production now, not a probe finding.
    const uint8_t* hands = (const uint8_t*)HandsProbe_Get();
    int hndNz = -1;
    if (hands)
        hndNz = FmScanWindow(hands, kFmHndLo, kFmHndHi,
            g_fmHndSnap, kFmHndN, &g_fmHndOwner, &g_fmWarnedHnd, "hnd",
            &s_hndLogged);

    // A run with no transitions must be distinguishable from a probe that never
    // armed. Once a second, say we are alive and what we are watching.
    if (now - g_fmLastBeat >= 1000)
    {
        g_fmLastBeat = now;
        Log(">>> SCRIPTED: beat  ctl %d/%d(%d)  pawn %d/%d(%d)  hands %d/%d(%d)"
            "  anchor %s  bools %d",
            ctlNz, kFmCtlN, s_ctlLogged, pawnNz, kFmPawnN, s_pawnLogged,
            hndNz, kFmHndN, s_hndLogged,
            hands ? (g_fmAnchorOk ? "ok" : "FAILED") : "no-hands", g_fmLogged);
    }
}

void GameState_Reset()
{
    _InterlockedExchange(&g_locked, 0);
    _InterlockedExchange(&g_class, CTX_UNKNOWN);
    g_scanDone = false;
    g_observeCalls = 0;
    g_offset = 0;
    g_nCand = 0;
    g_recheck = 0;
    MyHudReset();       // level load / save reload: re-resolve, never carry over
}