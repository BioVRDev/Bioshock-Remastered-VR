// BioshockVR/Hands/HandsProbe.cpp
//
// S42. The first version PREDICTED AActor::Location at +0x1D8, reasoning from
// the aim field at +0x1E4. That was wrong -- +0x1D8 held a position 141 m from
// the camera. The mistake was reasoning from +0x1E4 at all: it is one of two
// GUESSED candidates in CameraHook (kAimOffsets = { 0x1E4, 0x328 }) that
// happened to work for aiming. Working for aiming does not make it the actor
// member, so nothing could be derived from its neighbours.
//
// So: search, do not predict.
//
// STAGE A  Find AActor::Location AND the Pawn in one pass. For every aligned
//          pointer in the controller, look inside the target for three
//          consecutive floats near the camera. Camera coordinates are large and
//          distinctive (-6344.7, 4467.8, 2627.3) -- a three-axis match within a
//          few metres does not happen by accident. When several different
//          objects report a hit at the SAME internal offset, that offset is
//          AActor::Location, confirmed by agreement rather than by assumption.
//
// STAGE B  Find Hands. Using the discovered Location offset, look for an object
//          hanging off the Pawn that sits essentially ON the camera -- Hands
//          does SetLocation(~camera) every frame, so it is far closer than the
//          Pawn itself (which is an eye-height below).
//
// STAGE C  NUDGE TEST. Write Location, see whether the arms move. This is the
//          question the whole route depends on and it is worth reaching quickly.
//
// PERFORMANCE: Readable() is a VirtualQuery and is expensive. It is called ONCE
// per candidate object for a whole block, never per triple -- doing it per
// triple would be a quarter of a million VirtualQuery calls per scan and would
// hitch the game thread visibly.

#include "Hands/HandsProbe.h"
#include "Hands/ArmHide.h"
#include "Input/Swing.h"

#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include "Core/Config.h"

extern void LogFile(const char* msg);

// ---- S64/S65: the weapon's own actor, and its children ---------------------
// MEASURED S64: hands+0x45C is the weapon actor, and writing 0.50 into its
// DrawScale halved the gun -- but NOT the cylinder. So part of the weapon is a
// further actor attached to it, carrying its own scale. Same hunt, one level
// down.
//
// The settings that drive both sweeps -- gunScale, gunPtrOff, gunPtrBase,
// gunChildren, handsPtrOff, handsPosOff -- are documented on their fields in
// Core/Config.h.

void GameState_SetPawn(void* pawn);   // GameState.cpp
bool GameState_InGame();              // GameState.cpp

static void Log(const char* fmt, ...)
{
    char b[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

struct AVec { float x, y, z; };

// AActor::Location is DISCOVERED at runtime (g_locOff) -- measured at +0x1A0.
// Rotation is predicted to sit immediately after it, since UE2 lays Location
// then Rotation. Note this means +0x1E4 -- the aim field the camera hook writes
// -- is NOT AActor::Rotation but some other rotator on the controller, which is
// why deriving Location from it back in S42 failed. Predicted, not measured:
// STAGE B tests it, and a wall of large/absent rotation errors means this
// number is wrong rather than meaning Hands is unfindable.
// MEASURED S50, three turns, four objects, err 0.0-0.2 deg: the yaw component
// tracks at +0x1E8, so the rotator STARTS at +0x1E4. That is the offset the
// camera hook has written as the aim field since Phase 8 -- my original guess
// was right, and "correcting" it to +0x1AC (by assuming Rotation follows
// Location) is what made the last two probes find nothing.
static const size_t kActorRotation = 0x1E4;

// MEASURED S63, by writing 0.50 and watching the arm:
//
//   +0x2AC  DrawScale        uniform -- the whole arm got half sized
//   +0x2B0  DrawScale3D.X    the arm got thin
//   +0x2B4  DrawScale3D.Y    the arm got squashed vertically
//   +0x2B8  DrawScale3D.Z    the arm got shorter / stopped reaching as far
//
// That is exactly `float DrawScale; vector DrawScale3D;` -- the standard AActor
// pair -- confirmed one axis at a time rather than inferred. S58 wrote 0.70 to
// this same offset and reported "no change", which we now know was a stale
// build, not a wrong offset. Check the build stamp.
//
// This is a CLASS layout, so it is the same number on every actor in the game,
// including the weapon. That is the whole basis of the S64 hunt below.
static const size_t kActorDrawScale = 0x2AC;

// AActor::Location on the Hands object, measured at 0.0 cm from the camera.
static const size_t kActorLocation = 0x1D8;

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

// Every UObject begins with a pointer to its vtable.
//
// S48: the first version of this test required the VTABLE POINTER to land in
// executable memory. It does not -- a vtable is DATA. It lives in .rdata, which
// is PAGE_READONLY, and it CONTAINS pointers to code. Checking one level too
// shallow rejected nearly every real object: the 19:11 scan of a whole
// ShockPlayer reported "1 real object", when the pawn points at Hands, seven
// Holdables, and half a dozen managers. Pass 2 was then testing an empty room.
//
// Two levels: object -> vtable (readable) -> first virtual function (executable).
static bool LooksLikeObject(const void* p)
{
    if (!p || ((uintptr_t)p & 3) != 0 || (uintptr_t)p < 0x10000) return false;
    if (!Readable(p, 4)) return false;

    const void* vt = *(const void* const*)p;
    if (!vt || ((uintptr_t)vt & 3) != 0) return false;
    if (!Readable(vt, 4)) return false;                 // .rdata, read-only

    const void* fn = *(const void* const*)vt;           // first virtual function
    if (!fn) return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(fn, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;

    switch (mbi.Protect & 0xFF)
    {
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

static bool Writable(const void* p, size_t n)
{
    if (!p || !n) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (mbi.Protect & 0xFF)
    {
    case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
    default: return false;
    }
    const uint8_t* rs = (const uint8_t*)mbi.BaseAddress;
    const uint8_t* re = rs + mbi.RegionSize;
    const uint8_t* a = (const uint8_t*)p;
    return (a >= rs) && (a + n <= re);
}

// Largest readable block at p, up to want. One VirtualQuery, not many.
static size_t ReadableBlock(const void* p, size_t want)
{
    while (want >= 0x40)
    {
        if (Readable(p, want)) return want;
        want >>= 1;
    }
    return 0;
}

static double Dist(const AVec* v, const float c[3])
{
    if (v->x != v->x || v->y != v->y || v->z != v->z) return -1.0;   // NaN
    const double dx = (double)v->x - c[0];
    const double dy = (double)v->y - c[1];
    const double dz = (double)v->z - c[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

// Closest camera-like float triple inside an already-validated block.
static bool FindVecNear(const uint8_t* obj, size_t blockSz, const float cam[3],
    double maxCm, size_t* outOff, double* outDist)
{
    bool got = false;
    double best = 1e18;
    size_t bestOff = 0;

    for (size_t off = 0; off + sizeof(AVec) <= blockSz; off += 4)
    {
        const double d = Dist((const AVec*)(obj + off), cam);
        if (d < 0.0 || d > maxCm) continue;
        if (d < best) { best = d; bestOff = off; got = true; }
    }

    if (got) { *outOff = bestOff; *outDist = best; }
    return got;
}

// ---------------------------------------------------------------- state

static size_t g_locOff = 0;          // AActor::Location, discovered
static void* g_pawn = nullptr;
static void* g_hands = nullptr;
static int   g_calls = 0;
static int   g_retry = 0;

static const size_t kPtrScan = 0x1000;  // controller: how far to look for pointers
static const size_t kObjScan = 0x400;   // how far into a target to look
// S43: ShockPlayer is a BIG class (ShockPawn + Pawn + Actor beneath it, plus its
// own long var block), and Hands sits somewhere in there. 0x1000 found nothing;
// 0x4000 is a guess in the right direction, and cheap because it runs once.
static const size_t kPawnScan = 0x4000;

void* HandsProbe_Get() { return g_hands; }
void* HandsProbe_GetPawn() { return g_pawn; }

// Declared here, not further down: PollGripKeys uses both, and in one
// translation unit a static has to be declared before the function that reads it.
static int   g_editMode = 0;      // 0 position, 1 model rotation, 2 cursor
static float g_cursorBySlot[9][3] = {};
static int  g_wepSlot = -1;       // active weapon slot, -1 until the first switch
int HandsProbe_WeaponSlot() { return g_wepSlot; }

// ---------------------------------------------------------------- live tuning
//
// S56: HandsGripOffset is three numbers whose only test is "does the gun pivot
// about the grip when I twist my wrist" -- a visual judgement that cannot be
// made from a log and takes one rebuild per guess. So tune it in the headset.
//
// These keys belonged to XRSession's PollFovKeys (foreground FOV tuning), which
// ForegroundFovValue superseded. REMOVE THE CALL TO PollFovKeys or the two
// handlers will fight over the same presses.
//
//   Numpad 8 / 2   forward / back   (X)
//   Numpad 6 / 4   right / left     (Y)
//   Numpad 0 / 5   up / down        (Z)
//   Numpad 7       cycle step: 0.5 -> 2 -> 5 cm
//
// Every change logs the full line, ready to paste into BioshockVR.ini.
static void PollGripKeys()
{
    struct Bind { int vk; int axis; float sign; };
    static const Bind kBinds[6] = {
        { VK_NUMPAD8, 0, +1.0f }, { VK_NUMPAD2, 0, -1.0f },
        { VK_NUMPAD6, 1, +1.0f }, { VK_NUMPAD4, 1, -1.0f },
        { VK_NUMPAD0, 2, +1.0f }, { VK_NUMPAD5, 2, -1.0f },
    };

    static bool  prev[6] = {};
    static bool  prevStep = false;
    static bool  prevMode = false;
    static float step = 2.0f;
    static float rotStep = 1.0f;

    // NUMPAD 9 cycles what the six keys edit. PGUP still works as a second
    // trigger below.
    //
    // CORRECTED 2026-08-09: this comment used to claim VK_PRIOR "has never once
    // registered in a log on this keyboard". That is false -- the tester
    // confirmed directly that PGUP and PGDN both work and are used routinely.
    // Whatever the original symptom was, it was not a dead key, and acting on
    // the claim cost a detour in M1-S2. VK_PRIOR being bound in three places at
    // once is real, though: one press fires all three.
    //   0 POSITION  8/2 fwd   6/4 right  0/5 up     (cm)   -- the hands actor
    //   1 ROTATION  8/2 pitch 6/4 yaw    0/5 roll   (deg)  -- the hands MODEL
    //   2 CURSOR    8/2 pitch 6/4 yaw    0/5 roll   (deg)  -- the aim ray
    const bool modeDown = ((GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0);
    if (modeDown && !prevMode)
    {
        g_editMode = (g_editMode + 1) % 3;
        Log(">>> GRIP: numpad now edits %s",
            (g_editMode == 0) ? "POSITION (8/2 fwd, 6/4 right, 0/5 up, cm)"
            : (g_editMode == 1) ? "ROTATION (8/2 pitch, 6/4 yaw, 0/5 roll, deg)"
            : "CURSOR (8/2 pitch, 6/4 yaw, 0/5 roll, deg)");
    }
    prevMode = modeDown;

    const bool stepDown = (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0;
    if (stepDown && !prevStep)
    {
        if (g_editMode)
        {
            rotStep = (rotStep >= 15.0f) ? 0.1f
                : (rotStep >= 5.0f) ? 15.0f
                : (rotStep >= 2.0f) ? 5.0f
                : (rotStep >= 1.0f) ? 2.0f
                : (rotStep >= 0.5f) ? 1.0f
                : (rotStep >= 0.25f) ? 0.5f
                : (rotStep >= 0.1f) ? 0.25f : 0.1f;
            Log(">>> GRIP: rotation step = %.2f deg", rotStep);
        }
        else
        {
            step = (step > 4.0f) ? 0.5f : (step > 1.0f ? 5.0f : 2.0f);
            Log(">>> GRIP: position step = %.1f cm", step);
        }
    }
    prevStep = stepDown;

    float* const tgt = (g_editMode == 0) ? g_cfg.handsGrip
        : (g_editMode == 1) ? g_cfg.handsRot : g_cfg.cursorRot;
    const float amt = g_editMode ? rotStep : step;

    bool changed = false;
    for (int i = 0; i < 6; ++i)
    {
        const bool down = (GetAsyncKeyState(kBinds[i].vk) & 0x8000) != 0;
        if (down && !prev[i])
        {
            tgt[kBinds[i].axis] += kBinds[i].sign * amt;
            changed = true;
        }
        prev[i] = down;
    }

    if (changed)
    {
        // Slot -1 means no weapon switch has happened yet, so there is no key
        // to write to. Log it, don't invent a GripOffset-1.
        const char* what = (g_editMode == 0) ? "GripOffset"
            : (g_editMode == 1) ? "RotOffset" : "CursorOffset";
        const float* val = (g_editMode == 0) ? g_cfg.handsGrip
            : (g_editMode == 1) ? g_cfg.handsRot : g_cfg.cursorRot;
        const char* units = (g_editMode == 0) ? "(fwd, right, up cm)"
            : "(pitch, yaw, roll deg)";

        if (g_wepSlot >= 0 && g_wepSlot < 9)
        {
            char key[32];
            _snprintf_s(key, sizeof(key), _TRUNCATE, "%s%d", what, g_wepSlot);
            Cfg_WriteVec3(key, val);
            Log(">>> GRIP: %s=%.2f,%.2f,%.2f   %s   [SAVED]",
                key, val[0], val[1], val[2], units);
        }
        else
        {
            Log(">>> GRIP: %s=%.2f,%.2f,%.2f   %s   (no weapon slot yet, NOT saved)",
                what, val[0], val[1], val[2], units);
        }
    }
}

bool HandsProbe_GetTargets(void** obj, unsigned* locOff, unsigned* rotOff)
{
    if (!g_hands) return false;
    const size_t po = (g_cfg.handsPosOff > 0) ? (size_t)g_cfg.handsPosOff : g_locOff;
    if (obj)    *obj = g_hands;
    if (locOff) *locOff = (unsigned)po;
    if (rotOff) *rotOff = (unsigned)kActorRotation;
    return true;
}

// ---------------------------------------------------------------- stage A

// Is `pawn` the one we can actually drive? The real player pawn is the object
// whose Hands child (pawn + HandsPtrOffset) sits ON the camera every frame
// (Hands.UpdateLocation pins it there). Other actors do not. Stable across
// levels; the consensus-offset vote in STAGE A is not.
static bool PawnHasHandsOnCamera(void* pawn, const float cam[3])
{
    if (!pawn || g_cfg.handsPtrOff <= 0) return false;
    if (!Readable((const uint8_t*)pawn + g_cfg.handsPtrOff, 4)) return false;
    void* h = *(void**)((const uint8_t*)pawn + g_cfg.handsPtrOff);
    if (!LooksLikeObject(h)) return false;
    const size_t po = (g_cfg.handsPosOff > 0) ? (size_t)g_cfg.handsPosOff : 0x1D8;
    if (!Readable((const uint8_t*)h + po, sizeof(AVec))) return false;
    const double d = Dist((const AVec*)((const uint8_t*)h + po), cam);
    return (d >= 0.0 && d < 60.0);
}

static void FindLocationAndPawn(const void* pc, const float cam[3])
{
    // S51: version stamp. Three separate sessions have now been spent judging
    // results produced by a stale HandsProbe.cpp -- the file links fine when it
    // is out of date, because it simply never references the newer globals. If
    // this line does not say S51, nothing below it is worth reading.
    Log(">>> HANDS: probe build S66-RELOCK  (built %s %s)", __DATE__, __TIME__);
    Log(">>> HANDS: STAGE A  searching for AActor::Location + Pawn...");
    Log(">>> HANDS:   camera = %.1f %.1f %.1f", cam[0], cam[1], cam[2]);

    // Does the controller itself carry a camera-like position?
    {
        const size_t blk = ReadableBlock(pc, kObjScan);
        size_t off; double d;
        if (blk && FindVecNear((const uint8_t*)pc, blk, cam, 800.0, &off, &d))
            Log(">>> HANDS:   controller SELF has a position at +0x%03X (%.0f cm)",
                (unsigned)off, d);
        else
            Log(">>> HANDS:   controller SELF has no camera-like position "
                "(normal -- a Controller need not track its pawn)");
    }

    // S45: TWO PASSES, and the second only trusts objects that AGREE.
    //
    // The first version took "closest object wins" and picked 0x42700000 --
    // which is not a pointer at all, it is the float bit pattern for 60.0f
    // (DefaultForegroundFOV, sitting in the controller). Reading +0x100 of that
    // address happened to look like a position 10cm from the camera, so it beat
    // the real pawn at 57cm and every later stage scanned garbage.
    //
    // The tell was in the log: the fake reported its position at +0x100 while
    // the three real hits all agreed on +0x1A0. Requiring agreement with the
    // consensus offset excludes it for nothing.
    struct Tally { size_t off; int n; };
    Tally tally[32] = {};
    int nTally = 0;

    struct Hit { void* obj; size_t off; double d; };
    Hit hitList[24] = {};
    int hits = 0;

    for (size_t po = 0; po + 4 <= kPtrScan; po += 4)
    {
        if (!Readable((const uint8_t*)pc + po, 4)) continue;
        void* t = *(void**)((const uint8_t*)pc + po);
        if (!t || t == pc) continue;
        if (((uintptr_t)t & 3) != 0) continue;
        if ((uintptr_t)t < 0x10000) continue;

        const size_t blk = ReadableBlock(t, kObjScan);
        if (!blk) continue;

        size_t lo; double d;
        if (!FindVecNear((const uint8_t*)t, blk, cam, 800.0, &lo, &d)) continue;

        Log(">>> HANDS:   ptr +0x%03X -> 0x%08X   position at +0x%03X, %.0f cm",
            (unsigned)po, (unsigned)(uintptr_t)t, (unsigned)lo, d);

        int found = -1;
        for (int i = 0; i < nTally; ++i) if (tally[i].off == lo) { found = i; break; }
        if (found < 0) { if (nTally < 32) { tally[nTally].off = lo; tally[nTally].n = 1; ++nTally; } }
        else ++tally[found].n;

        if (hits < 24) { hitList[hits].obj = t; hitList[hits].off = lo; hitList[hits].d = d; }
        ++hits;
        if (hits >= 24) { Log(">>> HANDS:   ...stopping at 24 hits"); break; }
    }

    if (!hits)
    {
        Log(">>> HANDS: !!! STAGE A found nothing within 8 m of the camera.");
        Log(">>> HANDS: !!! Either the Pawn is not a direct pointer in the first "
            "0x%X bytes, or positions are not stored as plain float triples.",
            (unsigned)kPtrScan);
        return;
    }

    // Most-agreed offset wins. Then, among objects that USE that offset, take
    // the closest -- never across offsets, which is what let a float in.
    int bestN = 0; size_t agreed = 0;
    for (int i = 0; i < nTally; ++i)
        if (tally[i].n > bestN) { bestN = tally[i].n; agreed = tally[i].off; }

    void* bestObj = nullptr;
    double bestDist = 1e18;
    for (int i = 0; i < hits && i < 24; ++i)
    {
        if (hitList[i].off != agreed) continue;          // disagrees -> not an actor
        if (hitList[i].d < bestDist) { bestDist = hitList[i].d; bestObj = hitList[i].obj; }
    }

    if (!bestObj)
    {
        Log(">>> HANDS: !!! no object agreed on the consensus offset. Aborting.");
        return;
    }

    // The consensus vote is not stable across levels: on some maps unrelated
    // actors outvote the real pawn (measured: +0x1D8 beat the real pawn's +0x1A0
    // 6-to-2, locking an actor 476 cm away). Prefer whichever candidate actually
    // has its Hands on the camera.
    if (g_cfg.handsPtrOff > 0 && !PawnHasHandsOnCamera(bestObj, cam))
    {
        for (int i = 0; i < hits && i < 24; ++i)
        {
            if (PawnHasHandsOnCamera(hitList[i].obj, cam))
            {
                Log(">>> HANDS: consensus pawn 0x%08X has no hands on camera; "
                    "using verified pawn 0x%08X (+0x%03X) instead.",
                    (unsigned)(uintptr_t)bestObj,
                    (unsigned)(uintptr_t)hitList[i].obj, (unsigned)hitList[i].off);
                bestObj = hitList[i].obj;
                agreed = hitList[i].off;
                break;
            }
        }
    }

    g_locOff = agreed;
    g_pawn = bestObj;

    Log(">>> HANDS: STAGE A: %d hit(s); offset +0x%03X agreed by %d.",
        hits, (unsigned)agreed, bestN);
    Log(">>> HANDS: AActor::Location = +0x%03X    Pawn = 0x%08X (%.0f cm)",
        (unsigned)g_locOff, (unsigned)(uintptr_t)g_pawn, bestDist);

    // ShockPlayer carries LastPlayerInputContext -- the controller does not.
    GameState_SetPawn(g_pawn);

    if (bestN < 2)
        Log(">>> HANDS: !!! only one object agreed -- treat the offset as "
            "provisional until STAGE B corroborates it.");
}

// ---------------------------------------------------------------- stage B

// Largest per-axis rotator disagreement in degrees, or -1 if unreadable.
static double RotErrDeg(const void* obj, size_t rotOff, const int camRot[3])
{
    if (!Readable((const uint8_t*)obj + rotOff, 12)) return -1.0;
    const int32_t* R = (const int32_t*)((const uint8_t*)obj + rotOff);

    double worst = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        int32_t d = (int32_t)(((uint32_t)R[i] - (uint32_t)camRot[i]) & 0xFFFF);
        if (d > 32768) d -= 65536;
        const double deg = fabs((double)d) / 182.0444;
        if (deg > worst) worst = deg;
    }
    return worst;
}

// S47: DIFFERENTIAL rotation search.
//
// MEASURED 19:02: single-sample matching reported 132 "matches" because the view
// rotator happened to be near zero (-295, -582, 0), so "within 5 degrees of the
// view" meant "within 5 degrees of zero" and every zeroed rotator passed.
//
// The property we actually want is not "equals the view right now" but "TRACKS
// the view". Nothing static can fake that. So: sample once, wait until the view
// has turned at least ~20 degrees, sample again, keep only what moved with us.
//
// Position matching stays in as a free passenger -- it costs one subtraction and
// three scans of the correct pawn have now reported zero objects within 60cm,
// which is itself the finding.

// S49: find the ROTATION OFFSET and the object TOGETHER.
//
// MEASURED 19:22: with the vtable filter fixed, 31 real objects were examined
// and NONE tracked the view at +0x1AC -- and none sat near the camera either.
// Position was already dead; +0x1AC was a guess, made the same way +0x1D8 was,
// and it is very likely wrong for the same reason.
//
// So stop guessing the offset. Snapshot every 4-byte slot of every reachable
// object, wait for a large turn, and look for ANY integer that changed by the
// SAME amount the view changed. That finds the object AND the offset at once --
// which is exactly how Location was found, and the only method here that has
// actually worked.
static const int    kMaxObj = 96;
static const size_t kObjSnap = 0x400;          // 256 dwords per object
static const int    kSnapDw = (int)(kObjSnap / 4);

static void* g_objs[kMaxObj] = {};
static int32_t g_snap[kMaxObj][kSnapDw] = {};
static size_t  g_objPtrOff[kMaxObj] = {};
static int     g_nObjs = 0;
static int32_t g_sampleYaw = 0;
static bool    g_haveSample = false;

// Returns TRUE if a real sample was taken (so the caller can spend an attempt).
static bool FindHands(const void* pawn, const float cam[3], const int camRot[3])
{
    if (!g_haveSample)
    {
        Log(">>> HANDS: STAGE B pass 1  snapshotting Pawn 0x%08X to +0x%X ...",
            (unsigned)(uintptr_t)pawn, (unsigned)kPawnScan);

        g_nObjs = 0;
        int nearCam = 0;

        for (size_t po = 0; po + 4 <= kPawnScan && g_nObjs < kMaxObj; po += 4)
        {
            if (!Readable((const uint8_t*)pawn + po, 4)) continue;
            void* t = *(void**)((const uint8_t*)pawn + po);
            if (t == pawn || !LooksLikeObject(t)) continue;

            bool dup = false;
            for (int i = 0; i < g_nObjs; ++i) if (g_objs[i] == t) { dup = true; break; }
            if (dup) continue;

            const size_t blk = ReadableBlock(t, kObjSnap);
            if (blk < 0x40) continue;

            const double d = Dist((const AVec*)((const uint8_t*)t + g_locOff), cam);
            if (d >= 0.0 && d <= 60.0)
            {
                ++nearCam;
                Log(">>> HANDS:   +0x%04X -> 0x%08X  %.1f cm   <-- NEAR CAMERA",
                    (unsigned)po, (unsigned)(uintptr_t)t, d);
            }

            g_objs[g_nObjs] = t;
            g_objPtrOff[g_nObjs] = po;
            const int dw = (int)(blk / 4);
            for (int j = 0; j < kSnapDw; ++j)
                g_snap[g_nObjs][j] = (j < dw) ? ((const int32_t*)t)[j] : 0;
            ++g_nObjs;
        }

        g_sampleYaw = camRot[1];
        g_haveSample = true;

        Log(">>> HANDS: pass 1: %d object(s) snapshotted (%d dwords each), "
            "%d near the camera. View yaw %d.",
            g_nObjs, kSnapDw, nearCam, (int)g_sampleYaw);
        Log(">>> HANDS: >>> NOW TURN AT LEAST 45 DEGREES. <<<");
        return true;
    }

    int32_t dView = (int32_t)(((uint32_t)camRot[1] - (uint32_t)g_sampleYaw) & 0xFFFF);
    if (dView > 32768) dView -= 65536;
    const double dViewDeg = fabs((double)dView) / 182.0444;

    if (dViewDeg < 25.0)
    {
        Log(">>> HANDS: pass 2 waiting -- view turned %.1f deg, need 25+.", dViewDeg);
        return false;
    }

    Log(">>> HANDS: STAGE B pass 2  view turned %.1f deg. Scanning %d object(s) "
        "x %d dwords for anything that turned with us...",
        dViewDeg, g_nObjs, kSnapDw);

    int hits = 0;
    for (int i = 0; i < g_nObjs; ++i)
    {
        void* t = g_objs[i];
        const size_t blk = ReadableBlock(t, kObjSnap);
        if (!blk) continue;
        const int dw = (int)(blk / 4);

        for (int j = 0; j < dw && j < kSnapDw; ++j)
        {
            const int32_t now = ((const int32_t*)t)[j];
            const int32_t was = g_snap[i][j];
            if (now == was) continue;

            int32_t d = (int32_t)(((uint32_t)now - (uint32_t)was) & 0xFFFF);
            if (d > 32768) d -= 65536;

            const double err = fabs((double)(d - dView)) / 182.0444;
            if (err > 3.0) continue;

            ++hits;
            if (hits <= 40)
                Log(">>> HANDS:   obj 0x%08X (pawn+0x%04X)  +0x%03X  turned %.1f deg "
                    "(view %.1f, err %.1f)",
                    (unsigned)(uintptr_t)t, (unsigned)g_objPtrOff[i],
                    (unsigned)(j * 4), (double)d / 182.0444, dViewDeg, err);
        }
    }

    Log(">>> HANDS: pass 2: %d field(s) turned with the view.", hits);
    if (!hits)
        Log(">>> HANDS: !!! NOTHING in %d objects tracks the view. The hands "
            "transform is not stored as a rotator on anything reachable from "
            "the Pawn -- it is composed natively at render time.", g_nObjs);

    g_haveSample = false;
    return true;
}

// ---------------------------------------------------------------- stage C
//
//   arms move while held, snap back      UpdateLocation recomputes each frame
//                                        and our write wins -> 6-DOF is on
//   arms move and stay                   nothing recomputes them -> even easier
//   arms do not move                     wrong object, or the transform is
//                                        native downstream of Location -> dead
//
// HOME deliberately. A full audit of the codebase found EVERY numpad key
// already bound: XRSession owns 0/2/4/5/6/7/8 (FOV tuning), DrawHook owns
// 1/3/-/*//, CameraHook owns 9/+/. -- so the earlier nudge attempts on 6 and 0
// were silently driving fovScaleH and the FOV-mode toggle instead.

// S52: ABSOLUTE rotation write, not incremental.
//
// MEASURED: pawn+0x724 IS the hands -- writing its rotator visibly moves the
// arm. But the first version did `R[1] += nudge` on every CalcView, ~120-240
// times a second, so the yaw was being SPUN and wrapping through 65536 units
// rather than offset by the requested angle. That is why 60 "worked", 45 did
// nothing, and 60 again put the arm somewhere impossible: the result depended
// on where in the spin each frame happened to land.
//
// Absolute instead. The rotator tracks the view (measured, err 0.0 deg over
// three turns), so we sample the hands-to-view BIAS once, before writing
// anything, and then each frame drive it to view + bias + nudge. Idempotent, and
// it is the same shape the real 6-DOF write needs: a target orientation.
// S53: find the hands POSITION field.
//
// Every earlier scan only ever tested +0x1A0 on this object -- the offset that
// was measured on the PAWN. Rotation turned out to live at +0x1E4 on both, but
// there is no guarantee position does, and "no object near the camera" may have
// meant "not at +0x1A0" rather than "not stored anywhere".
//
// So look at every offset in the object we now know is the hands, and report
// anything that reads as a position near the camera. Runs once.
// S61: SWEEP the DrawScale candidates instead of guessing one per rebuild.
//
// +0x2AC was inferred from four consecutive 1.0 floats (the standard
// `float DrawScale; vector DrawScale3D;` pair) and writing 0.70 there changed
// nothing -- so the inference was wrong, and eleven candidates is eleven
// launches at one guess per build.
//
// Instead: write HandsScale into each candidate in turn, three seconds each,
// logging which one is live. One run, and you just note when the gun changes
// size. Each candidate is restored to 1.0 before moving on, so at most one is
// modified at a time and nothing is left altered.
// S64: THE OFFSET IS NO LONGER THE UNKNOWN. THE OBJECT IS.
//
// S63 swept 16 offsets on the Hands actor and four of them were live: +0x2AC
// scaled the whole arm, +0x2B0/B4/B8 scaled it per-axis. So DrawScale is found,
// and `HandsScale` is now a real setting rather than a probe -- it is written
// persistently below.
//
// But the GUN did not change size in that sweep, at any offset. The gun is a
// separate Holdable actor attached at bone R_Grip; it carries its OWN DrawScale,
// at the same +0x2AC, on an object we do not have a pointer to yet.
//
// So the sweep flips around. One offset, known and known-safe. Many candidate
// objects, walked three seconds each. Candidates are every distinct pointer
// hanging off the Pawn or off Hands that:
//
//   - passes LooksLikeObject (object -> vtable -> executable first method)
//   - is big enough to contain +0x2AC
//   - currently reads EXACTLY 1.0 at +0x2AC   (a real actor at default scale;
//     also a cheap sanity check that the field is what we think it is)
//
// and they are ordered by how close their Location sits to the camera, because
// a held weapon is attached to your hand and everything else in the pawn's
// pointer soup is a manager, an inventory entry, or a component.
//
// Restoring to exactly 1.0 afterwards is lossless, and a single float store into
// a field we have already proven is a scale cannot do what the S62 16-byte
// writes did.
static const int kMaxGunCand = 24;

struct GunCand
{
    void* obj;
    unsigned base;      // 0 == pawn, 1 == hands
    unsigned off;
    double   dist;      // cm from the camera at collection time
};

static GunCand g_gunCand[kMaxGunCand] = {};
static int     g_nGunCand = 0;
static bool    g_gunCollected = false;
static void* g_gun = nullptr;     // pinned by GunPtrOffset, once known

// S65: children of the weapon actor. Same shape, one level down.
static GunCand g_kidCand[kMaxGunCand] = {};
static int     g_nKidCand = 0;
static bool    g_kidCollected = false;

static const char* BaseName(unsigned b)
{
    return (b == 0) ? "pawn" : (b == 1) ? "hands" : "gun";
}

// Keep HandsScale applied. Idempotent, and cheap enough to do every call: the
// game re-authors this actor constantly and a one-shot write does not survive.
static void ApplyHandsScale(void* hands)
{
    if (g_cfg.handsScale <= 0.0f || !hands) return;
    float* p = (float*)((uint8_t*)hands + kActorDrawScale);
    if (!Writable(p, 4)) return;
    if (*p != g_cfg.handsScale) *p = g_cfg.handsScale;
}

// One qualifying test, used for both the weapon hunt and the child hunt.
// maxDist caps how far from the camera a candidate may sit: unlimited for the
// weapon hunt (we sort instead), tight for children, because anything actually
// attached to a gun in your hand is by definition next to your hand.
static void AddCand(GunCand* list, int* n, void* obj, unsigned base,
    unsigned off, const float cam[3], double maxDist)
{
    if (*n >= kMaxGunCand) return;
    if (!LooksLikeObject(obj)) return;
    if (obj == g_pawn || obj == g_hands || obj == g_gun) return;

    for (int i = 0; i < *n; ++i)
        if (list[i].obj == obj) return;                    // dedupe

    if (ReadableBlock(obj, kActorDrawScale + 4) < kActorDrawScale + 4) return;

    const float sc = *(const float*)((const uint8_t*)obj + kActorDrawScale);
    if (sc != 1.0f) return;                                // not an actor at rest

    double d = 1e9;
    if (Readable((const uint8_t*)obj + kActorLocation, sizeof(AVec)))
    {
        const double t = Dist((const AVec*)((const uint8_t*)obj + kActorLocation), cam);
        if (t >= 0.0) d = t;
    }
    if (d > maxDist) return;

    list[*n].obj = obj;
    list[*n].base = base;
    list[*n].off = off;
    list[*n].dist = d;
    ++(*n);
}

static void SortByDistance(GunCand* list, int n)
{
    for (int i = 0; i < n; ++i)
    {
        int best = i;
        for (int j = i + 1; j < n; ++j)
            if (list[j].dist < list[best].dist) best = j;
        if (best != i) { GunCand t = list[i]; list[i] = list[best]; list[best] = t; }
    }
}

static void LogCandList(const char* tag, const GunCand* list, int n)
{
    Log(">>> %s: %d candidate actor(s) at DrawScale 1.0, nearest first:", tag, n);
    for (int i = 0; i < n; ++i)
    {
        char d[32];
        if (list[i].dist > 1e8) _snprintf_s(d, sizeof(d), _TRUNCATE, "no position");
        else                    _snprintf_s(d, sizeof(d), _TRUNCATE, "%.0f cm", list[i].dist);
        Log(">>> %s:   [%2d] 0x%08X  %s+0x%03X   %s",
            tag, i + 1, (unsigned)(uintptr_t)list[i].obj,
            BaseName(list[i].base), list[i].off, d);
    }
}

static void CollectGunCandidates(const float cam[3])
{
    if (g_gunCollected) return;
    g_gunCollected = true;
    g_nGunCand = 0;

    // Hands FIRST. A Holdable is attached to the hand, and Hands is where the
    // script keeps the active one, so the answer is more likely here than in the
    // pawn's much larger pointer soup.
    struct { void* obj; unsigned base; size_t span; } roots[2] = {
        { g_hands, 1, 0x800 },
        { g_pawn,  0, 0xC00 },
    };

    for (int r = 0; r < 2; ++r)
    {
        if (!roots[r].obj) continue;
        const size_t blk = ReadableBlock(roots[r].obj, roots[r].span);
        if (!blk) continue;

        for (size_t off = 0; off + 4 <= blk; off += 4)
        {
            void* t = *(void**)((uint8_t*)roots[r].obj + off);
            AddCand(g_gunCand, &g_nGunCand, t, roots[r].base, (unsigned)off, cam, 1e18);
            if (g_nGunCand >= kMaxGunCand) break;
        }
    }

    SortByDistance(g_gunCand, g_nGunCand);
    LogCandList("GUN", g_gunCand, g_nGunCand);

    if (!g_nGunCand)
        Log(">>> GUN: !!! nothing qualified. The weapon actor is not a direct "
            "pointer on the pawn or on Hands -- it is reached through an "
            "inventory list, and we need to walk that instead.");
}

// S65: everything the WEAPON points at that is itself an actor sitting next to
// you. The cylinder, and whatever else the gun is built from.
static void CollectGunChildren(const float cam[3])
{
    if (g_kidCollected || !g_gun) return;
    g_kidCollected = true;
    g_nKidCand = 0;

    const size_t blk = ReadableBlock(g_gun, 0x800);
    if (!blk)
    {
        Log(">>> KID: weapon object unreadable.");
        return;
    }

    for (size_t off = 0; off + 4 <= blk; off += 4)
    {
        void* t = *(void**)((uint8_t*)g_gun + off);
        AddCand(g_kidCand, &g_nKidCand, t, 2, (unsigned)off, cam, 300.0);
        if (g_nKidCand >= kMaxGunCand) break;
    }

    SortByDistance(g_kidCand, g_nKidCand);
    LogCandList("KID", g_kidCand, g_nKidCand);

    if (!g_nKidCand)
        Log(">>> KID: !!! the weapon points at no other actor within 3 m. The "
            "cylinder is not a child actor -- it is a second mesh on the same "
            "actor, and needs a different handle entirely.");
}

// GunChildren=2. Scale everything at once -- this is the shape the finished
// feature wants, since the goal is "the whole gun is smaller", not "identify one
// object". The list was logged at collection time, so if something you did not
// expect changes size you can see exactly what did it.
static void ApplyGunChildren()
{
    for (int i = 0; i < g_nKidCand; ++i)
    {
        float* p = (float*)((uint8_t*)g_kidCand[i].obj + kActorDrawScale);
        if (Writable(p, 4) && *p != g_cfg.gunScale) *p = g_cfg.gunScale;
    }
}

// GunChildren=1. One at a time, three seconds each, in case mode 2 scales
// something it should not and we need to know which entry did it.
static void SweepGunChildren()
{
    if (!g_nKidCand) return;

    static int   idx = -1;
    static DWORD nextAt = 0;

    const DWORD now = GetTickCount();
    if (now < nextAt) return;
    nextAt = now + 3000;

    if (idx >= 0 && idx < g_nKidCand)
    {
        float* prev = (float*)((uint8_t*)g_kidCand[idx].obj + kActorDrawScale);
        if (Writable(prev, 4)) *prev = 1.0f;
    }

    if (++idx >= g_nKidCand)
    {
        idx = -1;
        Log(">>> KID SWEEP: pass complete, everything restored to 1.0. Looping.");
        return;
    }

    float* p = (float*)((uint8_t*)g_kidCand[idx].obj + kActorDrawScale);
    if (!Writable(p, 4)) return;
    *p = g_cfg.gunScale;

    Log(">>> KID SWEEP: [%d/%d] 0x%08X (gun+0x%03X) DrawScale = %.2f   <-- watch "
        "the CYLINDER now",
        idx + 1, g_nKidCand, (unsigned)(uintptr_t)g_kidCand[idx].obj,
        g_kidCand[idx].off, g_cfg.gunScale);
}

// Walk the candidate objects, three seconds each, writing GunScale into
// +0x2AC and putting the previous one back. You are watching for the GUN to
// change size -- the arm will already be scaled by HandsScale and will not move.
static void SweepGunScale(const float cam[3])
{
    if (g_cfg.gunScale <= 0.0f) return;

    // Pinned: we already know which object it is. Keep the value applied, and
    // deal with whatever the weapon itself is built from.
    if (g_gun)
    {
        float* p = (float*)((uint8_t*)g_gun + kActorDrawScale);
        if (Writable(p, 4) && *p != g_cfg.gunScale) *p = g_cfg.gunScale;

        if (g_cfg.gunChildren)
        {
            CollectGunChildren(cam);
            if (g_cfg.gunChildren == 1) SweepGunChildren();
            else                       ApplyGunChildren();
        }
        return;
    }

    CollectGunCandidates(cam);
    if (!g_nGunCand) return;

    static int   idx = -1;
    static DWORD nextAt = 0;

    const DWORD now = GetTickCount();
    if (now < nextAt) return;
    nextAt = now + 3000;

    if (idx >= 0 && idx < g_nGunCand)
    {
        float* prev = (float*)((uint8_t*)g_gunCand[idx].obj + kActorDrawScale);
        if (Writable(prev, 4)) *prev = 1.0f;
    }

    if (++idx >= g_nGunCand)
    {
        idx = -1;
        Log(">>> GUN SWEEP: pass complete, everything restored to 1.0. Looping. "
            "If the gun never changed size it is not a direct pointer on the "
            "pawn or Hands.");
        return;
    }

    float* p = (float*)((uint8_t*)g_gunCand[idx].obj + kActorDrawScale);
    if (!Writable(p, 4)) return;
    *p = g_cfg.gunScale;

    Log(">>> GUN SWEEP: [%d/%d] 0x%08X (%s+0x%03X) DrawScale = %.2f   <-- watch "
        "the GUN now. If it shrank: GunPtrBase=%d GunPtrOffset=0x%03X",
        idx + 1, g_nGunCand, (unsigned)(uintptr_t)g_gunCand[idx].obj,
        g_gunCand[idx].base ? "hands" : "pawn", g_gunCand[idx].off,
        g_cfg.gunScale, g_gunCand[idx].base, g_gunCand[idx].off);
}

static void ScanHandsForPosition(const void* hands, const float cam[3])
{
    static bool done = false;
    if (done) return;
    done = true;

    const size_t blk = ReadableBlock(hands, 0x800);   // S62: 0x400 may have been short
    if (!blk) { Log(">>> HANDS: position scan: object unreadable."); return; }

    Log(">>> HANDS: position scan of 0x%08X (camera %.1f %.1f %.1f)...",
        (unsigned)(uintptr_t)hands, cam[0], cam[1], cam[2]);

    int hits = 0;
    double best = 1e18; size_t bestOff = 0;

    for (size_t off = 0; off + sizeof(AVec) <= blk; off += 4)
    {
        const double d = Dist((const AVec*)((const uint8_t*)hands + off), cam);
        if (d < 0.0 || d > 800.0) continue;

        ++hits;
        if (d < best) { best = d; bestOff = off; }

        const AVec* v = (const AVec*)((const uint8_t*)hands + off);
        Log(">>> HANDS:   +0x%03X = %.1f %.1f %.1f   (%.1f cm from camera)%s",
            (unsigned)off, v->x, v->y, v->z, d,
            (d <= 60.0) ? "   <-- HANDS-LIKE" : "");

        if (hits >= 20) { Log(">>> HANDS:   ...stopping at 20"); break; }
    }

    // S64: the 1.0-float listing is retired. DrawScale is MEASURED at +0x2AC
    // and DrawScale3D at +0x2B0 (see kActorDrawScale). Just report what the
    // block currently holds, so a wrong object announces itself immediately.
    if (blk >= kActorDrawScale + 16)
    {
        const float* s = (const float*)((const uint8_t*)hands + kActorDrawScale);
        Log(">>> HANDS: DrawScale +0x2AC = %.3f   DrawScale3D = %.3f %.3f %.3f",
            s[0], s[1], s[2], s[3]);
    }
    else
    {
        Log(">>> HANDS: !!! object too small to hold DrawScale at +0x2AC (%u "
            "bytes readable). Wrong object?", (unsigned)blk);
    }

    if (hits)
        Log(">>> HANDS: position scan: %d candidate(s), closest +0x%03X at %.1f cm. "
            "Set HandsPosOffset=0x%X and HandsNudgeZ=100 to test it.",
            hits, (unsigned)bestOff, best, (unsigned)bestOff);
    else
        Log(">>> HANDS: !!! position scan found NOTHING within 8 m anywhere in "
            "the hands object. The world position is not stored here -- it is "
            "composed at render time from the rotator plus PlayerViewOffset.");
}

static void NudgeTest(void* hands, const float cam[3], const int camRot[3])
{
    ScanHandsForPosition(hands, cam);

    if (!hands) return;
    if (g_cfg.handsNudgeYaw == 0.0f && g_cfg.handsNudgePitch == 0.0f &&
        g_cfg.handsNudgeZ == 0.0f) return;

    if ((g_cfg.handsNudgeYaw != 0.0f || g_cfg.handsNudgePitch != 0.0f) &&
        Writable((uint8_t*)hands + kActorRotation, 12))
    {
        int32_t* R = (int32_t*)((uint8_t*)hands + kActorRotation);

        // Sample the offset between the hands rotator and the view rotator ONCE,
        // before we have written anything. It may not be zero, and assuming it
        // is would bake a constant error into every write from here on.
        static bool  haveBias = false;
        static int32_t biasP = 0, biasY = 0;
        if (!haveBias)
        {
            haveBias = true;
            biasP = (int32_t)(((uint32_t)R[0] - (uint32_t)camRot[0]) & 0xFFFF);
            biasY = (int32_t)(((uint32_t)R[1] - (uint32_t)camRot[1]) & 0xFFFF);
            if (biasP > 32768) biasP -= 65536;
            if (biasY > 32768) biasY -= 65536;

            Log(">>> HANDS: NUDGE ABSOLUTE on 0x%08X (rotator +0x%03X)",
                (unsigned)(uintptr_t)hands, (unsigned)kActorRotation);
            Log(">>> HANDS:   hands p=%d y=%d r=%d   view p=%d y=%d r=%d",
                R[0], R[1], R[2], camRot[0], camRot[1], camRot[2]);
            Log(">>> HANDS:   bias  pitch %.1f deg  yaw %.1f deg   -> applying "
                "pitch %+.0f  yaw %+.0f",
                (double)biasP / 182.0444, (double)biasY / 182.0444,
                g_cfg.handsNudgePitch, g_cfg.handsNudgeYaw);
        }

        if (g_cfg.handsNudgeYaw != 0.0f)
            R[1] = camRot[1] + biasY + (int32_t)(g_cfg.handsNudgeYaw * 182.0444f);
        if (g_cfg.handsNudgePitch != 0.0f)
            R[0] = camRot[0] + biasP + (int32_t)(g_cfg.handsNudgePitch * 182.0444f);
    }

    if (g_cfg.handsNudgeZ != 0.0f)
    {
        // Use the offset the position scan found, if one was given; otherwise
        // fall back to the pawn's Location offset, which is what every earlier
        // (fruitless) attempt used.
        const size_t po = (g_cfg.handsPosOff > 0) ? (size_t)g_cfg.handsPosOff : g_locOff;
        if (Writable((uint8_t*)hands + po, sizeof(AVec)))
        {
            static bool announced = false;
            if (!announced)
            {
                announced = true;
                const AVec* v = (const AVec*)((const uint8_t*)hands + po);
                Log(">>> HANDS: NUDGE Z %+.0f cm at +0x%03X (currently %.1f %.1f %.1f)",
                    g_cfg.handsNudgeZ, (unsigned)po, v->x, v->y, v->z);
            }
            ((AVec*)((uint8_t*)hands + po))->z += g_cfg.handsNudgeZ;
        }
    }
}

// ---- HAND MODE PROBE: plasmid vs weapon ---------------------------------
// Hands declares CurrentAbility (Ability*), OldAbility, CurrentHoldable, and a
// travel bool bIsInWeaponMode. SetHandsMode('Weapon'/'Ability') is the only
// writer of that bool, so switching wrench <-> plasmid MUST move one of:
//   * a pointer slot going null <-> object   -> CurrentAbility
//   * a dword changing by exactly one bit    -> bIsInWeaponMode's bitfield
// One shot per keypress. Never a per-frame scan.
//
//   HOME  snapshot every dword of the Hands object
//   END   dump the dwords that changed since the snapshot
static const size_t kHmScan = 0x800;
static uint32_t g_hmSnap[kHmScan / 4] = {};
static bool     g_hmValid = false;

static void HandsModeSnapshot(const void* hands)
{
    const size_t blk = ReadableBlock(hands, kHmScan);
    if (!blk) { Log(">>> HANDMODE: hands object unreadable."); return; }

    for (size_t off = 0; off + 4 <= blk; off += 4)
        g_hmSnap[off / 4] = *(const uint32_t*)((const uint8_t*)hands + off);

    g_hmValid = true;
    Log(">>> HANDMODE: snapshot of 0x%08X over 0x%X bytes. PGUP/PGDN. Now SWITCH between the "
        "wrench and a plasmid, then press END.",
        (unsigned)(uintptr_t)hands, (unsigned)blk);
}

static void HandsModeDiff(const void* hands)
{
    if (!g_hmValid) { Log(">>> HANDMODE: no snapshot yet. Press PGUP first."); return; }

    const size_t blk = ReadableBlock(hands, kHmScan);
    if (!blk) { Log(">>> HANDMODE: hands object unreadable."); return; }

    Log(">>> HANDMODE: ---- dwords that changed ----");
    int shown = 0;
    for (size_t off = 0; off + 4 <= blk; off += 4)
    {
        const uint32_t now = *(const uint32_t*)((const uint8_t*)hands + off);
        const uint32_t was = g_hmSnap[off / 4];
        if (now == was) continue;

        // Classify, so the two we want stand out from animation handles and
        // float timers rather than having to be guessed at from a wall of hex.
        const bool ptrNow = LooksLikeObject((const void*)now);
        const bool ptrWas = LooksLikeObject((const void*)was);
        const uint32_t x = now ^ was;
        const bool oneBit = (x != 0) && ((x & (x - 1)) == 0);

        Log(">>> HANDMODE:   +0x%03X  0x%08X -> 0x%08X%s%s",
            (unsigned)off, was, now,
            (ptrNow != ptrWas) ? "   <-- OBJECT PTR (CurrentAbility?)" : "",
            oneBit ? "   <-- ONE BIT (bIsInWeaponMode?)" : "");

        if (++shown >= 40) { Log(">>> HANDMODE:   ...truncated at 40"); break; }
    }
    if (!shown) Log(">>> HANDMODE:   (nothing changed -- did the mode actually switch?)");
    Log(">>> HANDMODE: ------------------------------");

    g_hmValid = false;   // one shot; press HOME again for the next comparison
}

static void PollHandsModeKeys(const void* hands)
{
    static bool kH = false, kE = false;

    const bool dH = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;   // PGUP
    if (dH && !kH) HandsModeSnapshot(hands);
    kH = dH;

    const bool dE = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;    // PGDN
    if (dE && !kE) HandsModeDiff(hands);
    kE = dE;
}

// ---- ACTIVE HAND MODE: weapon vs plasmid --------------------------------
// MEASURED with the PGUP/PGDN dword differ, both directions:
//   hands+0x454   null with the wrench   -> object with a plasmid   CurrentAbility
//   hands+0x45C   object with the wrench -> null with a plasmid     CurrentHoldable
// That pair also CONFIRMS the layout: Hands declares CurrentAbility, OldAbility,
// CurrentHoldable consecutively, so +0x454/+0x458/+0x45C is that run of three
// pointers -- which means +0x45C is CurrentHoldable, not "the weapon actor" as
// GunPtrOffset's name implies. Deriving ability = GunPtrOffset - 8 keeps one ini
// key driving both so they cannot drift.
//
// Requiring BOTH pointers avoids flapping: mid-equip one is briefly null before
// the other has been written.
static bool g_abilityMode = false;

static void UpdateHandMode(const void* hands)
{
    if (!hands || g_cfg.gunPtrOff <= 0 || !g_cfg.gunPtrBase) return;   // Hands-relative only

    const unsigned holdOff = (unsigned)g_cfg.gunPtrOff;
    const unsigned abilOff = holdOff - 8;

    if (!Readable((const uint8_t*)hands + abilOff, 4)) return;
    if (!Readable((const uint8_t*)hands + holdOff, 4)) return;

    const void* abil = *(void* const*)((const uint8_t*)hands + abilOff);
    const void* hold = *(void* const*)((const uint8_t*)hands + holdOff);

    const bool ability = (abil != nullptr) && (hold == nullptr);
    if (ability == g_abilityMode) return;

    g_abilityMode = ability;
    Log(">>> HANDMODE: %s   (ability=0x%08X holdable=0x%08X)",
        ability ? "PLASMID -- left hand" : "WEAPON -- right hand",
        (unsigned)(uintptr_t)abil, (unsigned)(uintptr_t)hold);
}

bool HandsProbe_AbilityMode() { return g_abilityMode; }

// ---- WHICH WEAPON IS EQUIPPED --------------------------------------------
// ShockPawn: var config array< Class<Weapon> > AllPossibleWeaponClasses;
// The UClass pointers are heap addresses and change every launch, but their
// ORDER in that array is fixed by config -- so the INDEX is a stable ini key.
//
// STAGE W1  find the array. A UE2 TArray is { void* Data; int Count; int Max; }.
//           Accept Count 4..16, Max >= Count, every element a DISTINCT readable
//           object pointer. PreloadClasses matches this shape too, so we keep
//           EVERY candidate and let the log tell us which is which.
//
// STAGE W2  find UObject::Class without knowing the UObject layout: scan the
//           live Holdable for a dword equal to one of those class pointers. The
//           match is simultaneously the offset and the proof it is the field.
static const int kWepMax = 16;
static const int kCandMax = 4;
static void* g_wepList[kCandMax][kWepMax] = {};
static int      g_wepCount[kCandMax] = {};
static unsigned g_wepAt[kCandMax] = {};
static int      g_nWepCand = 0;
static bool     g_wepScanned = false;

static void ScanWeaponClassLists(const void* pawn)
{
    if (g_wepScanned || !pawn) return;
    g_wepScanned = true;

    const size_t blk = ReadableBlock(pawn, 0x1000);
    for (size_t off = 0; off + 12 <= blk && g_nWepCand < kCandMax; off += 4)
    {
        const uint8_t* h = (const uint8_t*)pawn + off;
        void* const data = *(void* const*)h;
        const int count = *(const int*)(h + 4);
        const int maxN = *(const int*)(h + 8);

        if (count < 4 || count > kWepMax || maxN < count) continue;
        if (!data || !Readable(data, (size_t)count * 4)) continue;

        void* tmp[kWepMax] = {};
        bool ok = true;
        for (int i = 0; i < count && ok; ++i)
        {
            void* c = ((void**)data)[i];
            if (!LooksLikeObject(c)) { ok = false; break; }
            for (int j = 0; j < i; ++j) if (tmp[j] == c) { ok = false; break; }
            tmp[i] = c;
        }
        if (!ok) continue;

        const int k = g_nWepCand++;
        g_wepCount[k] = count;
        g_wepAt[k] = (unsigned)off;
        for (int i = 0; i < count; ++i) g_wepList[k][i] = tmp[i];

        Log(">>> WEP: candidate %d at pawn+0x%03X, %d entries", k, (unsigned)off, count);
    }

    if (!g_nWepCand) Log(">>> WEP: no class-list-shaped array found on the pawn.");
}

// Reports, for every candidate array, which slot the live weapon matches.
static void ReportWeaponIdentity(const void* hands)
{
    if (!g_nWepCand || !hands || g_cfg.gunPtrOff <= 0 || !g_cfg.gunPtrBase) return;

    if (!Readable((const uint8_t*)hands + g_cfg.gunPtrOff, 4)) return;
    const void* hold = *(void* const*)((const uint8_t*)hands + g_cfg.gunPtrOff);

    static const void* lastHold = nullptr;
    if (hold == lastHold) return;          // only on a real weapon change
    lastHold = hold;

    if (!hold) { Log(">>> WEP: holdable NULL (plasmid mode)."); return; }

    const size_t hb = ReadableBlock(hold, 0x40);
    for (size_t co = 0; co + 4 <= hb; co += 4)
    {
        void* const cls = *(void* const*)((const uint8_t*)hold + co);
        if (!cls) continue;

        for (int k = 0; k < g_nWepCand; ++k)
            for (int i = 0; i < g_wepCount[k]; ++i)
                if (g_wepList[k][i] == cls)
                    Log(">>> WEP: holdable 0x%08X  Class at +0x%03X = 0x%08X  "
                        "-> candidate %d slot %d",
                        (unsigned)(uintptr_t)hold, (unsigned)co,
                        (unsigned)(uintptr_t)cls, k, i);
    }
}

// ---- PER-WEAPON GRIP OFFSET (measured 2026-07-25) ------------------------
// MEASURED by cycling pistol -> machinegun -> wrench with the WEP probe:
//   AllPossibleWeaponClasses  pawn+0x750   (candidate 0, 8 entries)
//   PreloadClasses            pawn+0x998   (candidate 1, 8 entries -- DECOY)
//   UObject::Class            holdable+0x30
// MachineGun separated them: slot 5 in candidate 0, slot 6 in candidate 1,
// exactly as the two decompiled default lists differ. Candidate 0 is always the
// LOWER offset, because AllPossibleWeaponClasses is declared on ShockPawn while
// PreloadClasses is on the ShockPlayer subclass -- structural, not luck.
static const char* kWepName[9] = {
    "Wrench", "Pistol", "Shotgun", "Crossbow", "GrenadeLauncher",
    "MachineGun", "ChemicalThrower", "ResearchCamera", "Plasmid"
};

// One grip offset per slot, all seeded from HandsGripOffset. The numpad keys
// edit whichever slot is live; switching weapons SAVES the slot you were on and
// LOADS the one you switched to. So a single session tunes every weapon, and
// both values print on every switch ready to paste into the ini.
static float g_gripBySlot[9][3] = {};
static float g_rotBySlot[9][3] = {};
static bool  g_gripInit = false;

static int ResolveWeaponSlot(const void* hands)
{
    if (!hands || g_cfg.gunPtrOff <= 0 || !g_cfg.gunPtrBase) return -1;
    if (!Readable((const uint8_t*)hands + g_cfg.gunPtrOff, 4)) return -1;

    const void* hold = *(void* const*)((const uint8_t*)hands + g_cfg.gunPtrOff);
    if (!hold) return 8;                       // plasmid / ability mode

    int k = -1;
    for (int i = 0; i < g_nWepCand; ++i) if (g_wepCount[i] == 8) { k = i; break; }
    if (k < 0) return -1;

    if (!Readable((const uint8_t*)hold + 0x30, 4)) return -1;
    void* const cls = *(void* const*)((const uint8_t*)hold + 0x30);

    for (int i = 0; i < 8; ++i) if (g_wepList[k][i] == cls) return i;
    return -1;
}

// ---- IDLE HANDS ANIMATION ------------------------------------------------
// The sway IS the idle fidget. state WeaponIdling loops
// PlayAnimationOnChannelFlatEaseIn(0, GetIdlingHandsAnim(), ...) forever, and
// the weapon hangs off a bone of that mesh via AttachBone, so authored fidget
// motion lands on the gun.
//
// MEASURED TWICE -- DO NOT REPEAT: writing name index 0 ('None') hangs the
// GAME THREAD about one animation cycle later. That guard sits inside a
// while(true) whose only latent call is in the taken branch, so 'None'
// re-queries forever with nothing to yield on. ALWAYS write a different VALID
// name and the loop keeps yielding on FinishAnimation.
//
//   1  every entry -> entry[0]. Kills the wrench slap (FidgetSlapWrench is
//      entry[1], weight 50) and leaves one ordinary fidget.
//   2  every entry -> Hands::HandsOffscreenAnimationName (hands+0x498). No
//      motion, but the arms park OFF SCREEN -- only usable with HideArms.
//   3  every entry -> Holdable::EquippingHandsAnim (holdable+0x480). No idle
//      motion AND the arms hold the weapon-ready pose. This is the wrench one.
//
// Layout measured in S9: IdlingHandsAnim is a TArray<FName> at holdable+0x458
// ({Data, Count, Max}); FName is 8 bytes {Index, Number}.
static void ApplyIdleAnim(const void* hands, int slot)
{
    const int mode = (slot >= 0 && slot <= 8) ? g_cfg.idleModeSlot[slot]
        : g_cfg.idleAnimMode;
    if (mode <= 0) return;
    if (!Readable((const uint8_t*)hands + 0x45C, 4)) return;

    uint8_t* const hold = *(uint8_t* const*)((const uint8_t*)hands + 0x45C);
    if (!hold) return;                        // ability mode -- no holdable
    if (!Readable(hold + 0x458, 12)) return;

    uint8_t* const d = *(uint8_t* const*)(hold + 0x458);
    const int n = *(const int*)(hold + 0x45C);
    if (!d || n < 1 || n > 8) return;
    if (!Writable(d, (size_t)n * 8)) return;

    uint32_t idx = *(const uint32_t*)(d);         // entry[0]
    uint32_t num = *(const uint32_t*)(d + 4);

    if (mode == 2 && Readable((const uint8_t*)hands + 0x498, 8))
    {
        idx = *(const uint32_t*)((const uint8_t*)hands + 0x498);
        num = *(const uint32_t*)((const uint8_t*)hands + 0x49C);
    }
    else if (mode == 3 && Readable(hold + 0x480, 8))
    {
        idx = *(const uint32_t*)(hold + 0x480);
        num = *(const uint32_t*)(hold + 0x484);
    }

    if (!idx)
    {
        Log("!!! IDLE: source name is 0 -- REFUSING. That hangs the game.");
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        *(uint32_t*)(d + i * 8) = idx;
        *(uint32_t*)(d + i * 8 + 4) = num;
    }
    Log(">>> IDLE: slot %d, %d entr%s -> name %u (mode %d)",
        slot, n, (n == 1) ? "y" : "ies", idx, mode);
}

static void UpdateWeaponGrip(const void* hands)
{
    if (!g_gripInit)
    {
        for (int i = 0; i < 9; ++i)
            for (int a = 0; a < 3; ++a)
            {
                g_gripBySlot[i][a] = g_cfg.gripSlot[i][a];
                g_rotBySlot[i][a] = g_cfg.rotSlot[i][a];
                g_cursorBySlot[i][a] = g_cfg.cursorSlot[i][a];
            }
        g_gripInit = true;
    }

    const int slot = ResolveWeaponSlot(hands);
    if (slot < 0 || slot == g_wepSlot) return;

    if (g_wepSlot >= 0)
    {
        for (int a = 0; a < 3; ++a)
        {
            g_gripBySlot[g_wepSlot][a] = g_cfg.handsGrip[a];
            g_rotBySlot[g_wepSlot][a] = g_cfg.handsRot[a];
            g_cursorBySlot[g_wepSlot][a] = g_cfg.cursorRot[a];
        }
        Log(">>> GRIP: saved  slot %d %-16s GripOffset%d=%.1f,%.1f,%.1f  RotOffset%d=%.0f,%.0f,%.0f",
            g_wepSlot, kWepName[g_wepSlot],
            g_wepSlot, g_gripBySlot[g_wepSlot][0], g_gripBySlot[g_wepSlot][1], g_gripBySlot[g_wepSlot][2],
            g_wepSlot, g_rotBySlot[g_wepSlot][0], g_rotBySlot[g_wepSlot][1], g_rotBySlot[g_wepSlot][2]);
    }

    g_wepSlot = slot;
    for (int a = 0; a < 3; ++a)
    {
        g_cfg.handsGrip[a] = g_gripBySlot[slot][a];
        g_cfg.handsRot[a] = g_rotBySlot[slot][a];
        g_cfg.cursorRot[a] = g_cursorBySlot[slot][a];
    }

    Log(">>> GRIP: LIVE   slot %d %-16s pos %.1f,%.1f,%.1f  rot %.0f,%.0f,%.0f",
        slot, kWepName[slot],
        g_cfg.handsGrip[0], g_cfg.handsGrip[1], g_cfg.handsGrip[2],
        g_cfg.handsRot[0], g_cfg.handsRot[1], g_cfg.handsRot[2]);

    ApplyIdleAnim(hands, slot);
}

// ---- QUEST ARROW HUNT (READ ONLY, one shot) -----------------------------
// The arrow tracks the weapon in 3D through a full transform, which is what an
// ATTACHED ACTOR looks like. If it is one, it has Location +0x1D8 like every
// other actor and we can drive it the way we drive the hands -- real world
// placement, correct stereo, and its rotation is preserved so it keeps
// pointing at the objective.
//
// Same method that found the Hands: anything sitting within a metre of the gun
// is a short list, and the gun's own Location is already known.
static void ProbeNearGun(const void* pawn, const void* hands, const void* gun)
{
    static bool done = false;
    if (done || !pawn || !hands || !gun) return;
    if (!Readable((const uint8_t*)gun + 0x1D8, 12)) return;
    done = true;

    const float* const g = (const float*)((const uint8_t*)gun + 0x1D8);
    Log(">>> ARROW: searching within 150 cm of the gun at %.1f %.1f %.1f",
        g[0], g[1], g[2]);

    struct Src { const char* name; const uint8_t* base; unsigned lo, hi; };
    const Src srcs[2] = {
        { "pawn",  (const uint8_t*)pawn,  0x450, 0x1000 },
        { "hands", (const uint8_t*)hands, 0x450, 0x0800 },
    };

    int hits = 0;
    for (int s = 0; s < 2 && hits < 24; ++s)
    {
        for (unsigned o = srcs[s].lo; o + 4 <= srcs[s].hi && hits < 24; o += 4)
        {
            if (!Readable(srcs[s].base + o, 4)) continue;
            const void* const p = *(const void* const*)(srcs[s].base + o);
            if (!p || p == gun || p == hands || p == pawn) continue;
            if (!LooksLikeObject(p)) continue;
            if (!Readable((const uint8_t*)p + 0x1D8, 12)) continue;

            const float* const L = (const float*)((const uint8_t*)p + 0x1D8);
            const double dx = (double)L[0] - g[0];
            const double dy = (double)L[1] - g[1];
            const double dz = (double)L[2] - g[2];
            const double d = sqrt(dx * dx + dy * dy + dz * dz);
            if (d > 150.0) continue;

            Log("   %s+0x%03X -> 0x%08X   %6.1f cm   loc %.1f %.1f %.1f",
                srcs[s].name, o, (unsigned)(uintptr_t)p, d, L[0], L[1], L[2]);
            ++hits;
        }
    }
    if (!hits) Log("   nothing within 150 cm. The arrow is not reachable this way.");
}

// ---------------------------------------------------------------- driver

void HandsProbe_Observe(void* playerController,
    const float camLoc[3], const int camRot[3])
{
    if (!g_cfg.handsProbe) return;
    if (!playerController || !camLoc || !camRot) return;

    PollGripKeys();          // always live, even before the probe locks

    // NO LEVEL, NO PROBE. On the main menu the camera sits at the origin, so
    // every zeroed vector in memory reads as "57 cm from the camera" and the
    // probe locks junk -- then we write Location, Rotation and DrawScale into
    // objects the level load frees. That is the crash.
    if (!GameState_InGame()) return;

    // The arm delay exists because a scan during level load locks freed objects
    // and we then write Location/Rotation/DrawScale into them -- that was the
    // S45 crash. GameState_InGame() above is the real guard; this is belt and
    // braces, so it is tunable rather than baked at 600 (2.7 s at 220 calls/s).
    if (++g_calls < g_cfg.handsArmCalls) return;

    if (!g_locOff)
    {
        // FIRST attempt goes straight through. The old code made it pay the
        // retry interval too, doubling the wait on every level load for nothing.
        static bool tried = false;
        if (tried && ++g_retry < g_cfg.handsRetryCalls) return;
        tried = true;
        g_retry = 0;
        FindLocationAndPawn(playerController, camLoc);
        return;
    }

    // Explicit selection wins: pass 2 reports several view-tracking objects and
    // only you can see which one is the arms. HandsPtrOffset picks one.
    if (g_cfg.handsPtrOff > 0 && g_pawn)
    {
        const size_t po = (size_t)g_cfg.handsPtrOff;
        if (Readable((const uint8_t*)g_pawn + po, 4))
        {
            void* t = *(void**)((const uint8_t*)g_pawn + po);
            if (LooksLikeObject(t) && t != g_hands)
            {
                g_hands = t;
                Log(">>> HANDS: using pawn+0x%04X -> 0x%08X (HandsPtrOffset)",
                    (unsigned)po, (unsigned)(uintptr_t)t);
            }
        }
    }

    if (!g_hands)
    {
        // S45: CAP IT. Stage B walks 0x4000 bytes with a VirtualQuery per
        // candidate pointer; running that every ~0.35s on the game thread is
        // what made run 1 unplayable. Three attempts, well spaced, then stop.
        static int attempts = 0;
        if (attempts >= 6) return;
        if (++g_retry < 1200) return;          // ~10s apart
        g_retry = 0;
        if (FindHands(g_pawn, camLoc, camRot)) ++attempts;
        if (attempts >= 6 && !g_hands)
            Log(">>> HANDS: STAGE B gave up after 6 attempts. Not retrying "
                "(the scan is expensive and the answer is not changing).");
        return;
    }

    ScanHandsForPosition(g_hands, camLoc);
    ApplyHandsScale(g_hands);
    ScanWeaponClassLists(g_pawn);
    ReportWeaponIdentity(g_hands);
    UpdateWeaponGrip(g_hands);
    UpdateHandMode(g_hands);
    PollHandsModeKeys(g_hands);

    // Re-read the weapon pointer EVERY call, not once. Switching weapons swaps
    // the Holdable, so a pointer captured at lock time goes stale the first time
    // you press a number key -- and a stale one would leave the new gun full
    // size while we kept scaling something you are no longer holding.
    if (g_cfg.gunPtrOff > 0)
    {
        void* root = g_cfg.gunPtrBase ? g_hands : g_pawn;
        if (root && Readable((const uint8_t*)root + g_cfg.gunPtrOff, 4))
        {
            void* t = *(void**)((const uint8_t*)root + g_cfg.gunPtrOff);
            if (LooksLikeObject(t) && t != g_gun)
            {
                g_gun = t;
                g_kidCollected = false;     // new weapon, new children
                g_nKidCand = 0;
                Log(">>> GUN: using %s+0x%03X -> 0x%08X (GunPtrOffset)",
                    g_cfg.gunPtrBase ? "hands" : "pawn",
                    (unsigned)g_cfg.gunPtrOff, (unsigned)(uintptr_t)t);
            }
        }
    }

    SweepGunScale(camLoc);
    ProbeNearGun(g_pawn, g_hands, g_gun);

    // Re-lock after a level load, teleport, or respawn. The pawn/hands/gun are
    // rebuilt at NEW addresses, but the OLD memory usually stays readable, so a
    // readability test never fires. Two triggers, either one re-locks:
    //   (1) the camera teleports a long way in one step (load / respawn);
    //   (2) the hands actor drifts off the camera (we hold a dead object).
    {
        static float lastCam[3] = { 0.f, 0.f, 0.f };
        static bool  haveLastCam = false;
        double jump = 0.0;
        if (haveLastCam)
        {
            const double dx = (double)camLoc[0] - lastCam[0];
            const double dy = (double)camLoc[1] - lastCam[1];
            const double dz = (double)camLoc[2] - lastCam[2];
            jump = sqrt(dx * dx + dy * dy + dz * dz);
        }
        lastCam[0] = camLoc[0]; lastCam[1] = camLoc[1]; lastCam[2] = camLoc[2];
        haveLastCam = true;

        const size_t po = (g_cfg.handsPosOff > 0) ? (size_t)g_cfg.handsPosOff : g_locOff;
        double d = -1.0;
        if (Readable((const uint8_t*)g_hands + po, sizeof(AVec)))
            d = Dist((const AVec*)((const uint8_t*)g_hands + po), camLoc);

        static int staleFrames = 0;
        if (d < 0.0 || d > 800.0) ++staleFrames;
        else                      staleFrames = 0;

        if (jump > 5000.0 || staleFrames >= 8)
        {
            Log(">>> HANDS: RE-LOCK (camera jump %.0f cm, hands %.0f cm off camera). "
                "Dropping stale pawn/hands/gun.", jump, d);
            g_pawn = nullptr; g_hands = nullptr; g_gun = nullptr;
            g_locOff = 0; g_retry = 0;
            staleFrames = 0;
            return;
        }
    }

    NudgeTest(g_hands, camLoc, camRot);
}

void HandsProbe_Reset()
{
    // Drop the cached skeleton WITHOUT restoring: on a level change the old
    // actor may already be freed and its address handed to something else.
    ArmHide_Reset();
    Swing_Reset();

    g_locOff = 0;
    g_pawn = nullptr;
    g_hands = nullptr;
    g_gun = nullptr;
    g_gunCollected = false;
    g_nGunCand = 0;
    g_kidCollected = false;
    g_nKidCand = 0;
    g_calls = 0;
    g_retry = 0;
}