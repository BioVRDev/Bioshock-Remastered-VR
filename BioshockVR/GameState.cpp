// BioshockVR/GameState.cpp
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

#include "GameState.h"

#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

extern void LogFile(const char* msg);
extern bool  g_cfgGameState;      // EnableGameState, default 1
extern int   g_cfgFgFovOffset;    // ForegroundFovOffset, 0 == off
extern float g_cfgFgFovValue;     // ForegroundFovValue, 0 == use src/GameFovDegrees
extern int   g_cfgFgFovSrc;       // ForegroundFovSrcOffset, 0 == off
extern float g_cfgFovDeg;         // GameFovDegrees

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
    { "BathysphereUIActive",               CTX_SCRIPTED },

    { "PauseUIActive",                     CTX_MENU },
    { "InventoryUIActive",                 CTX_MENU },
    { "ContainerUIActive",                 CTX_MENU },
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

void GameState_Observe(void* playerController)
{
    if (!playerController) return;

    // S49: the FOV probe and the ForegroundFovAngle write run REGARDLESS of
    // EnableGameState. They only ever lived in this function because the
    // controller pointer was handy here; gating them on the (dead) context scan
    // meant EnableGameState=0 silently reverted the weapon to 60-degree
    // foreground FOV. Different feature, different switch.
    PollProbeKeys((const uint8_t*)playerController);
    ApplyForegroundFov((const uint8_t*)playerController);

    if (!g_cfgGameState) return;

    // Prefer the Pawn once we have it: that is where the context string lives.
    // The FOV floats stay on the CONTROLLER, so the probe below keeps using it.
    const uint8_t* obj = g_pawn ? (const uint8_t*)g_pawn
        : (const uint8_t*)playerController;

    if (!g_scanDone)
    {
        // Let the game settle. A controller observed on the very first CalcView
        // may not have run an input-context push yet.
        if (++g_observeCalls < 300) return;
        g_scanDone = true;
        ScanForContextField(obj);
    }

    if (!g_locked)
    {
        // ~every 2s at 118Hz. Cheap, and it means you only have to open a menu
        // once for us to find the field.
        if (++g_recheck >= 240) { g_recheck = 0; RecheckCandidates(obj); }
        return;
    }

    char val[96] = {};
    if (!ReadFStringAt(obj, g_offset, val, sizeof(val)))
    {
        // Unreadable -- most likely a different controller instance after a
        // level load. Re-scan rather than publish stale state.
        static int misses = 0;
        if (++misses > 240)
        {
            misses = 0;
            Log(">>> GAMESTATE: context field unreadable; re-scanning.");
            _InterlockedExchange(&g_locked, 0);
            _InterlockedExchange(&g_class, CTX_UNKNOWN);
            g_scanDone = false;
            g_observeCalls = 0;
        }
        return;
    }

    const ContextClass c = ClassifyContext(val);
    const long prev = g_class;

    if (c == CTX_UNKNOWN)
    {
        // Live field, unrecognised value. Log once so a missing context name
        // gets added to the table instead of silently passing as gameplay.
        static char lastUnknown[96] = {};
        if (strcmp(lastUnknown, val) != 0)
        {
            strncpy_s(lastUnknown, val, _TRUNCATE);
            Log(">>> GAMESTATE: unknown context \"%s\" -- treating as gameplay", val);
        }
        _InterlockedExchange(&g_class, CTX_GAMEPLAY);
    }
    else
    {
        _InterlockedExchange(&g_class, (long)c);
    }

    PublishName(val);

    if ((long)c != prev && c != CTX_UNKNOWN)
    {
        static const char* kClsName[] = { "UNKNOWN", "GAMEPLAY", "RADIAL", "SCRIPTED", "MENU" };
        Log(">>> GAMESTATE: context -> \"%s\"  [%s]", val, kClsName[c]);
    }
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
        "-- zoom the pistol -- and press Numpad 5.", n);
}

static void DiffFloats(const uint8_t* obj)
{
    if (!g_snapValid)
    {
        Log(">>> FOVPROBE: no snapshot yet. Press Numpad 4 first.");
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
    if (g_cfgFgFovOffset <= 0) return;

    const size_t off = (size_t)g_cfgFgFovOffset;
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
    if (g_cfgFgFovSrc > 0 && Readable(obj + (size_t)g_cfgFgFovSrc, 4))
        want = *(const float*)(obj + (size_t)g_cfgFgFovSrc);
    else if (g_cfgFgFovValue > 1.0f)
        want = g_cfgFgFovValue;
    else
        want = g_cfgFovDeg;

    if (want < 5.0f || want > 170.0f) return;      // never write nonsense

    static bool announced = false;
    if (!announced)
    {
        announced = true;
        Log(">>> FOVPROBE: writing ForegroundFov at +0x%03X = %.1f (was %.1f)%s",
            (unsigned)off, want, *p,
            g_cfgFgFovSrc > 0 ? "  [copied from src]" : "");
    }

    if (*p != want) *p = want;
}

static void PollProbeKeys(const uint8_t* obj)
{
    static bool k4 = false, k5 = false;

    const bool d4 = (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
    if (d4 && !k4) SnapshotFloats(obj);
    k4 = d4;

    const bool d5 = (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
    if (d5 && !k5) DiffFloats(obj);
    k5 = d5;
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
}