// BioshockVR/Camera/CameraHook.cpp
//
// Phase 6: the six-stage FName search (UNCHANGED from Phase 5), plus
//   * automatic site0 detection -- the render view is the site with the most
//     calls, because it is the only one that ticks while standing still (§6c-2)
//   * the EYE TAG FIFO -- CalcView is on the GAME thread, Present is on the
//     RENDER thread (MEASURED: 42040 vs 42432). The eye must travel with the
//     frame, not be read from a shared flag.
//   * the WRITE path: a per-eye camera POSITION offset for AER stereo (§6e)
//
// Gated TWICE:
//   EnableCameraHook=1   installs the hook at all (Phase 5 kill switch)
//   EnableCameraWrite=1  lets it MODIFY CameraLocation (Phase 6 kill switch)
//
// We do NOT touch *CameraRotation in Phase 6. Head tracking is Phase 8, and
// `final == clean` means writing it back would be a no-op anyway.

#include "Camera/CameraHook.h"
#include "Hands/ArmHide.h"
#include "Game/GameState.h"
#include "Game/EngineExec.h"
#include "Input/InputHook.h"
#include "Hands/HandsProbe.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>
#include <intrin.h>     // _ReturnAddress, _InterlockedIncrement

#include <MinHook.h>
#include "Core/Config.h"

#pragma comment(lib, "psapi.lib")

extern void LogFile(const char* msg);

extern void  XR_GetHeadQuat(float out[4]);   // from XRSession.cpp (render thread)
extern void  XR_GetHeadPos(float out[3]);
bool GameState_GetPawnEyePoint(float outPos[3]);   // GameState.cpp
void* GameState_Pawn();

int HandsProbe_WeaponSlot();
bool GameState_Cutscene();   // GameState.cpp
bool GameState_Theater();    // GameState.cpp
bool GameState_ScriptedAnim();          // GameState.cpp
bool GameState_ScriptedWindow();        // GameState.cpp -- the HELD pair
bool GameState_Bathysphere();           // GameState.cpp
bool Input_GetTurnX(float* out);        // InputHook.cpp
bool Input_GetSentStickAngle(float* outDeg);   // InputHook.cpp -- for WalkDrift

static void Log(const char* fmt, ...);   // defined just below

// M7-S4: is the rig actually animating? The script has no usable flag for this
// -- see ArmHide.h. Peak-held, then held a further ScriptedHandsHoldMs so a
// pause inside a chained animation does not flicker the hands out.
//
// THE HOLD IS AN INI VALUE BECAUSE THE RIGHT ANSWER DIFFERS BY SCENE, and this
// signal cannot tell the scenes apart. The plasmid injection holds a pose for
// 2.5 and 4.5 seconds mid-animation -- the rig really is frozen, the gate is
// working as designed, and the arms vanish anyway. But M7's verified win was the
// Little Sister crawl "unhiding for the bottle catch and hidden for the rest",
// which needs the gate sharp. A longer hold fixes one and risks the other, so
// the number is tunable in a headset instead of guessed at here.
static bool ScriptedHandsMoving()
{
    float smoothed = 0.0f, raw = 0.0f;
    if (!ArmHide_HandMotion(&smoothed, &raw)) return false;

    const bool moving = smoothed > g_cfg.scriptedHandsMotion;

    static DWORD s_lastMoving = 0;
    const DWORD now = GetTickCount();
    if (moving) s_lastMoving = now;

    // The raw and smoothed values are what calibrate the threshold, so print
    // them while a sequence is running rather than shipping a guessed constant
    // forever. Throttled; silent outside scripted sequences.
    static DWORD s_lastLog = 0;
    if (GameState_ScriptedAnim() && now - s_lastLog >= 500)
    {
        s_lastLog = now;
        // The BONE is printed because which one is sampled depends on the hand
        // being driven, and a run of exact zeros is only interpretable if you
        // know whether it came from a bone we were writing.
        Log(">>> SCRIPTED: motion raw %.4f smoothed %.4f  thresh %.4f  bone %d "
            "-> %s",
            raw, smoothed, g_cfg.scriptedHandsMotion, ArmHide_MotionBone(),
            moving ? "MOVING" : "still");
    }

    return moving || (s_lastMoving &&
        (now - s_lastMoving) < (DWORD)g_cfg.scriptedHandsHoldMs);
}

// ===========================================================================
//  TURN RATE -- A DIAGNOSTIC, NOT A FIX
//
// REPORTED, LONG-STANDING: "the turn speed with the right thumbstick is
// inconsistent. Sometimes it's slow and sometimes it's fast. I can have a period
// where I am standing still and it's slow, then I release and do it again and
// it's much faster."
//
// WITH ModYaw=0 THE MOD DOES NOT TURN YOU. The right stick is passed through to
// the game as an XInput axis and the game rotates itself; all this file does is
// follow the result. So the rate is the game's own -- but the mod is what
// changed the frame pacing, and a turn applied PER TICK rather than per second
// would vary with load exactly as described.
//
// So measure it before changing anything: degrees of yaw the game actually
// produced in the last second, against the number of CalcView calls that second.
// If degrees track the call count, it is frame-rate dependence, and the fix is
// to drive turning through g_aimBase at ModYawSpeed -- machinery that already
// exists and that the tester has just confirmed feels right during scripted
// sequences, where it is the only thing turning them.
//
// Logs only while the stick is meaningfully deflected, once a second, so an
// ordinary session is a handful of lines. `dY` is in UE rotator units.
static void TurnRateProbe(int dY)
{
    if (!g_cfg.turnRateProbe) return;

    float tx = 0.0f;
    const bool deflected = Input_GetTurnX(&tx) && fabsf(tx) > 0.5f;

    static double s_degrees = 0.0;
    static int    s_calls = 0;
    static DWORD  s_since = 0;
    static bool   s_wasDeflected = false;

    const DWORD now = GetTickCount();

    // A fresh push starts a fresh measurement. The complaint is specifically
    // about one push differing from the next, so carrying a partial second
    // across a release would average away the thing being measured.
    if (deflected && !s_wasDeflected)
    {
        s_degrees = 0.0; s_calls = 0; s_since = now;
    }
    s_wasDeflected = deflected;

    if (!deflected) return;

    s_degrees += fabs((double)dY) / 182.0444;
    ++s_calls;

    if (s_since && now - s_since >= 1000)
    {
        const double secs = (double)(now - s_since) / 1000.0;
        Log(">>> TURNRATE: %.1f deg/s over %.2fs  (%d CalcView calls, %.0f/s)  "
            "stick %.2f",
            s_degrees / secs, secs, s_calls, s_calls / secs, tx);
        s_degrees = 0.0; s_calls = 0; s_since = now;
    }
}

// ===========================================================================
//  WHAT TURNS THE PLAYER DURING A SCRIPTED SEQUENCE
//
// THE FIRST VERSION OF THIS PROBE MEASURED THE WRONG THING, and it is worth
// saying why. It logged the ABSOLUTE change in three rotation fields across a
// scripted window and reported ~95 degrees on all three -- which read as "the
// rotation is arriving fine". The tester supplied the missing fact: that 95
// degrees was THEM, turning with the right stick. A probe that cannot separate
// player input from engine injection answers a different question than the one
// asked.
//
// So this one logs `dY`, which the surrounding code already defines as "only the
// GAME's own change since our last write" -- player turning is excluded by
// construction. Alongside it goes the game's own camera yaw and all four gates,
// so a rotation that arrived and was DISCARDED is distinguishable from one that
// never arrived.
//
// Silent outside a scripted window; one line a second inside one.
static void ScriptedRotProbe(bool scripted, int dY, int32_t cleanYaw,
    bool cut, bool freeze, bool gameplayFreeze, bool rotBlocked)
{
    if (!g_cfg.scriptedRotProbe) return;

    static bool     s_was = false;
    static double   s_injected = 0.0;      // sum of |dY|, the game's own
    static double   s_camera = 0.0;        // sum of |d cleanRot.yaw|
    static int32_t  s_prevClean = 0;
    static DWORD    s_last = 0;

    const DWORD now = GetTickCount();

    if (scripted && !s_was)
    {
        s_injected = 0.0; s_camera = 0.0;
        s_prevClean = cleanYaw;
        s_last = now;
        Log(">>> SCRIPTROT: window began.");
    }
    else if (!scripted && s_was)
    {
        Log(">>> SCRIPTROT: window ended.");
    }
    s_was = scripted;
    if (!scripted) return;

    s_injected += fabs((double)dY) / 182.0444;
    s_camera += fabs((double)(short)(cleanYaw - s_prevClean)) / 182.0444;
    s_prevClean = cleanYaw;

    if (now - s_last < 1000) return;
    const double secs = (double)(now - s_last) / 1000.0;
    s_last = now;

    Log(">>> SCRIPTROT: game injected %.2f deg/s into the AIM FIELD, %.2f deg/s "
        "onto its own CAMERA  | gates cut=%d freeze=%d gameplayFreeze=%d "
        "rotBlocked=%d",
        s_injected / secs, s_camera / secs,
        (int)cut, (int)freeze, (int)gameplayFreeze, (int)rotBlocked);

    s_injected = 0.0; s_camera = 0.0;
}

// M7-S2. THE FIRST REAL SCRIPTED-EVENT SIGNAL THIS PROJECT HAS HAD.
// hands+0x594 bit 2, measured exact on both edges and zero false positives over
// six minutes of mixed play (docs/ENGINE-MAP.md § Hands actor).
//
// DELIBERATELY NOT ROUTED THROUGH TheaterMode(). That predicate is ANDed with
// g_cfg.cutsceneTheater, which docs/INVARIANTS.md records as falsified because
// it forces every cutscene onto the flat quad -- so gating the arms on it meant
// they could only ever unhide as a side effect of a setting nobody should use.
// Different question, different switch.
//
// SCOPE, and do not widen it by assumption: this is scripted hand ANIMATION
// SEQUENCES. The Little Sister rescue and the EVE injection were both MEASURED
// leaving this bit clear -- they are Hands states, a different mechanism.
static bool ScriptedQol()
{
    return g_cfg.scriptedQol && GameState_ScriptedAnim();
}

// M7-S6: A WIDER WINDOW, AND ONLY FOR THE AIM.
//
// The aim write must also stand down while the game is FORCE-MOVING the player,
// which happens BEFORE the scripted animation starts and for the whole of a
// bathysphere boarding. Writing Controller.Rotation through that window fights
// the interpolation -- the measured entry stall.
//
// DELIBERATELY SEPARATE FROM ScriptedQol(). A forced move is not an animation,
// so the arms and hands must keep using the motion signal; making them follow
// this would show them during boarding, when nothing is animating. Two
// questions, two predicates, and they must not be merged.
// ===========================================================================
//  WHO AIMS -- TWO PREDICATES, AND KEEPING THEM APART IS THE WHOLE POINT
//
// NEITHER OF THESE IS MovementMode ANY MORE. Until 2026-08-11 mode 0 meant both
// "your head aims" and "your head steers", which made the other three modes
// impossible to express. Locomotion moved out to InputHook's R term; what is
// left here is only the question of what the AIM FIELD carries.
//
// AimUsesHeadNow() decides what goes into the aim field, and it is DYNAMIC --
// it flips with empty hands. Safe, because locomotion no longer reads it: the R
// term cancels whatever the aim field happens to carry, so the two can disagree
// without the head being applied twice. That double application was the measured
// bug behind "turning 90 degrees left almost moves you backwards" (90 twice is
// 180), and the reason it cannot return is now structural rather than a rule.
//
// ModeUsesHead() decides how the VIEW is composed and is deliberately STATIC --
// the config flag alone, never the empty-hands term. Picking a weapon up must
// not change how the world is presented; it was asked for as "don't tie this to
// anything else", and a view that recomposes as you holster is exactly the side
// effect that warns against. WIRING THIS ONE TO AimUsesHeadNow() WOULD DO THAT.
// ===========================================================================
static bool ModeUsesHead()
{
    return g_cfg.headAimAlways != 0;
}

static bool AimUsesHeadNow()
{
    if (ModeUsesHead()) return true;

    // Empty hands: no crosshair, so nothing shows where the controller points.
    // Looking at a thing to pick it up is the natural fallback.
    return g_cfg.headAimUnarmed && !HandsProbe_Armed();
}

// Exported so InputHook gates the stick rotation on the SAME answer rather than
// a second copy of the reasoning.
bool CameraHook_AimUsesHead() { return AimUsesHeadNow(); }

// ===========================================================================
//  DO WE OWN THE AIM FIELD THIS FRAME?
//
// THE REGRESSION THIS EXISTS TO PREVENT, MEASURED 2026-08-11. InputHook redirects
// walking by rotating the movement stick by R, and R is derived from the identity
//
//     walk = aimFieldYaw + stickAngle + R
//
// which assumes the aim field carries OUR composed heading, base + C. Two of the
// four movement modes SUBTRACT the head and controller terms on that assumption.
//
// During a scripted sequence the assumption is false. M7-S3 deliberately
// suppresses the write into Controller.Rotation -- grep "THE SUPPRESSION LIVES
// HERE NOW" -- so the field carries the GAME's heading, containing neither term.
// Subtracting them then rotates the walk off the path the scene intends. The
// tester's words: "when you enter it, you are turned slightly to the left, which
// causes the walking path to move you to the wrong position."
//
// IT LOOKED LIKE A MODE-0-ONLY BUG AND WAS NOT. Mode 2 subtracts only O, which is
// zero while the head owns the aim -- so an UNARMED scripted scene masks it
// completely and an armed one drifts by up to AimClampDeg. The mode that appeared
// clean was the one that got lucky.
//
// GAME THREAD writes this from CalcView; the XInput detour reads it. One aligned
// LONG, so a torn read is not possible; the interlocked exchange is for the
// barrier, not the atomicity.
static volatile LONG g_ownsAimField = 0;

bool CameraHook_OwnsAimField()
{
    // Starvation checked here rather than trusted from the flag: if CalcView
    // stops being called the flag keeps its last value forever, and "we own the
    // aim" is the dangerous direction to be stale in.
    return g_ownsAimField != 0 && !CameraHook_Starved();
}

// ---- A SCRIPTED SEQUENCE YOU CAN WALK THROUGH ---------------------------
// The HUD peak-hold and the verdict both MOVED to GameState.cpp, and the move
// is the fix rather than tidying. This used to be decided here, per frame, from
// a render-thread draw counter -- so it could flip in the MIDDLE of a sequence,
// which resumed the aim write while the game was still moving the player and
// threw the balcony landing 3.7 m off. GameState now takes the verdict ONCE per
// animation and freezes it. See the ONE VERDICT PER SCRIPTED ANIMATION banner
// there for the measurement.
//
// The policy stays here; only the fact moved.
static bool ScriptedButInControl()
{
    return g_cfg.controllableScripted && GameState_ScriptedInControl();
}

// ---- THE WINDOW IS HELD, AND NOT HERE ------------------------------------
// One scene raises two signals in SEQUENCE -- the forced move, then the scripted
// animation -- and their union can gap for a single frame when the order
// reverses. One frame was enough to release the aim, re-arm the base from a
// field reading exactly (0,0), and leave the Little Sister crawl 18.6 deg off
// for 58 seconds. The measurement is in GameState.cpp, grep ONE SCENE, ONE
// WINDOW, and in docs/INVARIANTS.md.
//
// THE HOLD IS ON THE SHARED PAIR, in GameState, deliberately. Two consumers read
// that pair with different policies on top: this one narrows it with
// ScriptedButInControl() so a walk-through scene keeps your aim, while
// InputHook's turn release stays wider -- and the note on its ScriptedQol() says
// in as many words not to weld the two together without a headset. Holding the
// signal they share fixes both without touching either policy.
static bool ScriptedAimReleased()
{
    if (!g_cfg.scriptedQol) return false;

    // KEEP THE AIM when the sequence is one you can move in. Releasing it is
    // what leaves head-look unable to steer and walking following the game's own
    // heading -- right for a sequence that owns you, wrong for one that does not.
    //
    // The old `&& !GameState_ForcedMove()` guard is GONE, not lost: a forced move
    // now settles the verdict itself, permanently, for the rest of the animation.
    // One place encodes that rule, because two places encoding it is how the
    // window came to break mid-scene in the first place.
    //
    // Safe to evaluate per frame inside the hold: the verdict can only be
    // OWN_PLAYER for a scene that never released the aim in the first place, and
    // it resets to UNDECIDED -- which reads as "the game owns you" -- on the
    // animation's falling edge.
    if (ScriptedButInControl()) return false;

    return GameState_ScriptedWindow();
}

// One predicate, four call sites. Cheap: a cached pointer and two strcmps.
bool DrawHook_NoWorldRender();   // DrawHook.cpp

static bool TheaterMode()
{
    return g_cfg.cutsceneTheater &&
        (DrawHook_NoWorldRender() || GameState_Theater());
}

bool GameState_Paused();     // GameState.cpp
void GameState_PitchSample(double degThisSecond);   // GameState.cpp
bool DrawHook_MenuUp();   // DrawHook.cpp

// TRUE while an IN-GAME UI that should sit on the world-locked quad is up --
// the tonic/plasmid slot screen, hacking, vending, the map. These do NOT pause,
// so GameState_Paused() cannot see them, and MenuIndexCounts cannot either:
// Hooks.cpp only lets a draw signature reach the quad when GameState_InGame()
// is FALSE. Separate list, separate consumer -- so a wrong entry here can never
// freeze the camera the way a wrong MenuIndexCounts entry did.
bool DrawHook_AnchorUp();

// TRUE while a screen is being shown as the game's own composed picture on the
// menu quad. See the banner in DrawHook.cpp.
bool DrawHook_ComposedFrameUp();

// ---- WHEN THE VIEW HOLDS STILL BEHIND AN INTERFACE ------------------------
// The pause menu already freezes the head-driven camera and the hands, latching
// them where they were on entry, "because inventory and vending all render the
// world behind the UI, and having it swing with your head while you read is
// disorienting."
//
// THE SAME IS TRUE WHEN THE GAME IS NOT PAUSED. A machine's second page unpauses
// while its interface stays up (grep ONE SCENE, ONE WINDOW's sibling finding in
// .planning/STATE.md), so the world behind the panel kept rendering live: head
// motion slid the picture and the gun swayed inside a screen the player was
// trying to read as a still image. Tester: it "should not track head or arm/gun
// movement in the background so it doesn't change the background and looks
// paused instead of gameplay", and "the pause menu logic should have the same
// thing".
//
// So this is not a new mechanism -- it is the pause freeze, asked the right
// question. One predicate, four sites, so they cannot drift apart.
static bool ViewHeldForUi()
{
    return GameState_Paused() || DrawHook_ComposedFrameUp();
}

bool GameState_InGame();  // GameState.cpp

// Pawn eye point: actor Location + eye height, i.e. the view origin BEFORE the
// engine adds walk bob, landing dip and damage shake in CalcView. Returns false
// unless the pawn is known and both fields read sane.
bool GameState_GetPawnEyePoint(float outPos[3]);

static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

static float g_lastHeadPos[3] = {};
static double g_lastCleanYaw = 0.0;

// ---------------------------------------------------------------- types

struct FVector { float   x, y, z; };            // 1 unit == 1 CENTIMETRE (MEASURED, §6b-note)
struct FRotator { int32_t pitch, yaw, roll; };   // low 16 bits: 65536 == 360 deg

// MSVC __thiscall on x86: `this` in ECX, stack args right-to-left, callee cleans.
// You cannot DEFINE a free function as __thiscall, so we use __fastcall with a
// dummy EDX parameter: identical register + stack layout, also callee-cleans.
typedef void(__fastcall* CalcViewFn)(
    void* pThis,        // APlayerController*   (ECX)
    void* edx_unused,   //                      (EDX, never read)
    void** ViewActor,    // AActor**
    FVector* CameraLocation,   // OUT
    FRotator* CameraRotation);  // OUT

static CalcViewFn g_orig = nullptr;
static void* g_fnAddr = nullptr;

static uint8_t* g_modBase = nullptr;
static size_t   g_modSize = 0;

// ---------------------------------------------------------------- the eye FIFO
// Single producer (game thread, in hkCalcView) / single consumer (render thread,
// in hkPresent). x86 + volatile + Interlocked is sufficient here.

static volatile long  g_eyeWr = 0;
static volatile long  g_eyeRd = 0;
static unsigned char  g_eyeQ[64] = {};

static volatile long  g_qMin = 0x7FFFFFFF;
static volatile long  g_qMax = -1;
static volatile long  g_underruns = 0;
static int            g_lastEye = 1;   // so the first underrun yields eye 0
static volatile long  g_needResync = 0;    // set on underrun; cleared on resync
static volatile long  g_lastPushTick = 0;  // GetTickCount at last tag push (menu detect)
static long           g_deepPops = 0;      // consecutive pops with depth > 1
static FVector g_lastCamCenter = {};

// How far the camera travelled between the eye-0 and eye-1 renders -- i.e. how
// far the WORLD slid between the two eye images. This is the exact quantity the
// 10a clamp is supposed to drive to zero, so it is how we judge it.
static double g_ieLast = 0.0, g_ieSum = 0.0, g_ieMax = 0.0;
static long   g_ieN = 0;

// S80: how far the camera travelled between the eye-0 and eye-1 renders --
// i.e. how far the WORLD slid between the two eye images. The exact quantity
// behind the bathysphere doubling.
static double g_interEyeMove = 0.0;
double CameraHook_InterEyeMove() { return g_interEyeMove; }

// With head-aim the head reaches the view INDIRECTLY (we write the aim field,
// the game derives CameraRotation from it NEXT call). So the image is rendered
// from the PREVIOUS pair's head pose, and stamping the current one re-opens the
// §2 render/layer mismatch -- i.e. the flicker comes back.
static float g_prevQuat[4] = { 0.f, 0.f, 0.f, 1.f };
static bool  g_prevQuatValid = false;

// ---------------------------------------------------------------- latched pose
// game->render seqlock (flicker fix, §2). The mirror of XRSession's head
// seqlock. Written once per pair in the eye-0 latch; read in SubmitPair.
static volatile long g_lpSeq = 0;
static float         g_lpQuat[4] = { 0.f, 0.f, 0.f, 1.f };
static float         g_lpPos[3] = { 0.f, 0.f, 0.f };
static volatile long g_lpValid = 0;

// ---------------------------------------------------------------- memory safety

static bool IsMemoryValid(const void* addr, size_t size)
{
    if (!addr || !size) return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

    switch (mbi.Protect & 0xFF)
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        break;
    default:
        return false;
    }

    const uint8_t* rs = (const uint8_t*)mbi.BaseAddress;
    const uint8_t* re = rs + mbi.RegionSize;
    const uint8_t* a = (const uint8_t*)addr;
    return (a >= rs) && (a + size <= re);
}

// The WRITE path needs the page to actually be writable, which IsMemoryValid
// does not guarantee (it accepts PAGE_READONLY). Stack locals always are, but
// check anyway -- this is the first phase that writes to the game.
static bool IsMemoryWritable(const void* addr, size_t size)
{
    if (!addr || !size) return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

    switch (mbi.Protect & 0xFF)
    {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        break;
    default:
        return false;
    }

    const uint8_t* rs = (const uint8_t*)mbi.BaseAddress;
    const uint8_t* re = rs + mbi.RegionSize;
    const uint8_t* a = (const uint8_t*)addr;
    return (a >= rs) && (a + size <= re);
}

static bool IsExecutable(const void* addr)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
        p == PAGE_EXECUTE || p == PAGE_EXECUTE_WRITECOPY;
}

// ---------------------------------------------------------------- module scan

struct Region { uint8_t* base; size_t size; };

static void EnumReadableRegions(std::vector<Region>& out)
{
    uint8_t* p = g_modBase;
    uint8_t* end = g_modBase + g_modSize;

    while (p < end)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.RegionSize == 0) break;

        uint8_t* rb = (uint8_t*)mbi.BaseAddress;
        uint8_t* re = rb + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        {
            uint8_t* s = (rb > g_modBase) ? rb : g_modBase;
            uint8_t* e = (re < end) ? re : end;
            if (e > s) out.push_back({ s, (size_t)(e - s) });
        }
        p = re;
    }
}

// Every 4-byte little-endian occurrence of `value` in readable module memory.
static void FindDwordRefs(const std::vector<Region>& regs, uint32_t value,
    std::vector<uint8_t*>& out, size_t cap)
{
    for (const Region& r : regs)
    {
        if (r.size < 4) continue;
        uint8_t* e = r.base + r.size - 4;
        for (uint8_t* p = r.base; p <= e; ++p)
        {
            if (*(uint32_t*)p == value)
            {
                out.push_back(p);
                if (out.size() >= cap) return;
            }
        }
    }
}

// ---------------------------------------------------------------- the six stages
// UNCHANGED FROM PHASE 5. Works: 31ms, module+0x1BE7A0. Do not touch it.

static void* FindCalcView()
{
    std::vector<Region> regs;
    EnumReadableRegions(regs);
    Log("camera: %u readable regions in module", (unsigned)regs.size());
    if (regs.empty()) { Log("camera: STAGE 0 FAIL - no readable module memory"); return nullptr; }

    // --- STAGE 1: the wide string "PlayerCalcView" (UTF-16LE) ---
    const wchar_t* kName = L"PlayerCalcView";
    const size_t   kLen = 14 * sizeof(wchar_t);   // 28 bytes, no terminator

    std::vector<uint8_t*> strHits;
    for (const Region& r : regs)
    {
        if (r.size < kLen) continue;
        uint8_t* e = r.base + r.size - kLen;
        for (uint8_t* p = r.base; p <= e; ++p)
        {
            if (*(uint16_t*)p != (uint16_t)L'P') continue;   // cheap first filter
            if (memcmp(p, kName, kLen) == 0)
            {
                strHits.push_back(p);
                if (strHits.size() >= 16) break;
            }
        }
        if (strHits.size() >= 16) break;
    }

    Log("camera: STAGE 1  \"PlayerCalcView\" wide-string hits: %u", (unsigned)strHits.size());
    for (uint8_t* h : strHits) Log("camera:          str @ 0x%08X", (unsigned)(uintptr_t)h);
    if (strHits.empty()) { Log("camera: STAGE 1 FAIL - string not found. STOP."); return nullptr; }

    for (uint8_t* strAddr : strHits)
    {
        // --- STAGE 2: PUSH <strAddr>  (68 imm32) inside executable memory ---
        std::vector<uint8_t*> raw;
        FindDwordRefs(regs, (uint32_t)(uintptr_t)strAddr, raw, 64);

        std::vector<uint8_t*> pushXrefs;
        for (uint8_t* p : raw)
        {
            if (p == g_modBase) continue;
            if (!IsMemoryValid(p - 1, 5)) continue;
            if (p[-1] != 0x68) continue;          // PUSH imm32
            if (!IsExecutable(p - 1)) continue;
            pushXrefs.push_back(p - 1);           // address of the 0x68
        }

        Log("camera: STAGE 2  str 0x%08X -> %u raw refs, %u PUSH xrefs",
            (unsigned)(uintptr_t)strAddr, (unsigned)raw.size(), (unsigned)pushXrefs.size());
        if (pushXrefs.empty()) continue;

        for (uint8_t* xref : pushXrefs)
        {
            Log("camera:          PUSH xref @ 0x%08X", (unsigned)(uintptr_t)xref);

            // --- STAGE 3: forward <=96 bytes: next E8, then next 89 0D imm32 ---
            uint32_t nameGlobal = 0;
            bool sawCall = false;

            for (int i = 0; i < 96; ++i)
            {
                uint8_t* q = xref + i;
                if (!IsMemoryValid(q, 6)) break;

                if (!sawCall)
                {
                    if (q[0] == 0xE8) sawCall = true;    // CALL rel32
                    continue;
                }
                if (q[0] == 0x89 && q[1] == 0x0D)        // MOV [imm32], ECX
                {
                    nameGlobal = *(uint32_t*)(q + 2);
                    break;
                }
            }

            if (!nameGlobal)
            {
                Log("camera: STAGE 3  no '89 0D' within 96 bytes (sawCall=%d). Next xref.",
                    (int)sawCall);
                continue;
            }
            Log("camera: STAGE 3  NAME_PlayerCalcView.Index global = 0x%08X", nameGlobal);

            // --- STAGE 4: xrefs to that global, skipping any within 200 bytes ---
            std::vector<uint8_t*> gRaw;
            FindDwordRefs(regs, nameGlobal, gRaw, 512);

            std::vector<uint8_t*> gXrefs;
            for (uint8_t* p : gRaw)
            {
                ptrdiff_t d = p - xref;
                if (d > -200 && d < 200) continue;    // too close to the init site
                if (!IsExecutable(p)) continue;
                gXrefs.push_back(p);
            }

            Log("camera: STAGE 4  global refs: %u raw, %u surviving (exec, >200B away)",
                (unsigned)gRaw.size(), (unsigned)gXrefs.size());
            if (gXrefs.empty()) { Log("camera: STAGE 4  none survived. Next xref."); continue; }

            // --- STAGE 5: walk backward <=512 bytes for  CC CC CC 55 8B EC ---
            for (uint8_t* g : gXrefs)
            {
                for (int d = 0; d <= 512; ++d)
                {
                    uint8_t* q = g - d;
                    if (q < g_modBase) break;
                    if (!IsMemoryValid(q, 6)) continue;

                    if (q[0] == 0xCC && q[1] == 0xCC && q[2] == 0xCC &&
                        q[3] == 0x55 && q[4] == 0x8B && q[5] == 0xEC)
                    {
                        uint8_t* fn = q + 3;          // the 'push ebp'
                        if (!IsExecutable(fn)) break;

                        Log("camera: STAGE 5  prologue found. global-xref 0x%08X, back %d bytes",
                            (unsigned)(uintptr_t)g, d);
                        Log("camera: STAGE 6  *** eventPlayerCalcView @ 0x%08X  (module+0x%X) ***",
                            (unsigned)(uintptr_t)fn, (unsigned)(fn - g_modBase));
                        Log("camera:          bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                            fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], fn[6], fn[7]);
                        return (void*)fn;
                    }
                }
            }
            Log("camera: STAGE 5  no MSVC prologue behind any global xref. Next xref.");
        }
    }

    Log("camera: SEARCH EXHAUSTED. No function found. NO HOOK INSTALLED.");
    return nullptr;
}

// ---------------------------------------------------------------- basis math (§6d)

struct Vec3 { double x, y, z; };

static double UnitsToDeg(int32_t u)
{
    return (double)(int16_t)(u & 0xFFFF) * (360.0 / 65536.0);
}

static double UnitsToRad(int32_t u)
{
    return UnitsToDeg(u) * (3.14159265358979323846 / 180.0);
}

// §6d rotator_to_basis, but Phase 6 only needs the RIGHT vector -- that's the
// axis the eye offset slides along. Roll is taken RAW here; the "-roll"
// inversion belongs to apply_world_space_yaw (Phase 8), not here.
static Vec3 RotatorRight(const FRotator& r)
{
    const double p = UnitsToRad(r.pitch);
    const double y = UnitsToRad(r.yaw);
    const double o = UnitsToRad(r.roll);

    const double cp = cos(p), sp = sin(p);
    const double cy = cos(y), sy = sin(y);
    const double cr = cos(o), sr = sin(o);

    const Vec3 right0 = { -sy,      cy,      0.0 };
    const Vec3 up0 = { -sp * cy, -sp * sy,  cp };

    return { right0.x * cr + up0.x * (-sr),
             right0.y * cr + up0.y * (-sr),
             right0.z * cr + up0.z * (-sr) };
}

// ---- motion aim state (game thread writes, render thread reads) ---------
static double g_aimHandYaw = 0.0, g_aimHandPitch = 0.0;
static bool   g_aimHandValid = false;

static volatile long g_aimOffSeq = 0;
static float         g_aimOffYaw = 0.0f, g_aimOffPitch = 0.0f;

static double WrapDeg180(double d)
{
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d;
}

// ===========================================================================
//  AimOverride -- ONE SITE WHERE A FEATURE CAN TAKE THE AIM
//
// Absolute pitch/yaw substituted by whichever feature owns the aim this frame.
// THREE PLANNED FEATURES ALL WANT THIS SAME LINE: aiming down the gun barrel
// (bones 43->44), the wrench tip during a swing, and the two-handed grip.
//
// INTRODUCED EMPTY, ON PURPOSE, BEFORE ANY OF THEM EXIST. This codebase has
// already paid once for two features reaching the same place independently: the
// head yaw applied twice, where the aim field carried it and HeadRelativeMove
// rotated the stick by it again, and 90 degrees became 180. The fix was one
// resolver with a fixed precedence. Building the resolver first means the third
// feature adds a CLAUSE rather than a second site.
//
// PRECEDENCE, when the clauses arrive: barrel/tip first (it is weapon-slot
// scoped, so it already knows when it does not apply), two-handing second, and
// none of them while the head owns the aim -- there the head is the aim by
// contract and a substitution would be a silent third opinion.
static bool AimOverride(double* pitchDeg, double* yawDeg)
{
    (void)pitchDeg; (void)yawDeg;
    return false;                      // no feature owns the aim yet
}

bool CameraHook_GetAimOffset(float* dYawDeg, float* dPitchDeg)
{
    for (int t = 0; t < 8; ++t)
    {
        const long s0 = g_aimOffSeq;
        if (s0 & 1) continue;
        MemoryBarrier();
        const float y = g_aimOffYaw, p = g_aimOffPitch;
        MemoryBarrier();
        if (g_aimOffSeq != s0) continue;
        if (dYawDeg)   *dYawDeg = y;
        if (dPitchDeg) *dPitchDeg = p;
        return g_aimHandValid;
    }
    return false;
}

// --- head tracking (Phase 11) -----------------------------------------------
// OpenXR LOCAL-space head quat -> UE-convention pitch/yaw/roll DEGREES.
// Axis map XR->UE: v_ue = (-v.z, v.x, v.y)   [UE +X == XR -Z look dir,
// UE +Y == XR +X, UE +Z == XR +Y]. Roll extracted RAW; -roll inversion is
// applied later, where the angle is USED (§5). LOG ONLY for now.
static void HeadQuatToDeg(const float q[4], double& pitchDeg, double& yawDeg, double& rollDeg)
{
    const double x = q[0], y = q[1], z = q[2], w = q[3];

    const Vec3 fXR = { -(2 * (x * z + y * w)), -(2 * (y * z - x * w)), -(1 - 2 * (x * x + y * y)) }; // -Zcol
    const Vec3 rXR = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w),   2 * (x * z - y * w) };       // +Xcol

    const Vec3 fwd = { -fXR.z, fXR.x, fXR.y };
    const Vec3 rgt = { -rXR.z, rXR.x, rXR.y };

    const double PI = 3.14159265358979323846;
    double p = asin(fwd.z < -1.0 ? -1.0 : (fwd.z > 1.0 ? 1.0 : fwd.z));
    double yw = atan2(fwd.y, fwd.x);
    double cp = cos(p);

    Vec3 right0, up0;
    if (fabs(cp) > 1e-6)
    {
        right0 = { -sin(yw), cos(yw), 0.0 };
        up0 = { -sin(p) * cos(yw), -sin(p) * sin(yw), cp };
    }
    else
    {
        right0 = { rgt.x, rgt.y, 0.0 };
        up0 = { 0.0, 0.0, (cp < 0 ? -1.0 : 1.0) };
    }
    double rl = atan2(-(rgt.x * up0.x + rgt.y * up0.y + rgt.z * up0.z),
        (rgt.x * right0.x + rgt.y * right0.y + rgt.z * right0.z));

    pitchDeg = p * 180.0 / PI;
    yawDeg = yw * 180.0 / PI;
    rollDeg = rl * 180.0 / PI;
}

// Latched ONCE per pair (on eye 0), held for both eyes (§6 rule). Computed +
// logged this increment; NOT yet written to the camera.
static double g_headPitch = 0.0, g_headYaw = 0.0, g_headRoll = 0.0;

// ---- HIDDEN PITCH SERVO ---------------------------------------------------
// The wrench does NOT use the aim ray. Its Havok collision phantom is aimed
// from the engine's own internal view pitch -- so with right-stick Y dropped,
// that pitch drifts and sticks (measured near -89 deg), and the wrench hits the
// floor no matter where you are looking.
//
// We do NOT write the pitch memory directly. We publish the ERROR here and feed
// proportional right-stick Y through the normal input path, so the engine's own
// clamps and pitch behaviour all still apply.
static volatile float g_pitchErrDeg = 0.0f;    // HMD pitch - engine pitch
static volatile long  g_pitchErrOk = 0;

static void PublishPitchError(int engineRot)
{
    // UE rotator: 65536 units == 360 deg, and it wraps. Normalise to +/-180
    // before converting, or a stored 60000 reads as +329 instead of -31.
    int p = engineRot & 0xFFFF;
    if (p > 32767) p -= 65536;
    const double engineDeg = (double)p / 182.0444;

    double err = g_headPitch - engineDeg;
    while (err > 180.0)  err -= 360.0;
    while (err < -180.0) err += 360.0;

    g_pitchErrDeg = (float)err;
    g_pitchErrOk = 1;
}

// Head yaw relative to the movement heading. Walking is relative to the PAWN,
// so with the head turned 60 deg you still walk where the pawn faces. Rotating
// the stick by this makes "forward" mean "where I am looking".
bool CameraHook_GetHeadYawOffset(float* outDeg)
{
    if (!outDeg) return false;
    double y = g_headYaw;
    while (y > 180.0) y -= 360.0;
    while (y < -180.0) y += 360.0;
    *outDeg = (float)y;
    return true;
}

bool CameraHook_GetPitchError(float* outDeg)
{
    if (!outDeg || !g_pitchErrOk) return false;
    *outDeg = g_pitchErrDeg;
    return true;
}

// Positional tracking (6DOF). Offsets in cm, head-frame (right, up, forward),
// latched once per pair with the rotation. Origin = recenter point.
static double g_posRight = 0.0, g_posUp = 0.0, g_posFwd = 0.0;
static float  g_posOrigin[3] = {};
static bool   g_posOriginSet = false;

// Asymmetric clamps (cm), itsloopyo-proven: lean forward more than back.
static const double kPosSide = 30.0, kPosUpMax = 20.0, kPosDownMax = 20.0;
static const double kPosFwdMax = 40.0, kPosBackMax = 10.0;

// --- full rotator<->basis math (§5), for composing head-look onto the camera ---
struct Basis { Vec3 forward, right, up; };

static Basis RotatorToBasisRad(double p, double y, double o)
{
    const double cp = cos(p), sp = sin(p), cy = cos(y), sy = sin(y), cr = cos(o), sr = sin(o);
    Basis b;
    b.forward = { cp * cy, cp * sy, sp };
    const Vec3 right0 = { -sy, cy, 0.0 };
    const Vec3 up0 = { -sp * cy, -sp * sy, cp };
    b.right = { right0.x * cr + up0.x * (-sr), right0.y * cr + up0.y * (-sr), right0.z * cr + up0.z * (-sr) };
    b.up = { right0.x * sr + up0.x * cr,    right0.y * sr + up0.y * cr,    right0.z * sr + up0.z * cr };
    return b;
}

static Basis RotatorToBasis(const FRotator& r)
{
    return RotatorToBasisRad(UnitsToRad(r.pitch), UnitsToRad(r.yaw), UnitsToRad(r.roll));
}

static FRotator BasisToRotator(const Basis& b)
{
    const double PI = 3.14159265358979323846;
    double fz = b.forward.z < -1.0 ? -1.0 : (b.forward.z > 1.0 ? 1.0 : b.forward.z);
    double p = asin(fz);
    double y = atan2(b.forward.y, b.forward.x);
    double cp = cos(p);
    Vec3 right0, up0;
    if (fabs(cp) > 1e-6)
    {
        right0 = { -sin(y), cos(y), 0.0 };
        up0 = { -sin(p) * cos(y), -sin(p) * sin(y), cp };
    }
    else
    {
        right0 = { b.right.x, b.right.y, 0.0 };
        up0 = { 0.0, 0.0, (cp < 0 ? -1.0 : 1.0) };
    }
    double o = atan2(-(b.right.x * up0.x + b.right.y * up0.y + b.right.z * up0.z),
        (b.right.x * right0.x + b.right.y * right0.y + b.right.z * right0.z));
    FRotator r;
    r.pitch = (int32_t)lround(p * 32768.0 / PI);   // rad -> units (65536 == 2*PI)
    r.yaw = (int32_t)lround(y * 32768.0 / PI);
    r.roll = (int32_t)lround(o * 32768.0 / PI);
    return r;
}

static Vec3 TransformVec(const Basis& b, const Vec3& v)
{
    return { b.forward.x * v.x + b.right.x * v.y + b.up.x * v.z,
             b.forward.y * v.x + b.right.y * v.y + b.up.y * v.z,
             b.forward.z * v.x + b.right.z * v.y + b.up.z * v.z };
}

static Basis MulBasis(const Basis& a, const Basis& b)
{
    return { TransformVec(a, b.forward), TransformVec(a, b.right), TransformVec(a, b.up) };
}

// clean = game's rotator (units); hmd angles in DEGREES. Roll inverted here (§5).
static FRotator ApplyWorldSpaceYaw(const FRotator& clean,
    double hmdYawDeg, double hmdPitchDeg, double hmdRollDeg)
{
    const double D2R = 3.14159265358979323846 / 180.0;
    const double a = hmdYawDeg * D2R, ca = cos(a), sa = sin(a);
    Basis c = RotatorToBasis(clean);
    Basis yawed = {
        { c.forward.x * ca - c.forward.y * sa, c.forward.x * sa + c.forward.y * ca, c.forward.z },
        { c.right.x * ca - c.right.y * sa,   c.right.x * sa + c.right.y * ca,   c.right.z   },
        { c.up.x * ca - c.up.y * sa,      c.up.x * sa + c.up.y * ca,      c.up.z      } };
    Basis pr = RotatorToBasisRad(hmdPitchDeg * D2R, 0.0, hmdRollDeg * D2R);
    return BasisToRotator(MulBasis(yawed, pr));
}

// --- S19: THE WORLD MAP MUST NOT DEPEND ON THE HEAD -------------------------
// A VR camera is coherent only if the room->world transform M is independent of
// head orientation: camera = M . head, M fixed. The compositor assumes exactly
// that when it reprojects our layer from the stamped head pose.
//
// The legacy head-aim write ADDED euler components:
//     camera = Rz(yaw_base + yaw_head) . Ry(pitch_base + pitch_head)
// Solve for M and you get  Rz(yaw_head) . Ry(pitch_base) . Rz(-yaw_head)  --
// a tilt of size pitch_base whose AXIS ROTATES WITH HEAD YAW. So the world
// leans one way looking left and the other looking right, by an amount
// proportional to how far the MOUSE is pitched. That is the turn artifact:
// yaw-triggered, pitch-scaled, roll-innocent.
//
// Fix: apply the whole head rotation in the base's LOCAL frame (right-multiply)
// so M collapses to the mouse-only rotator and stops moving.
//
//   mode 1  M = Rz(yaw_base) . Ry(pitch_base)   -- keeps mouse pitch, but the
//                                                  horizon tilts with it
//   mode 2  M = Rz(yaw_base)                    -- PITCH DECOUPLED: all pitch
//                                                  comes from the head, so the
//                                                  horizon is always level
static FRotator ComposeHeadLocal(const FRotator& base,
    double headYawDeg, double headPitchDeg, bool dropBasePitch)
{
    const double D2R = 3.14159265358979323846 / 180.0;

    FRotator m = base;
    if (dropBasePitch) m.pitch = 0;
    m.roll = 0;                       // M is the player's heading, never rolled

    const Basis M = RotatorToBasis(m);
    const Basis H = RotatorToBasisRad(headPitchDeg * D2R, headYawDeg * D2R, 0.0);
    return BasisToRotator(MulBasis(M, H));
}

// ---- APPLIED SHOT DIRECTION ----------------------------------------------
// The crosshair used to rebuild the aim itself on the render thread from a
// fresh controller pose. That is a SECOND calculation: it re-applied
// CursorOffsetN but skipped the clamp and the smoothing, so the dot and the
// shot could disagree even with identical settings.
//
// Now the game thread publishes the direction it ACTUALLY wrote into the aim
// field, expressed relative to the view it ACTUALLY rendered. One calculation,
// no second algebra to drift.
//
// MUST live below RotatorToBasis and struct Basis -- it uses both.
//
// Published in XR head-local axes (+x right, +y up, -z forward) so the render
// thread can use it directly as a head-locked quad position.
static volatile long g_shotSeq = 0;
static float         g_shotDir[3] = { 0.f, 0.f, -1.f };
static volatile long g_shotOk = 0;

static void PublishShotDir(const FRotator& viewRot, const FRotator& aimRot)
{
    const Basis V = RotatorToBasis(viewRot);
    const Basis A = RotatorToBasis(aimRot);
    const Vec3  s = A.forward;                 // where the shot goes, world space

    // Project onto the view's own axes -> direction relative to the view.
    const double f = s.x * V.forward.x + s.y * V.forward.y + s.z * V.forward.z;
    const double r = s.x * V.right.x + s.y * V.right.y + s.z * V.right.z;
    const double u = s.x * V.up.x + s.y * V.up.y + s.z * V.up.z;

    // UE (fwd,right,up) -> XR head-local (+x right, +y up, -z forward).
    double x = r, y = u, z = -f;
    const double n = sqrt(x * x + y * y + z * z);
    if (n < 1e-6) return;
    x /= n; y /= n; z /= n;

    _InterlockedIncrement(&g_shotSeq);         // odd == writing
    MemoryBarrier();
    g_shotDir[0] = (float)x;
    g_shotDir[1] = (float)y;
    g_shotDir[2] = (float)z;
    MemoryBarrier();
    _InterlockedIncrement(&g_shotSeq);         // even == done
    g_shotOk = 1;
}

bool CameraHook_GetShotDir(float out[3])
{
    if (!out || !g_shotOk) return false;
    for (int t = 0; t < 8; ++t)
    {
        const long s0 = g_shotSeq;
        if (s0 & 1) continue;
        MemoryBarrier();
        out[0] = g_shotDir[0]; out[1] = g_shotDir[1]; out[2] = g_shotDir[2];
        MemoryBarrier();
        if (g_shotSeq == s0) return true;
    }
    return false;
}

// ===========================================================================
//  THE WALK ROTATION -- MEASURED HERE, NOT PREDICTED ON THE OTHER THREAD
//
// The game measures the walk direction from the aim field and then applies the
// stick angle, so rotating the stick by R redirects walking without touching the
// field:
//
//     walk = aimFieldYaw + stickAngle + R
//
// R USED TO BE DERIVED FROM THE HEAD AND CONTROLLER YAWS, on the assumption that
// aimFieldYaw == base.yaw + headYaw + controllerOffset. Reading the field
// instead is strictly more robust, because ComposeHeadLocal is a BASIS
// MULTIPLICATION and in general its yaw depends on the pitch as well.
//
// ⚠ BUT THAT DID NOT FIX THE REPORTED COUPLING, AND THE HONEST REASON MATTERS.
// With HeadAimMode=2 -- the shipping default -- ComposeHeadLocal drops the base
// pitch, so M is a PURE YAW rotation and want.yaw == base.yaw + aimY exactly.
// The old prediction and this measurement are numerically IDENTICAL in that
// configuration, which is exactly why the tester reported the drift as "still
// present and the exact same" after this landed. This form is correct for
// HeadAimMode 0 and 1 and removes a class of future error; it was never capable
// of fixing the drift. CHECK WHICH HEAD-AIM MODE IS LIVE BEFORE BLAMING THE
// COMPOSITION.
//
// WHAT THAT LEAVES: R cancels aimRot.yaw algebraically and exactly, so a
// surviving drift means the line above is wrong -- walking is NOT measured from
// the aim field alone. See WalkDriftProbe below, which measures where the player
// actually went instead of inferring it a third time.
//
// Every value needed is already in hand at the write site, exact and
// post-composition:
//
//   aimRot   what goes into the aim field -- what walking is measured from
//   viewRot  the view actually rendered -- the head's contribution, exactly
//   base     the heading with nothing composed onto it
//
//   mode 0 neither      want base            R = base   - aimRot
//   mode 1 controller   want the field       R = 0
//   mode 2 head         want the view        R = viewRot - aimRot
//   mode 3 both         field + the head     R = viewRot - base
//
// This cancels BY CONSTRUCTION whatever the composition did, at any pitch, with
// no trigonometry of ours left to drift. Same discipline as PublishShotDir
// above, and for the same reason: one calculation, no second algebra.
//
// GAME THREAD writes, the XInput detour reads. Seq-locked.
static volatile long g_walkSeq = 0;
static float         g_walkRotDeg = 0.0f;
static volatile long g_walkOk = 0;

// The cinematic follow's reference. File-scope so the edge detector can drop it
// on both boundaries without being inside the block that uses it -- see the
// banner at `THE CINEMATIC REFERENCE, DROPPED ON BOTH EDGES`.
static bool    g_cineHave = false;
static int32_t g_cinePrev = 0;

// ===========================================================================
//  WHERE THE PLAYER ACTUALLY WENT -- GROUND TRUTH, NOT A THIRD INFERENCE
//
// THE BUG: walking a straight line while pointing the controller 90 degrees to
// one side drifts the path 10-20 degrees the OTHER way, steadily, and mirrors
// exactly when the controller points the other side. Mode 0 promises that
// pointing changes nothing.
//
// TWO EXPLANATIONS HAVE ALREADY FAILED -- the clamp, and the composition (see
// the ⚠ note above). R cancels aimRot.yaw algebraically and exactly, so if drift
// survives then `walk = aimFieldYaw + stickAngle` is simply not the equation the
// game solves. Rather than guess a third time, measure the one thing that cannot
// be argued with: the direction the pawn ACTUALLY travelled, from its own
// position, against the direction mode 0 promised.
//
// Pawn location is pawn+0x1D8, confirmed by the 6-DOF hands probe. The heading
// is atan2 over the horizontal lanes only -- a stair or a slope must not read as
// a turn.
//
// THE LEADING SUSPECT IS ON THE SAME LINE. UE2 builds movement acceleration from
// GetAxes(Pawn.Rotation), so pawn yaw is logged beside the aim field's. A steady
// difference between those two, matching the drift, names the cause outright.
//
// FALSIFIED 2026-08-11, kept only because the probe still reports it as a
// standing check: the pawn's rotator yaw tracks the aim field EXACTLY. 60 of 62
// samples read `aim-pawn +0.0`, including while a 76-degree controller offset was
// held -- the condition the hypothesis was invented for. UE2 does build movement
// from GetAxes(Pawn.Rotation), but the pawn is not where the drift enters.
static bool PawnYaw(int32_t* out)
{
    const uint8_t* const p = (const uint8_t*)GameState_Pawn();
    if (!p || !out) return false;
    if (IsBadReadPtr(p + 0x1E4, 12)) return false;
    *out = ((const int32_t*)(p + 0x1E4))[1];
    return true;
}

// What the game ACTUALLY received, after our rotated stick has been through
// ToAxis, the game's own deadzone and its input binding. aForward and aStrafe are
// the raw input axes UE2 builds NewAccel from -- both measured and named in
// docs/ENGINE-MAP.md -- so their angle is the movement direction the engine will
// use, expressed relative to whatever rotation it measures from.
//
// THIS IS THE TERM THE PROBE WAS MISSING. Comparing it against the angle we sent
// says in one line whether the transformation survives the trip, instead of
// leaving the whole chain to be inferred from where the player ended up.
static bool ReceivedStickAngle(const void* controller, double* outDeg)
{
    const uint8_t* const c = (const uint8_t*)controller;
    if (!c || !outDeg) return false;
    if (IsBadReadPtr(c + 0x5C0, 4) || IsBadReadPtr(c + 0x5C8, 4)) return false;

    const float fwd = *(const float*)(c + 0x5C0);
    const float str = *(const float*)(c + 0x5C8);
    if (fabs((double)fwd) + fabs((double)str) < 1.0) return false;   // centred

    *outDeg = atan2((double)str, (double)fwd) * (180.0 / 3.14159265358979323846);
    return true;
}

// Silent unless the pawn is actually moving; one line a second.
//
// ⚠ THE FIRST VERSION COULD NOT ANSWER, and it is worth saying why rather than
// quietly widening it. It compared travel against base.yaw alone -- but the
// intended heading is base.yaw + STICK ANGLE, and the stick is not readable from
// CalcView. The limitation was written in the comment and shipped anyway, and the
// result was +-7 degrees of noise at ZERO controller offset, as large as the
// signal. InputHook now publishes the angle it sent, so `intended` is real.
static void WalkDriftProbe(int32_t aimYaw, int32_t baseYaw, float rDeg,
    const void* controller)
{
    if (!g_cfg.walkDriftProbe) return;

    const uint8_t* const p = (const uint8_t*)GameState_Pawn();
    if (!p || IsBadReadPtr(p + 0x1D8, 12)) return;
    const float* const loc = (const float*)(p + 0x1D8);

    static float  s_prev[3] = {};
    static bool   s_have = false;
    static DWORD  s_last = 0;
    static const void* s_pawn = nullptr;

    // A new pawn is a new coordinate history. Reset rather than measure a
    // teleport as travel.
    if (p != s_pawn) { s_pawn = p; s_have = false; s_last = 0; }

    const DWORD now = GetTickCount();
    if (!s_have || now - s_last < 1000)
    {
        if (!s_have) { memcpy(s_prev, loc, sizeof(s_prev)); s_have = true; s_last = now; }
        return;
    }

    const double dx = (double)loc[0] - s_prev[0];
    const double dy = (double)loc[1] - s_prev[1];
    memcpy(s_prev, loc, sizeof(s_prev));
    s_last = now;

    // Below this you are standing still and the heading is noise.
    const double dist = sqrt(dx * dx + dy * dy);
    if (dist < 20.0) return;

    // UE yaw: 0 is +X, increasing toward +Y, 65536 units per turn.
    const double actual = atan2(dy, dx) * (180.0 / 3.14159265358979323846);
    const double baseDeg = (double)(short)baseYaw / 182.0444;
    const double aimDeg = (double)(short)aimYaw / 182.0444;

    int32_t pawnYawRaw = 0;
    const bool havePawnYaw = PawnYaw(&pawnYawRaw);
    const double pawnDeg = (double)(short)pawnYawRaw / 182.0444;

    // THE ANGLE WE SENT, from InputHook, and THE ANGLE THE GAME RECEIVED, from
    // its own raw input axes. If those two disagree the bug is in the trip
    // between them and nothing further downstream needs explaining.
    float sentDeg = 0.0f;
    const bool haveSent = Input_GetSentStickAngle(&sentDeg);

    double recvDeg = 0.0;
    const bool haveRecv = ReceivedStickAngle(controller, &recvDeg);

    double sentVsRecv = recvDeg - (double)sentDeg;
    while (sentVsRecv > 180.0) sentVsRecv -= 360.0;
    while (sentVsRecv < -180.0) sentVsRecv += 360.0;

    // INTENDED is the heading mode 0 promises: the base plus wherever the stick
    // is pushed. Without the stick term this reduces to base, which is only the
    // right answer while walking exactly forward -- the flaw in the first cut.
    double intended = baseDeg + (haveSent ? (double)sentDeg : 0.0);
    while (intended > 180.0) intended -= 360.0;
    while (intended < -180.0) intended += 360.0;

    double err = actual - intended;
    while (err > 180.0) err -= 360.0;
    while (err < -180.0) err += 360.0;

    double aimVsPawn = aimDeg - pawnDeg;
    while (aimVsPawn > 180.0) aimVsPawn -= 360.0;
    while (aimVsPawn < -180.0) aimVsPawn += 360.0;

    Log(">>> WALKDRIFT: moved %.0f  actual %+.1f  intended %+.1f  err %+.1f  |  "
        "sent %+.1f  recv %+.1f  recv-sent %+.1f  |  base %+.1f  aim %+.1f  "
        "aim-pawn %+.1f  R %+.1f",
        dist, actual, intended, err,
        haveSent ? (double)sentDeg : 0.0,
        haveRecv ? recvDeg : 0.0,
        (haveSent && haveRecv) ? sentVsRecv : 0.0,
        baseDeg, aimDeg, havePawnYaw ? aimVsPawn : 0.0, rDeg);
}

static void PublishWalkRotation(const FRotator& viewRot, const FRotator& aimRot,
    const FRotator& base, const void* controller)
{
    // Walking is measured from the aim field -- which is what we write, so this
    // cancels it. The PAWN rotator was tried as an alternative basis and
    // FALSIFIED: it tracks the aim field exactly (see PawnYaw), so substituting
    // it changed nothing and the flag is gone.
    const int32_t fromYaw = aimRot.yaw;

    // Rotator units are 16-bit and wrap, so every difference is taken as a
    // SIGNED SHORT before it becomes degrees. A raw subtraction reads a small
    // turn across the wrap as a full circle.
    double r = 0.0;
    switch (g_cfg.movementMode)
    {
    case 0:  r = (double)(short)(base.yaw - fromYaw);    break;
    case 2:  r = (double)(short)(viewRot.yaw - fromYaw); break;
    case 3:  r = (double)(short)(viewRot.yaw - base.yaw); break;
    default: r = 0.0;                                    break;   // 1
    }

    _InterlockedIncrement(&g_walkSeq);         // odd == writing
    MemoryBarrier();
    g_walkRotDeg = (float)(r / 182.0444);
    MemoryBarrier();
    _InterlockedIncrement(&g_walkSeq);         // even == done
    g_walkOk = 1;

    WalkDriftProbe(aimRot.yaw, base.yaw, (float)(r / 182.0444), controller);
}

bool CameraHook_GetWalkRotation(float* outDeg)
{
    if (!outDeg || !g_walkOk) return false;
    for (int t = 0; t < 8; ++t)
    {
        const long s0 = g_walkSeq;
        if (s0 & 1) continue;
        MemoryBarrier();
        const float v = g_walkRotDeg;
        MemoryBarrier();
        if (g_walkSeq == s0) { *outDeg = v; return true; }
    }
    return false;
}

// ---------------------------------------------------------------- the detour

struct CallSite
{
    void* ret;
    uint64_t count;
    FVector  loc;    // CLEAN, snapshotted before any write
    FRotator rot;
};

static CallSite g_sites[8] = {};
static int      g_siteCount = 0;
static int      g_leader = 0;

static uint64_t g_calls = 0;
static uint64_t g_wLeft = 0, g_wRight = 0;   // writes per eye -- MUST stay ~50/50
static bool     g_armLogged = false;

static DWORD    g_lastTick = 0;

// Absolute pitch the GAME wrote into the aim field since the last heartbeat,
// in rotator units. Sums |delta| so a sweep up and back down cannot cancel out.
static double   g_aimGameDPitch = 0.0;

// S77: the game's own yaw change since our last write, republished every
// CalcView. Under input context NullInput the game DISCARDS stick input, so
// this stays flat while the stick is hard over -- a direct test for "the game
// is ignoring you" that needs no offsets and no cutscene flag.
static int      g_gameDYaw = 0;

// S22: rotator fields are 16-bit-periodic (65536 units == 360 deg) but stored
// in a wider signed field, and the game NORMALISES what we write. Look down and
// we write a negative pitch; the game stores it normalised, and a naive
// (now - then) reads that as a full +360 deg EVERY FRAME. g_aimBase then grows
// by 65536 units/frame -- ~15M/s, which overflows int32 in about 140 seconds.
// Mode 2 discards base pitch so it is invisible there, but it corrupts mode 1
// and the yaw accumulator regardless. Difference on the circle instead.
static inline int RotDelta(int now, int then)
{
    int d = (now - then) & 0xFFFF;
    if (d >= 32768) d -= 65536;
    return d;
}

// ---- HEAD-AIM (§15) -----------------------------------------------------
// MEASURED: the reticle is drawn at backbuffer center (so it appears to follow
// the head) but melee/fire resolve against Controller.Rotation, driven by the
// mouse. The crosshair therefore LIES at any head angle. Head-aim writes that
// field so the gun goes where you look.
//
// THE TRAP: writing the field naively makes the head offset accumulate -- the
// game applies the next mouse delta on top of OUR value and the player spins.
// So we keep our own base, and each frame add only the delta the GAME made
// since our last write.
static FRotator g_aimBase = {};
static FRotator g_aimLastWrote = {};
static bool     g_aimInit = false;
// Has a base ever been established? Only so the arm log can say how far the
// reference jumped -- on the very first arm of a run there is nothing to jump
// from, and printing one would invite reading noise as a finding.
static bool     g_aimInitEver = false;
static int      g_aimCand = 0;      // Numpad + cycles the ROTSCAN candidates
static const unsigned kAimOffsets[2] = { 0x1E4, 0x328 };

// ============================================================================
//  WHAT STATE DOES A SCRIPTED WINDOW OPEN AND CLOSE IN?
//
// THE QUESTION. The balcony fall lands somewhere different every single run.
// The path is straight (StickPrecomp) and the scene rotates correctly
// (ScriptedCameraFollow), so the remaining source of run-to-run variation is
// the state the window OPENS in -- and a forced move steers by
// Controller.Rotation, whose last value is whatever WE wrote on the frame
// before. That is the controller's aim, not the player's body heading.
//
// TWO CANDIDATES, AND THE PROBE SEPARATES THEM. It could be the entry HEADING
// (you were pointing somewhere) or the entry POSITION (you walked up to the
// trigger from a slightly different spot). Both are logged, on both edges of
// every window, so the answer is read rather than argued. Position also makes
// the OUTCOME measurable: the exit of the balcony's second window is the
// landing spot, in world coordinates, instead of "way off" or "almost right".
//
// TWO WINDOWS AT THE BALCONY, 118 ms apart -- the forced move lives entirely in
// the first (362 ms) and the fall plays out in the second (~67 s). No
// special-casing here, or the second one is exactly the one that goes missing.
//
// EMITTED FROM THE AIM BLOCK, NOT FROM THE EDGE. The edge detector runs before
// aimField is resolved, so the interesting value is not available where the
// edge is seen. Latch there, print here, and keep a fallback line for the case
// where the aim block is skipped entirely (head aim off, starved, UI up) --
// otherwise a silent log would look like a window that never happened.
// ============================================================================

// FALSIFIED AND REMOVED, 2026-08-11: substituting a heading into the aim field
// on the window's rising edge. The idea was that a forced move steers by
// whatever we last left in Controller.Rotation. It does not -- under M7-S6,
// which never writes the field during a sequence, three falls entered at wildly
// different controller angles all landed on the SAME spot. With the substitution
// on, both straight-on runs landed badly wrong, because the write itself is the
// damage. Do not re-add a write of any kind inside a scripted window.
static int   g_edgePending = 0;     // 0 none, 1 entry, 2 exit
static DWORD g_edgeLastTick = 0;    // when the OPPOSITE edge fired

static inline double YawDeg(int32_t r)
{
    // Rotators are 16-bit-periodic in a wider field, so the printable angle is
    // the low word read as signed -- otherwise an accumulated base prints as
    // thousands of degrees and two runs cannot be compared by eye.
    return (double)(short)(r & 0xFFFF) / 182.0444;
}

// Declared up here because the edge report below prints the manual total. The
// machinery that maintains them is under the SCRIPTED RECENTRE banner.
static int   g_scriptedManualYaw = 0;    // rotator units, signed
static int   g_scriptedCancelled = 0;    // how much has been spent, for the log

static void ScriptedEdgeReport(const FRotator* aim)
{
    if (!g_edgePending) return;

    const int which = g_edgePending;
    g_edgePending = 0;

    float p[3] = { 0.0f, 0.0f, 0.0f };
    const bool havePos = GameState_GetPawnEyePoint(p);

    const DWORD now = GetTickCount();
    const DWORD since = g_edgeLastTick ? (now - g_edgeLastTick) : 0;
    g_edgeLastTick = now;

    Log(">>> SCRIPTED %s: aim y %+8.2f p %+7.2f %s| base y %+8.2f | head y %+7.2f"
        " | hand y %+7.2f valid %d | pawn %9.1f %9.1f %9.1f ok %d | forced %d"
        " anim %d | manual y %+7.2f | %lu ms since the last edge",
        which == 1 ? "ENTRY" : "EXIT ",
        aim ? YawDeg(aim->yaw) : 0.0,
        aim ? YawDeg(aim->pitch) : 0.0,
        aim ? "" : "(FIELD UNREADABLE) ",
        YawDeg(g_aimBase.yaw),
        g_headYaw,
        g_aimHandYaw, g_aimHandValid ? 1 : 0,
        p[0], p[1], p[2], havePos ? 1 : 0,
        GameState_ForcedMove() ? 1 : 0,
        GameState_ScriptedAnim() ? 1 : 0,
        g_scriptedManualYaw / 182.0444,
        (unsigned long)since);
}

// ============================================================================
//  SCRIPTED RECENTRE -- the scene reaches its OWN framing
//
// THE PROBLEM, reported after the balcony arc closed. The right stick still
// turns you during a scripted sequence (M7-S3, and that is deliberate -- it is
// the comfort option for people who do not want the camera moved for them). But
// the scene's own rotation then lands ON TOP of wherever you turned to, so a
// scene that means to face you at something faces you at something-plus-your-
// offset instead.
//
// THE FIX. Track what the PLAYER added to g_aimBase during the window, then
// spend it back down as the scene rotates. Two sites add player yaw and both are
// already isolated -- the stick-look block below (grep MOD-SIDE SMOOTH YAW) and
// snap turn. Nothing else touches the base from input.
//
//   1  WASH OUT. Each frame the scene turns by |d|, up to |d| of the manual
//      offset is cancelled. A large authored turn lands exactly on the framing;
//      a small one gets partway; a scene that never turns you leaves your offset
//      completely alone. Invisible in motion, because the view is already moving.
//   2  DROP IT. The whole offset goes the first frame the scene turns at all.
//      Exact, but it lands as a visible jerk.
//
// ⚠ THIS IS NOT THE PITCH SERVO (graveyard 4). That one read a value back out of
// the engine and drove toward it, which is a feedback loop, and it froze the
// view. This spends down a quantity WE accumulated ourselves, is clamped to what
// remains, and can only ever reach zero. It never reads engine state and cannot
// overshoot.
//
// ONLY ARTIFICIAL TURNING. The head is never in this accumulator and never
// cancelled -- the view stays 1:1 with the player's neck under every setting.
// Taking head look away to hit a framing is a nausea trigger, not a feature.
//
// It lives with the camera follow because after Build J that block is the single
// source of scripted yaw, so it is the only place the scene's rotation arrives.
// ============================================================================

// g_scriptedManualYaw and g_scriptedCancelled are declared above the edge
// report, which prints the running total.

// Called from the two player-input sites. Silent and free when the setting is
// off or no sequence is running.
static void ScriptedManualYaw(int units, bool scriptedAim)
{
    if (scriptedAim && g_cfg.scriptedRecentre) g_scriptedManualYaw += units;
}

static void ScriptedRecentre(int d)
{
    if (!g_cfg.scriptedRecentre || d == 0 || g_scriptedManualYaw == 0) return;

    int cancel;
    if (g_cfg.scriptedRecentre >= 2)
    {
        cancel = g_scriptedManualYaw;            // all of it, on the first turn
    }
    else
    {
        const int budget = (d < 0) ? -d : d;     // spend |d| of it this frame
        const int have = (g_scriptedManualYaw < 0) ? -g_scriptedManualYaw
            : g_scriptedManualYaw;
        const int take = (budget < have) ? budget : have;
        cancel = (g_scriptedManualYaw < 0) ? -take : take;
    }

    g_aimBase.yaw -= cancel;
    g_scriptedManualYaw -= cancel;
    g_scriptedCancelled += (cancel < 0) ? -cancel : cancel;

    if (g_scriptedManualYaw == 0 && g_scriptedCancelled)
    {
        Log(">>> SCRIPTED: recentred -- %.1f deg of your own turning handed back "
            "to the scene (mode %d)",
            g_scriptedCancelled / 182.0444, g_cfg.scriptedRecentre);
        g_scriptedCancelled = 0;
    }
}

// ---- PAIR LOCK (§14): the two eyes of a pair must be rendered from the SAME
// instant. The head pose was already latched per pair (§6) -- but cleanRot and
// CameraLocation were read FRESH each CalcView, so during a stick turn eye 1
// rendered ~4.2ms of extra yaw (~0.5 deg at 120 deg/s == ~28% disparity error
// at 2m, with a VERTICAL disparity component once the view is pitched). This
// is the standard AER artifact; UEVR's "Synchronized Sequential" exists
// precisely to hold game state constant across the pair.
static FRotator g_pairRot = {};
static FVector  g_pairLoc = {};
static bool     g_pairValid = false;

static const uint64_t kArmAfterCalls = 200;   // let the leader settle before writing

// ---- ROTATION FIELD FINDER (head-aim / motion-control groundwork) --------
// The gun follows Controller.Rotation, a DIFFERENT field from the view rotation
// we write in CalcView -- which is why the weapon slides opposite your head.
// To make aim follow the head we must find that field's offset. Numpad 9 fires
// ONE scan of the PlayerController and logs every FRotator whose yaw matches the
// clean view yaw. Run it facing a landmark, turn 90 deg, run it again: the
// offset whose yaw TRACKS yours across both scans is Controller.Rotation.
static void ScanForRotation(void* pc, const FRotator& clean)
{
    if (!pc) return;
    const unsigned char* base = (const unsigned char*)pc;
    Log("ROTSCAN: view p=%d y=%d r=%d  in PC 0x%08X",
        clean.pitch, clean.yaw, clean.roll, (unsigned)(uintptr_t)pc);

    int hits = 0;
    for (size_t off = 0; off + sizeof(FRotator) <= 0x800; off += 4)
    {
        const unsigned char* p = base + off;
        if (!IsMemoryValid((void*)p, sizeof(FRotator))) continue;

        const FRotator* r = (const FRotator*)p;
        const int dy = abs(r->yaw - clean.yaw);
        if (dy < 364)                       // within ~2 deg (182 units == 1 deg)
        {
            Log("  ROTSCAN +0x%03X  p=%d y=%d r=%d",
                (unsigned)off, r->pitch, r->yaw, r->roll);
            if (++hits >= 16) break;
        }
    }
    if (!hits) Log("  ROTSCAN: nothing in the first 2KB. Widen the scan.");
}

// ---- 6-DOF HANDS (S54) --------------------------------------------------
// The hands actor is at pawn+0x724 with Location +0x1D8 and Rotation +0x1E4,
// all measured. At rest Hands.Location == the camera exactly and Hands.Rotation
// == the view rotator exactly, so this is substitution, not correction.
//
// ABSOLUTE writes. The nudge tests proved the game does NOT rewrite either
// field between our calls -- an incremental write accumulated the yaw into a
// spin and lifted the arms out of the level. So every frame we compute the
// target outright.
//
// Position: the controller pose RELATIVE TO THE HEAD, converted XR->game
// (XR is +x right, +y up, -z forward; game is +X forward, +Y right, +Z up) and
// rotated into the world by the same room yaw the head-position write uses.
// Relative to the head, not to the origin, so recentring and CameraHeightOffset
// come along for free.
//
// Rotation: the controller aim quaternion through the SAME conversion as the
// head, composed onto the same mouse heading. Unclamped -- the clamp exists to
// keep the gun on screen when the VIEW is driven from the aim field, and here
// the two are finally independent.
// MEASURED S59 (readback, ~10s of play):
//   pitch drift 0.0-0.3 deg, yaw drift 0.0-1.2 deg  -> our writes HOLD
//   roll  drift 5-102 deg, scaling with wrist twist -> the game ERASES roll
//
// So roll was never landing. Writing it anyway was actively harmful: the grip
// correction rotated the offset by a roll the mesh never rendered with, which
// swung the hand through an arc that grew with the twist. That was the residual
// drift. We now write only what survives, and correct with the same values.

// ---- S60: LATE ROTATION WRITE -------------------------------------------
// MEASURED S59: pitch and yaw survive our CalcView write; roll was erased every
// frame by 5-102 degrees, scaling with wrist twist. That is the game tick
// running AFTER us and resetting the rotator to the view rotation.
//
// So write again later. Present is the last thing in the frame, well past the
// game tick, so a re-apply there lands after theirs. Published on the game
// thread, consumed on the render thread; a torn read would cost one frame of a
// slightly wrong angle, which is not worth a lock.
static void* g_hwObj = nullptr;
static unsigned g_hwRotOff = 0;
static FRotator g_hwWant = {};
static bool     g_hwValid = false;

void CameraHook_LateHandsWrite()
{
    if (!g_cfg.sixDofHands || !g_hwValid || !g_hwObj) return;
    if (GameState_Theater()) { g_hwValid = false; return; }

    // M7-S2, and this one is NOT optional. DriveHands stops publishing during a
    // scripted animation, but this runs from Present off a CACHED rotation --
    // so without invalidating it we would keep re-applying the last pre-cutscene
    // wrist angle every frame, pinning the hands at a stale rotation for the
    // whole sequence. Clearing the cache is what hands the rig back to the game.
    if (ScriptedAimReleased()) { g_hwValid = false; return; }

    if (ViewHeldForUi()) return;      // render-thread half of the same freeze

    // The hands actor is destroyed on level/save load. This runs on the RENDER
    // thread from a pointer cached on the GAME thread, so a stale cache writes
    // into freed or reallocated memory -- a crash during loading. Re-check the
    // probe's current target every call and drop the cache the moment it moves.
    void* obj = nullptr; unsigned locOff = 0, rotOff = 0;
    if (!HandsProbe_GetTargets(&obj, &locOff, &rotOff) || obj != g_hwObj)
    {
        g_hwValid = false;
        return;
    }

    FRotator* R = (FRotator*)((uint8_t*)g_hwObj + g_hwRotOff);
    if (!IsMemoryWritable(R, sizeof(FRotator))) return;
    *R = g_hwWant;
}

// ---- PHASE 10a: ONE WORLD ADVANCE PER EYE PAIR --------------------------
// RE'd in phase 10. module+0x53D850 is the frame-delta function on the game
// thread: it scales the incoming delta by LevelInfo->TimeDilation, clamps to
// [0, 0.4], stores it at this+0xC8, then calls the virtual advance -- so the
// delta can be intercepted on the way IN.
//
// Option-B carry: pass 0 on the right-eye frame, (delta + carry) on the left.
// The world then advances ONCE per stereo pair instead of once per eye. That
// is the entire cause of the bathysphere doubling -- 4.2ms of world motion
// between the two eye renders, which near geometry turns into unfusable
// disparity. Nothing is discarded, so full speed is preserved.
static const unsigned kDelta_FnOff = 0x53D850;
typedef int(__fastcall* DeltaFn)(void* thisPtr, void* edx, void* arg1, uint32_t deltaBits);
static DeltaFn  g_origDelta = nullptr;
static void* g_deltaFnAddr = nullptr;
static bool     g_deltaFired = false;

// TWO objects receive an advance every frame. Only one is the player world;
// the other ignores TimeDilation and drives UI/streaming. ASLR means we cannot
// use a recorded address -- the right one is found live, every launch.
static volatile uint32_t g_deltaObjA = 0, g_deltaObjB = 0;
static uint32_t g_targetObj = 0;
static bool     g_targetLocked = false;
static float    g_carry = 0.0f;   // carry for the player world
static float    g_carryB = 0.0f;   // carry for the second world

// Phase 10 proved the clamp by watching the STORED delta alternate
// (~0.0085 doubled / ~0.0005 near-zero). Bits only -- positive floats compare
// correctly as uint32, so the hook never has to touch an FP register.
static uint32_t g_fdMinBits = 0xFFFFFFFFu, g_fdMaxBits = 0;

static int __fastcall hkDelta(void* thisPtr, void* edx, void* arg1, uint32_t deltaBits)
{
    if (!g_deltaFired)
    {
        g_deltaFired = true;
        Log(">>> DELTA HOOK FIRED. this=0x%08X thread=%lu",
            (unsigned)(uintptr_t)thisPtr, GetCurrentThreadId());
    }

    const uint32_t self = (uint32_t)(uintptr_t)thisPtr;

    // Track the two delta-receiving objects. A save or level load destroys both
    // and makes new ones. With the old pair still recorded, neither new object
    // matches g_deltaObjB, so BOTH map onto g_carry and share one accumulator
    // -- one world then advances twice per pair. That is the double speed after
    // a load. A third identity means the pairing is stale: reset and re-lock.
    if (self != g_deltaObjA && self != g_deltaObjB)
    {
        if (g_deltaObjA == 0) g_deltaObjA = self;
        else if (g_deltaObjB == 0) g_deltaObjB = self;
        else
        {
            g_deltaObjA = self; g_deltaObjB = 0;
            g_carry = 0.0f; g_carryB = 0.0f;
            g_targetObj = 0; g_targetLocked = false;
            Log(">>> DELTA: world objects changed (0x%08X). Carries reset, re-locking.",
                self);
        }
    }

    uint32_t passDelta = deltaBits;

    if (g_cfg.deltaClamp)
    {
        const bool isTarget = (g_targetLocked && self == g_targetObj);
        // Mode 2: clamp BOTH delta-receiving objects. FrameDelta proves the
        // player world is already freezing, yet the camera still moves 1.2
        // units between eyes -- so whatever carries the bathysphere is being
        // ticked by the other one.
        const bool doClamp = (g_cfg.deltaClamp == 2) ? true : isTarget;

        if (doClamp)
        {
            // Each object needs its OWN carry. Sharing one would hand each
            // world the other's frozen time and desync them both.
            float* carry = (self == g_deltaObjB) ? &g_carryB : &g_carry;
            const float d = *(const float*)&deltaBits;
            if ((int)(g_eyeWr & 1) == 1)
            {
                *carry += d;                       // right-eye frame: freeze
                // Safety: during a load the eye tag stops alternating and the
                // carry can bank far more than a frame. Dumping that in one go
                // is a lurch, so discard anything implausible.
                if (*carry > 0.1f) *carry = 0.0f;
                const float zero = 0.0f;
                passDelta = *(const uint32_t*)&zero;
            }
            else
            {
                const float carried = d + *carry;  // left-eye frame: advance the pair
                *carry = 0.0f;
                passDelta = *(const uint32_t*)&carried;
            }
        }
    }

    // Nothing after the call. Your phase-10 notes were careful to keep the
    // post-call section integer-only so a possible EAX/xmm0 return was never
    // clobbered; leaving it empty removes the hazard entirely.
    const int ret = g_origDelta(thisPtr, edx, arg1, passDelta);

    // Integer-only readback, after the call. this+0xC8 is FrameDelta: the
    // scaled, clamped value the engine actually stored and advanced on.
    if (g_targetLocked && self == g_targetObj &&
        IsMemoryValid((const char*)thisPtr + 0xC8, 4))
    {
        const uint32_t stored = *(const uint32_t*)((const char*)thisPtr + 0xC8);
        if (stored < g_fdMinBits) g_fdMinBits = stored;
        if (stored > g_fdMaxBits) g_fdMaxBits = stored;
    }

    return ret;
}

// ---- S5: THE OFFSET IS A ROTATION, NOT EULER ARITHMETIC ------------------
// Adding constants to Euler angles is a shear. Near +-90 deg of controller
// pitch the yaw and roll terms go degenerate and a small movement whips the
// model across the screen. Build the offset as a quaternion in the CONTROLLER'S
// LOCAL frame, then do ONE Euler conversion.
//
// Axis map derived against HeadQuatToDeg (UE_x = -XR_z, UE_y = XR_x,
// UE_z = XR_y):  UE pitch = +rot about XR +X
//                UE yaw   = -rot about XR +Y
//                UE roll  = +rot about XR -Z
static void QuatMul(const float a[4], const float b[4], float out[4])
{
    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
}

static void QuatAxisAngle(float x, float y, float z, double deg, float out[4])
{
    const double h = deg * (3.14159265358979323846 / 180.0) * 0.5;
    const double s = sin(h);
    out[0] = (float)(x * s); out[1] = (float)(y * s);
    out[2] = (float)(z * s); out[3] = (float)cos(h);
}

static void HandsOffsetQuat(const float pyr[3], float out[4])
{
    float qy[4], qp[4], qr[4], t[4];
    QuatAxisAngle(0.f, 1.f, 0.f, -(double)pyr[1], qy);   // UE yaw
    QuatAxisAngle(1.f, 0.f, 0.f, (double)pyr[0], qp);   // UE pitch
    QuatAxisAngle(0.f, 0.f, -1.f, (double)pyr[2], qr);   // UE roll
    QuatMul(qy, qp, t);
    QuatMul(t, qr, out);
}

// Exported so the crosshair uses the SAME maths as the model offset.
void CameraHook_OffsetQuat(const float in[4], const float pyr[3], float out[4])
{
    float qOff[4];
    HandsOffsetQuat(pyr, qOff);
    QuatMul(in, qOff, out);
}

// ---- QUEST ARROW ---------------------------------------------------------
// MEASURED: the arrow is an actor at pawn+0xAE4, found 22 cm directly above the
// gun by the proximity probe. Attached to the weapon, which is why it rode the
// gun around.
//
// Writing its Location every frame parks it relative to the CAMERA instead.
// Rotation is deliberately left alone so it keeps pointing at the objective.
// This replaces the ArrowCounts viewport hack outright -- that could only add a
// constant screen offset to something still tracking the weapon, and its texture
// match was loose enough to displace world geometry for a frame at a time.
static void DriveQuestArrow(const FVector& camLoc)
{
    if (!g_cfg.arrowPtrOff) return;

    void* const pawn = HandsProbe_GetPawn();
    if (!pawn) return;
    if (!IsMemoryValid((const uint8_t*)pawn + g_cfg.arrowPtrOff, 4)) return;

    void* const arrow = *(void**)((uint8_t*)pawn + g_cfg.arrowPtrOff);
    if (!arrow) return;
    if (!IsMemoryWritable((uint8_t*)arrow + 0x1D8, sizeof(FVector))) return;

    // Room frame, same yaw basis the head-position write uses, so "forward"
    // means where the body faces rather than where the eyes happen to point.
    const double yaw = UnitsToRad(
        (g_cfg.headAim && g_aimInit) ? g_aimBase.yaw : g_lastCleanYaw);
    const double cs = cos(yaw), sn = sin(yaw);

    const double f = g_cfg.arrowWorld[0];
    const double r = g_cfg.arrowWorld[1];
    const double u = g_cfg.arrowWorld[2];

    FVector* const L = (FVector*)((uint8_t*)arrow + 0x1D8);

    // ---- OUT OF THE WAY DURING A SCRIPTED SCENE ---------------------------
    // Requested for immersion: a quest marker floating in front of your face
    // through a cutscene is exactly the kind of thing that breaks one. The same
    // held window the aim suppression uses, so it covers a scene's forced move
    // and its animation as one unit and cannot flicker between the two.
    //
    // Parked far below the world rather than hidden: we already own this actor's
    // Location every frame, so this needs no second mechanism and no bHidden
    // hunt, and it restores itself the moment the window closes.
    if (g_cfg.arrowHideScripted && GameState_ScriptedWindow())
    {
        L->x = (float)camLoc.x;
        L->y = (float)camLoc.y;
        L->z = -1.0e6f;
        return;
    }

    L->x = (float)(camLoc.x + (f * cs - r * sn));
    L->y = (float)(camLoc.y + (f * sn + r * cs));
    L->z = (float)(camLoc.z + u);

    static bool announced = false;
    if (!announced)
    {
        announced = true;
        Log(">>> ARROW: driving pawn+0x%X -> 0x%08X",
            (unsigned)g_cfg.arrowPtrOff, (unsigned)(uintptr_t)arrow);
    }

    // ---- WHY DOES IT POINT WHERE THE GUN POINTS? (read-only) --------------
    // MEASURED IN A HEADSET 2026-08-12: the Location write above works -- the
    // arrow now sits where we put it and no longer bobs -- but its ROTATION
    // still follows the right hand. The comment above assumed leaving rotation
    // alone would keep it aimed at the objective; that assumption is now
    // falsified, so the next step needs numbers rather than another assumption.
    //
    // Actor layout: Location +0x1D8, so Rotation is +0x1E4 (FVector is 12
    // bytes), the same pairing Controller.Rotation uses. Printed beside the
    // WEAPON actor's rotation, because the whole question is whether the arrow's
    // own field already carries the gun's heading (the game composes it and we
    // can overwrite) or is independent of it (something downstream composes at
    // render time, and overwriting will not help).
    //
    if (!IsMemoryWritable((uint8_t*)arrow + 0x1E4, sizeof(FRotator))) return;
    FRotator* const R = (FRotator*)((uint8_t*)arrow + 0x1E4);

    // The weapon actor, so the two rotations can be compared and, if the
    // composition turns out to be additive, cancelled.
    FRotator gun = {};
    bool haveGun = false;
    {
        void* const hands = HandsProbe_Get();
        if (hands && IsMemoryValid((const uint8_t*)hands + 0x45C, 4))
        {
            void* const wpn = *(void* const*)((const uint8_t*)hands + 0x45C);
            if (wpn && IsMemoryValid((const uint8_t*)wpn + 0x1E4, sizeof(FRotator)))
            {
                gun = *(const FRotator*)((const uint8_t*)wpn + 0x1E4);
                haveGun = true;
            }
        }
    }

    // ---- THE CANCELLATION WAS WRONG, AND THE LOG SAYS HOW ----------------
    // FALSIFIED 2026-08-12, first run. Subtracting the gun's rotation each frame
    // assumed the GAME rewrites this field every tick, so that each subtraction
    // applies to a fresh value. It does not. The subtraction COMPOUNDED, and the
    // probe caught it walking away within seconds:
    //
    //   arrow p -1.1 ... p +44.6 ... p +151.1 ... p -103.3
    //
    // A cumulative correction to a field nobody resets is a runaway, and this
    // one is the same shape as the pitch servo in the graveyard. Now DEFAULT
    // OFF, so the probe below reports the game's own values instead of ours.
    //
    // WHAT THE NEXT RUN DECIDES: with the write off, does the arrow's rotation
    // CHANGE as the objective moves relative to the player? If yes, the field is
    // the authored direction and the gun is composed downstream at render time
    // -- which no write here can undo, and detaching the actor (its Base) is the
    // real fix. If it sits still, the field is not what aims the arrow at all.
    if (g_cfg.arrowUnparentRot && haveGun)
    {
        R->pitch -= gun.pitch;
        R->yaw -= gun.yaw;
    }

    // ---- IT WAS THE ROLL, AND THE YAW WAS NEVER WRONG --------------------
    // MEASURED 2026-08-12 with every write off, player standing still:
    //
    //   arrow y  -90.2  -89.1  -89.3  -89.4  -88.9   <- parked on the objective
    //   gun   y +153.8 +159.1 +159.2 +163.1          <- swinging freely
    //   arrow r  +19.8  +17.3  +14.7  +14.8   +3.3   <- following the gun
    //
    // So "it points where the gun points" was never a heading error. The yaw is
    // the authored objective direction and it holds; what tracks the weapon is
    // ROLL, tilting the arrow by up to 20 degrees, which reads as the thing
    // fighting you. The attachment scan agrees there is nothing to detach --
    // arrow+0x0AC holds the PAWN and no slot in its first 0x400 bytes points at
    // the weapon.
    //
    // An arrow has no business carrying roll, so this zeroes it. ABSOLUTE, not
    // cumulative -- that distinction is the entire lesson of the failed
    // cancellation above, and writing a constant is idempotent by construction.
    if (g_cfg.arrowLevel)
    {
        R->roll = 0;
        if (g_cfg.arrowLevel >= 2) R->pitch = 0;   // also pin it horizontal
    }

    // ---- SIZE, and it uses the mechanism that already works on the gun ----
    // Actor DrawScale, the same +0x2AC HandsProbe writes for GunScale. Apparent
    // size is mostly distance for a world actor, but the arrow's mesh is small
    // enough that distance alone could not get there.
    if (g_cfg.arrowDrawScale > 0.0f)
    {
        float* const S = (float*)((uint8_t*)arrow + 0x2AC);
        if (IsMemoryWritable(S, 4) && *S != g_cfg.arrowDrawScale)
            *S = g_cfg.arrowDrawScale;
    }

    // ---- WHAT IS THIS ACTOR ATTACHED TO? (one shot, read-only) -----------
    // "Fully decoupled instead of just canceled out" is the right instinct and
    // arithmetic cannot get there: if the renderer composes a parent transform,
    // no value written into this actor's own rotation survives it. The parent
    // pointer is what has to change.
    //
    // Rather than guess where AActor::Base lives, SEARCH FOR ITS VALUE. Walk the
    // arrow's head looking for any slot holding a pointer we can already name --
    // the weapon actor, the hands actor, the pawn. A hit identifies the
    // attachment field outright, the same way the self-referential back-reference
    // pinned myHUD. Runs once, prints, and changes nothing.
    if (g_cfg.arrowProbe)
    {
        static bool s_scanned = false;
        if (!s_scanned && haveGun)
        {
            s_scanned = true;
            void* const hands = HandsProbe_Get();
            void* const wpn = (hands && IsMemoryValid((const uint8_t*)hands + 0x45C, 4))
                ? *(void* const*)((const uint8_t*)hands + 0x45C) : nullptr;

            int hits = 0;
            for (unsigned off = 0; off < 0x400; off += 4)
            {
                if (!IsMemoryValid((const uint8_t*)arrow + off, 4)) continue;
                void* const v = *(void* const*)((const uint8_t*)arrow + off);
                if (!v) continue;
                const char* what = (v == wpn) ? "the WEAPON actor"
                    : (v == hands) ? "the HANDS actor"
                    : (v == pawn) ? "the pawn" : nullptr;
                if (!what) continue;
                ++hits;
                Log(">>> ARROWBASE: arrow+0x%03X -> %s (0x%08X)",
                    off, what, (unsigned)(uintptr_t)v);
            }
            if (!hits)
                Log(">>> ARROWBASE: no slot in the first 0x400 bytes points at "
                    "the weapon, hands or pawn -- it is not parented to any of "
                    "them, so the gun-following is coming from somewhere else.");
        }
    }

    // Throttled to once a second, read-only, and it gates nothing.
    if (g_cfg.arrowProbe)
    {
        static DWORD s_last = 0;
        const DWORD now = GetTickCount();
        if (now - s_last >= 1000)
        {
            s_last = now;
            Log(">>> ARROWROT: arrow p%+7.1f y%+7.1f r%+7.1f | gun %sp%+7.1f "
                "y%+7.1f | arrow-gun y%+7.1f | camYaw%+7.1f | cancel %d",
                UnitsToDeg(R->pitch), UnitsToDeg(R->yaw), UnitsToDeg(R->roll),
                haveGun ? "" : "(none) ",
                UnitsToDeg(gun.pitch), UnitsToDeg(gun.yaw),
                haveGun ? UnitsToDeg(R->yaw) - UnitsToDeg(gun.yaw) : 0.0,
                UnitsToDeg((int32_t)((g_cfg.headAim && g_aimInit)
                    ? g_aimBase.yaw : g_lastCleanYaw)),
                g_cfg.arrowUnparentRot ? 1 : 0);
        }
    }
}

// ---- GUN DISTANCE WATCH (diagnostic, read only) --------------------------
// The gun changing SIZE with turn direction is either a PROJECTION error (the
// weapon is where it should be, drawn at the wrong scale) or a PLACEMENT error
// (it is genuinely moving toward and away from the camera). At 40 cm those look
// the same through a headset. The distance does not lie.
static void WatchGunDistance(const FVector& camLoc, const void* handsObj)
{
    if (!handsObj) return;
    if (!IsMemoryValid((const uint8_t*)handsObj + 0x45C, 4)) return;

    const uint8_t* const gun = *(const uint8_t* const*)((const uint8_t*)handsObj + 0x45C);
    if (!gun || !IsMemoryValid(gun + 0x1D8, 12)) return;

    const float* const g = (const float*)(gun + 0x1D8);
    const double dx = (double)g[0] - camLoc.x;
    const double dy = (double)g[1] - camLoc.y;
    const double dz = (double)g[2] - camLoc.z;
    const double d = sqrt(dx * dx + dy * dy + dz * dz);

    static double lo = 1e9, hi = -1e9, sum = 0.0;
    static int    n = 0, yawSum = 0;
    static DWORD  last = 0;

    if (d < lo) lo = d;
    if (d > hi) hi = d;
    sum += d; ++n;
    yawSum += g_gameDYaw;

    const DWORD t = GetTickCount();
    if (!last) { last = t; return; }
    if (t - last < 1000) return;
    last = t;

    Log("  GUNDIST: avg %6.1f cm   min %6.1f  max %6.1f  spread %5.1f   turn %+7.0f deg/s",
        n ? sum / n : 0.0, lo, hi, hi - lo, yawSum / 182.0444);

    lo = 1e9; hi = -1e9; sum = 0.0; n = 0; yawSum = 0;
}

// ===========================================================================
//  M6-S1: THE TRACKED FREE HAND
//
// Places the free hand's bones where its own controller is, while the actor --
// and with it the weapon and the working hand -- carries on exactly as before.
// That separation is the whole feature: two hands, one actor.
//
// WHICH HAND IS FREE DEPENDS ON WHAT YOU ARE HOLDING. With a weapon the actor
// is posed from the right controller and the LEFT hand is free. With a plasmid
// the game puts it in your left hand and the actor follows the left controller,
// so the RIGHT hand is the free one. Same write either way.
//
// It applies to precisely the slots that HIDE the free hand today, because those
// are the ones where it has nothing to do. The two-handed weapons keep both
// hands on the gun and are never touched. See the ARMS block for the swap.
//
// MEASURED, M6-S1, and the reason this can be a few lines rather than a hunt:
// the bone array is a common model space in centimetres, and the left cluster
// is not animated at all -- so a rigid transform on it fights nothing.
//
// THE ONE PREDICTION LEFT IN HERE is which model lane is which. The rest pose
// reads as (forward, right, up) -- the actor's own axes -- but that is inferred
// from anatomy, not measured. So it is LeftHandAxisMap in the ini rather than a
// constant in this file, and LeftHandTracked=3 measures it outright.
// ===========================================================================

// Set by the ARMS block, consumed here. Both run on the game thread inside the
// same CalcView, the ARMS block first, so this is sequencing rather than
// sharing -- the decision and its use cannot disagree within a frame.
static bool g_leftTrackOn = false;
static int  g_freeHand = HAND_LEFT;

// A rotation matrix straight to a quaternion. Deliberately NOT built out of
// HandsOffsetQuat: that composes in the XR axis convention, and these bones are
// mesh space. Going through the basis keeps one convention end to end.
static void BasisToQuat(const Basis& b, float out[4])
{
    // Columns are the rotated axes, expressed in the frame we want the result in.
    const double m00 = b.forward.x, m01 = b.right.x, m02 = b.up.x;
    const double m10 = b.forward.y, m11 = b.right.y, m12 = b.up.y;
    const double m20 = b.forward.z, m21 = b.right.z, m22 = b.up.z;

    const double tr = m00 + m11 + m22;
    double x, y, z, w;
    if (tr > 0.0)
    {
        double s = sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        double s = sqrt(1.0 + m00 - m11 - m22) * 2.0;
        w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        double s = sqrt(1.0 + m11 - m00 - m22) * 2.0;
        w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s;
    }
    else
    {
        double s = sqrt(1.0 + m22 - m00 - m11) * 2.0;
        w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s;
    }
    out[0] = (float)x; out[1] = (float)y; out[2] = (float)z; out[3] = (float)w;
}

// Express a world vector in a basis's own axes -- the same projection
// PublishShotDir uses to turn a world shot direction into a view-relative one.
static Vec3 IntoBasis(const Basis& b, double x, double y, double z)
{
    return { x * b.forward.x + y * b.forward.y + z * b.forward.z,
             x * b.right.x + y * b.right.y + z * b.right.z,
             x * b.up.x + y * b.up.y + z * b.up.z };
}

static void DriveFreeHand(void* handsActor, int hand, const FRotator& want,
    double ax, double ay, double az,
    const FVector& camLoc, const float headPos[3], double cs, double sn)
{
    HandPose lp = {};
    if (!Input_GetHandPose(hand, &lp)) return;
    if (!lp.aimValid && !lp.gripValid) return;

    // Mirror images, so a value tuned for one hand is wrong for the other.
    const float* const offCfg = (hand == HAND_RIGHT)
        ? g_cfg.rightHandOffset : g_cfg.leftHandOffset;
    const float* const rotCfg = (hand == HAND_RIGHT)
        ? g_cfg.rightHandRot : g_cfg.leftHandRot;

    const Basis A = RotatorToBasis(want);

    // ---- rotation FIRST, because the offset below is expressed in its frame --
    // UNVALIDATED against the bone quaternions' handedness and component order,
    // which nothing has measured. Position does not depend on any of that.
    //
    // Only for the identity axis map. A lane permutation is a change of frame,
    // and applying it to a quaternion is not permuting its components -- so
    // rather than get that subtly wrong, it declines.
    float quat[4];
    const float* quatPtr = nullptr;
    Basis T = {};
    bool haveT = false;

    if (g_cfg.offHandTracked == 2)
    {
        const bool identity = g_cfg.leftHandAxis[0] == 1 &&
            g_cfg.leftHandAxis[1] == 2 && g_cfg.leftHandAxis[2] == 3;
        if (identity)
        {
            // GRIP, not aim, and matching the position below -- which takes its
            // point from the grip pose too. The aim pose is where a weapon would
            // shoot; on most controllers it is tilted tens of degrees off the
            // hand. A bare hand wants the pose that describes the hand.
            float qOff[4], qFinal[4];
            HandsOffsetQuat(rotCfg, qOff);
            QuatMul(lp.gripValid ? lp.gripQuat : lp.aimQuat, qOff, qFinal);

            double cp, cy, cr;
            HeadQuatToDeg(qFinal, cp, cy, cr);

            FRotator lwant = ComposeHeadLocal(g_aimBase, cy, cp, g_cfg.headAimMode >= 2);
            lwant.roll = g_aimBase.roll + (int32_t)(cr * 182.0444);

            T = RotatorToBasis(lwant);
            haveT = true;

            // The target frame expressed in the ACTOR's frame -- which, with the
            // identity map, is the model frame the bones are written in.
            Basis rel;
            rel.forward = IntoBasis(A, T.forward.x, T.forward.y, T.forward.z);
            rel.right = IntoBasis(A, T.right.x, T.right.y, T.right.z);
            rel.up = IntoBasis(A, T.up.x, T.up.y, T.up.z);

            BasisToQuat(rel, quat);
            quatPtr = quat;
        }
        else
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                Log("!!! FREEHAND: OffHandTracked=2 needs the identity LeftHandAxisMap.");
                Log("!!! FREEHAND: Falling back to position only.");
            }
        }
    }

    // ---- position: same conversion as the right hand above, deliberately ----
    const float* P = lp.gripValid ? lp.gripPos : lp.aimPos;
    const double relRight = ((double)P[0] - headPos[0]) * 100.0;
    const double relUp = ((double)P[1] - headPos[1]) * 100.0;
    const double relFwd = -((double)P[2] - headPos[2]) * 100.0;

    double lx = camLoc.x + (relFwd * cs - relRight * sn);
    double ly = camLoc.y + (relFwd * sn + relRight * cs);
    double lz = camLoc.z + relUp;

    // ---- THE OFFSET MUST NOT LIVE IN THE ACTOR'S FRAME ------------------
    // REPORTED, first tuned session: with the right hand still the left hand
    // tracked almost perfectly, but MOVING the right hand dragged the left one
    // about. The cause is here and it is exact: this offset used to be added in
    // actor-local space, and the actor is rotated by the RIGHT controller. A
    // tuned 8 cm correction therefore swung through 16 cm as the right wrist
    // turned. Everything else in this function cancels the actor out
    // algebraically -- world = actorLoc + A * A^T * (P - actorLoc) = P -- so the
    // offset was the only term that could couple the two hands, and it did.
    //
    // It belongs in the frame it describes: the offset from the controller's
    // grip point to the wrist bone is fixed relative to YOUR HAND. In rotation
    // mode that frame is known exactly. In position mode there is no hand
    // orientation, so fall back to the player's heading -- which the right hand
    // does not turn either.
    const double o0 = offCfg[0];
    const double o1 = offCfg[1];
    const double o2 = offCfg[2];
    if (o0 || o1 || o2)
    {
        const Basis O = haveT ? T
            : Basis{ { cs, sn, 0.0 }, { -sn, cs, 0.0 }, { 0.0, 0.0, 1.0 } };
        lx += O.forward.x * o0 + O.right.x * o1 + O.up.x * o2;
        ly += O.forward.y * o0 + O.right.y * o1 + O.up.y * o2;
        lz += O.forward.z * o0 + O.right.z * o1 + O.up.z * o2;
    }

    // World offset from the actor origin (which sits at the eye), then into the
    // actor's own axes. The bones live in the actor's frame, not the world's.
    const Vec3 local = IntoBasis(A, lx - ax, ly - ay, lz - az);

    // DrawScale scales the whole mesh, skeleton included, so a bone moved by N
    // model units renders as N * scale centimetres. Divide to ask for a real
    // distance. HandsScale 0 means "leave DrawScale alone", i.e. 1.
    const double s = (g_cfg.handsScale > 0.01f) ? (double)g_cfg.handsScale : 1.0;
    const double al[3] = { local.x / s, local.y / s, local.z / s };

    // Actor axes -> model lanes, per the ini. 1 fwd, 2 right, 3 up, signed.
    float target[3];
    for (int i = 0; i < 3; ++i)
    {
        const int sel = g_cfg.leftHandAxis[i];
        const int a = (sel < 0 ? -sel : sel) - 1;
        target[i] = (float)((sel < 0) ? -al[a] : al[a]);
    }

    ArmHide_DriveFreeHand(handsActor, hand, target, quatPtr);

    static DWORD lastLog = 0;
    const DWORD now = GetTickCount();
    if (now - lastLog >= 2000)
    {
        lastLog = now;
        Log(">>> FREEHAND: %s  model %+7.1f %+7.1f %+7.1f   (actor-local %+6.1f fwd "
            "%+6.1f right %+6.1f up cm)",
            hand == HAND_RIGHT ? "right" : "left ",
            target[0], target[1], target[2], local.x, local.y, local.z);
    }
}

static void DriveHands(const FVector& camLoc, const float headPos[3])
{
    if (!g_cfg.sixDofHands) return;
    if (GameState_Cutscene()) return;
    if (ViewHeldForUi()) return;      // hands hold still behind any UI panel

    // M7-S2: the game is animating the hands on purpose. Let it. Moving the
    // controllers during a scripted moment dragged the hand models wherever you
    // pointed, which is the immersion break this exists to fix.
    if (ScriptedQol()) return;

    void* obj = nullptr; unsigned locOff = 0, rotOff = 0;
    if (!HandsProbe_GetTargets(&obj, &locOff, &rotOff)) return;

    HandPose hp = {};
    const int poseHand = HandsProbe_AbilityMode() ? HAND_LEFT : HAND_RIGHT;
    if (!Input_GetHandPose(poseHand, &hp)) return;
    if (!hp.aimValid && !hp.gripValid) return;

    if (!IsMemoryWritable((uint8_t*)obj + locOff, sizeof(FVector))) return;
    if (!IsMemoryWritable((uint8_t*)obj + rotOff, sizeof(FRotator))) return;

    // ---- rotation: yaw and pitch only ------------------------------------
    float qOff[4], qFinal[4];
    HandsOffsetQuat(g_cfg.handsRot, qOff);
    QuatMul(hp.aimQuat, qOff, qFinal);

    double cp, cy, cr;
    HeadQuatToDeg(qFinal, cp, cy, cr);

    FRotator want = ComposeHeadLocal(g_aimBase, cy, cp, g_cfg.headAimMode >= 2);
    // Roll restored. The game tick erases it, so CameraHook_LateHandsWrite
    // re-applies the whole rotator from Present, after they are done.
    want.roll = g_aimBase.roll + (int32_t)(cr * 182.0444);

    // Readback: keep watching, so a future change that breaks pitch/yaw shows up
    // immediately instead of being tuned around.
    {
        static FRotator lastWrote = {};
        static bool  haveLast = false;
        static DWORD lastLog = 0;

        if (haveLast)
        {
            const FRotator now = *(const FRotator*)((const uint8_t*)obj + rotOff);
            const double dP = RotDelta(now.pitch, lastWrote.pitch) / 182.0444;
            const double dY = RotDelta(now.yaw, lastWrote.yaw) / 182.0444;
            const double dR = RotDelta(now.roll, lastWrote.roll) / 182.0444;

            const DWORD t = GetTickCount();
            if (t - lastLog >= 1000)
            {
                lastLog = t;
                Log(">>> 6DOF readback: p=%.1f y=%.1f r=%.1f deg since our write%s",
                    dP, dY, dR,
                    (fabs(dY) > 1.0 || fabs(dP) > 1.0)
                    ? "   <-- PITCH/YAW BEING OVERWRITTEN" : "   (pitch/yaw held)");
            }
        }
        lastWrote = want;
        haveLast = true;
    }

    *(FRotator*)((uint8_t*)obj + rotOff) = want;
    g_hwObj = obj; g_hwRotOff = rotOff; g_hwWant = want; g_hwValid = true;

    // ---- position: from the GRIP pose -----------------------------------
    const float* P = hp.gripValid ? hp.gripPos : hp.aimPos;

    const double relRight = ((double)P[0] - headPos[0]) * 100.0;
    const double relUp = ((double)P[1] - headPos[1]) * 100.0;
    const double relFwd = -((double)P[2] - headPos[2]) * 100.0;

    const double roomYaw = UnitsToRad(
        (g_cfg.headAim && g_aimInit) ? g_aimBase.yaw : g_lastCleanYaw);
    const double cs = cos(roomYaw), sn = sin(roomYaw);

    double wx = camLoc.x + (relFwd * cs - relRight * sn);
    double wy = camLoc.y + (relFwd * sn + relRight * cs);
    double wz = camLoc.z + relUp;

    // The Hands actor origin sits at the EYE (PlayerViewOffset is 0,0,0), with
    // the arm authored extending forward and down from there. Subtract where the
    // hand sits in mesh space, rotated by the orientation that will ACTUALLY be
    // rendered -- which is `want`, now that we no longer ask for a roll the game
    // refuses to keep.
    if (g_cfg.handsGrip[0] || g_cfg.handsGrip[1] || g_cfg.handsGrip[2])
    {
        const double gp = UnitsToRad(want.pitch);
        const double gy = UnitsToRad(want.yaw);
        const double gr = UnitsToRad(want.roll);

        const double CP = cos(gp), SP = sin(gp);
        const double CY = cos(gy), SY = sin(gy);
        const double CR = cos(gr), SR = sin(gr);

        const double Fx = CP * CY, Fy = CP * SY, Fz = SP;
        const double Rx = SR * SP * CY - CR * SY, Ry = SR * SP * SY + CR * CY, Rz = -SR * CP;
        const double Ux = -(CR * SP * CY + SR * SY), Uy = CY * SR - CR * SP * SY, Uz = CR * CP;

        const double gX = g_cfg.handsGrip[0];
        const double gY = g_cfg.handsGrip[1];
        const double gZ = g_cfg.handsGrip[2];

        wx -= (Fx * gX + Rx * gY + Ux * gZ);
        wy -= (Fy * gX + Ry * gY + Uy * gZ);
        wz -= (Fz * gX + Rz * gY + Uz * gZ);
    }

    FVector* L = (FVector*)((uint8_t*)obj + locOff);
    L->x = (float)wx;
    L->y = (float)wy;
    L->z = (float)wz;

    WatchGunDistance(camLoc, obj);

    // ---- M6-S1: THE LEFT HAND, IN THE FRAME THE RIGHT HAND DEFINES -------
    // Deliberately LAST, because it consumes `want` and wx/wy/wz -- the actor
    // rotation and location this function has just decided. "Decoupled from the
    // right hand" is exactly that: the actor keeps carrying the weapon hand, and
    // the left cluster is then placed relative to it.
    //
    // The controller pose goes through the SAME room-yaw conversion as the right
    // hand above rather than a second one of its own. One algebra, no drift
    // between them.
    if (g_leftTrackOn && (g_cfg.offHandTracked == 1 || g_cfg.offHandTracked == 2))
        DriveFreeHand(obj, g_freeHand, want, wx, wy, wz, camLoc, headPos, cs, sn);

    static bool announced = false;
    if (!announced)
    {
        announced = true;
        Log(">>> 6DOF: %s pose, %+.0f fwd %+.0f right %+.0f up (cm) from the head",
            hp.gripValid ? "GRIP" : "aim", relFwd, relRight, relUp);
    }
}

static void __fastcall hkCalcView(void* pThis, void* edx,
    void** ViewActor,
    FVector* CameraLocation,
    FRotator* CameraRotation)
{
    // 1. Call the ORIGINAL first. It rebuilds *CameraRotation from scratch every
    //    call (§6c), so what arrives here is always CLEAN. Absolute offset, no
    //    accumulator, no drift.
    g_orig(pThis, edx, ViewActor, CameraLocation, CameraRotation);

    // GameState_Observe / HandsProbe_Observe MOVED below the site filter.
    // They must not run on non-leader call sites -- see the note down there.

    void* ret = _ReturnAddress();

    if (g_calls == 0)
    {
        Log(">>> CAMERA HOOK FIRED. First call.");
        Log("camera:   pThis  = 0x%08X  (APlayerController*)", (unsigned)(uintptr_t)pThis);
        Log("camera:   thread = %lu   (GAME thread -- Present is on a DIFFERENT one)",
            GetCurrentThreadId());
        Log("camera:   write  = %s", g_cfg.cameraWrite ? "ENABLED (EnableCameraWrite=1)"
            : "disabled (EnableCameraWrite=0)");
        g_lastTick = GetTickCount();
    }
    ++g_calls;

    if (!CameraLocation || !CameraRotation) return;
    if (!IsMemoryValid(CameraLocation, sizeof(FVector)))  return;
    if (!IsMemoryValid(CameraRotation, sizeof(FRotator))) return;

    // --- bucket by return address (§6c-2) ---
    CallSite* site = nullptr;
    for (int i = 0; i < g_siteCount; ++i)
        if (g_sites[i].ret == ret) { site = &g_sites[i]; break; }

    if (!site && g_siteCount < 8)
    {
        site = &g_sites[g_siteCount++];
        site->ret = ret;
        site->count = 0;
        Log("camera: NEW CALL SITE #%d  ret 0x%08X  (module+0x%X)",
            g_siteCount - 1, (unsigned)(uintptr_t)ret,
            (unsigned)((uint8_t*)ret - g_modBase));
    }
    if (!site) return;

    ++site->count;
    site->loc = *CameraLocation;     // CLEAN snapshot, before we touch anything
    site->rot = *CameraRotation;

    // --- site0 = the site with the most calls. THE RENDER VIEW: the only one
    //     that keeps ticking while the player stands still. Auto-detected. ---
    int leader = 0;
    for (int i = 1; i < g_siteCount; ++i)
        if (g_sites[i].count > g_sites[leader].count) leader = i;
    g_leader = leader;

    // --- site0 ONLY. Sites 2/3/4 are movement/physics/AI CONSUMING the view;
    //     writing to them would let head-look steer the character. ---
    if (site != &g_sites[leader]) return;

    // LEADER ONLY. MEASURED: the engine calls CalcView 4.2x per frame (site0,
    // site2, site3, site5 each tick once). Above the filter these two cost
    // 200-257 ms/s against 4 ms/s for the engine's own CalcView -- ~22% of the
    // frame, three quarters of it on views we discard one line earlier.
    // It also fixes a real bug: non-leader sites carry a DIFFERENT rotator
    // (site1 read yaw -176 while site0 read +160), so HandsProbe's rotation
    // match was being handed whichever site happened to call last.
    GameState_Observe(pThis);
    HandsProbe_Observe(pThis, (const float*)CameraLocation, (const int*)CameraRotation);

    // VIEWACTOR PROBE. The engine hands us the actor the view belongs to on
    // EVERY call, and we have been ignoring it while inferring cutscenes from
    // injected pitch -- which measurably false-fires in combat.
    //
    // NOT the same as the dead +0x450 / +0x620 / +0x914 scan: those were cached
    // controller fields that always equalled the pawn. This is the live
    // out-parameter of the call we are already inside.
    {
        void* const pawn = GameState_Pawn();
        void* const actor = (ViewActor && IsMemoryValid(ViewActor, sizeof(void*)))
            ? *ViewActor : nullptr;

        void* actorVt = nullptr, * pawnVt = nullptr;
        if (actor && IsMemoryValid(actor, sizeof(void*))) actorVt = *(void**)actor;
        if (pawn && IsMemoryValid(pawn, sizeof(void*)))  pawnVt = *(void**)pawn;

        const bool gameplayView = (actor && pawn) &&
            (actor == pawn || (actorVt && actorVt == pawnVt));

        static void* lastActor = (void*)~(uintptr_t)0;
        static int   lastVerdict = -1;
        if (actor != lastActor || (int)gameplayView != lastVerdict)
        {
            lastActor = actor;
            lastVerdict = gameplayView ? 1 : 0;
            Log(">>> VIEWACTOR: actor=0x%08X pawn=0x%08X actorVT=0x%08X pawnVT=0x%08X gameplay=%d",
                (unsigned)(uintptr_t)actor, (unsigned)(uintptr_t)pawn,
                (unsigned)(uintptr_t)actorVt, (unsigned)(uintptr_t)pawnVt,
                gameplayView ? 1 : 0);
        }
    }

    // Evaluated ONCE per CalcView and reused below, so the two halves of this
    // function cannot disagree about which mode we are in mid-frame.
    const bool theater = TheaterMode();
    {
        static bool wasTheater = false;
        if (theater != wasTheater)
        {
            g_pairValid = false;          // never carry half a stereo pair across
            if (!theater)
            {
                // You moved your head during the cutscene and none of it drove
                // the camera, so the origin is stale by exactly that much.
                g_posOriginSet = false;
                g_aimInit = false;
                g_lpValid = 0;
                Log(">>> THEATER off -- head origin and aim base cleared");
            }
            else Log(">>> THEATER on -- scripted camera left untouched");
            wasTheater = theater;
        }
    }

    // ARMS. A skeleton write, NOT part of the camera calculation -- which is
    // why it lives out here rather than inside DriveHands(). The whole
    // camera-write block below is skipped during theater, and a hide that never
    // gets un-hidden would leave the arms collapsed through every cutscene.
    {
        void* handsActor = nullptr;
        unsigned locOff = 0, rotOff = 0;
        const bool haveHands = HandsProbe_GetTargets(&handsActor, &locOff, &rotOff);

        if (haveHands)
        {
            // M7-S2: show the arms during a scripted animation. They are hidden
            // in normal play because the game animates them from a shoulder that
            // is not where yours is -- but during a scripted moment the game is
            // animating them ON PURPOSE and the stretch does not happen, so the
            // reason to hide them is gone. ArmHide_Update(actor, false) restores
            // the engine's own pose; it does not have to be told how.
            // M7-S4: only while the rig is ACTUALLY MOVING. The first attempt
            // gated on bFinishedStateAnimations and failed in opposite
            // directions in two scenes -- see the falsification banner in
            // GameState.cpp. Motion answers the question the flags cannot.
            const bool inScripted = ScriptedQol();
            const bool animating = inScripted && ScriptedHandsMoving();

            // Still, mid-sequence: hide the WHOLE actor -- arms, hands and
            // weapon. Those are the frozen poses the tester reported as goofy:
            // hands pointing straight down through the crawl scene, and parked
            // in your face after the balcony fall.
            ArmHide_SetActorHidden(handsActor, inScripted && !animating);

            // ---- M7-S5: !inScripted, NOT !animating. THIS IS A LATCH FIX ----
            // MEASURED: motion read exactly 0.0000 for a whole scripted scene
            // while peaking at 3.77 elsewhere in the same run. Not small --
            // FROZEN.
            //
            // ArmHide_Update(true) clears the skeleton's dirty byte so its
            // sleeve writes stick, and that stops the engine re-evaluating THE
            // WHOLE BONE ARRAY, including the bone the motion sampler reads. So
            // a sequence that started hidden stayed hidden forever (motion could
            // never rise) and one that started animating stayed visible. A
            // bistable latch, and it explained both reported scenes at once.
            //
            // Releasing the sleeves for the WHOLE sequence sets the dirty byte
            // back to 1, the engine keeps evaluating, and the motion signal
            // stays honest. Nothing is lost: ArmHide_SetActorHidden above
            // already hides arms, hands and weapon together during the still
            // stretches.
            //
            // Hiding by bone and measuring by bone cannot both be true at once.
            const bool hide = g_cfg.hideArmSleeves && g_cfg.sixDofHands &&
                !theater && !inScripted && !GameState_Paused();
            ArmHide_Update(handsActor, hide);

            // M6-S1, DIAGNOSTIC AND READ-ONLY. Behind HandRigProbe, default
            // off, and it stops after six dumps. Deliberately AFTER the sleeve
            // pass: the numbers we need describe the array as the cluster
            // transform will actually find it, hidden sleeves and all.
            ArmHide_RigProbe(handsActor);

            // Per-weapon: the shotgun and Tommy gun read better two-handed, so
            // the second hand can be kept for individual slots.
            const int wslot = HandsProbe_WeaponSlot();
            const bool hideHand = (wslot >= 0 && wslot <= 8)
                ? (g_cfg.hideHandSlot[wslot] != 0) : (g_cfg.hideInactiveHand != 0);

            // M6-S1: WHERE THE HIDE BECOMES A TRACK.
            //
            // The slots that hide the left hand are exactly the slots where it
            // has nothing to do, and the slots that KEEP it are the two-handed
            // weapons -- shotgun and Tommy gun, whose second hand is already on
            // the gun where it belongs. So one condition serves both halves of
            // what was asked for: show and track the left hand precisely where
            // it is hidden today, and leave the two-handers completely alone.
            //
            // Ability mode is excluded because there the left hand IS the active
            // one, holding the plasmid. It also keeps bone 43 -- in the right
            // cluster -- permanently out of reach of the cluster write.
            const bool handsFree = g_cfg.sixDofHands && !theater && !inScripted &&
                !GameState_Paused();
            g_leftTrackOn = handsFree && hideHand && g_cfg.offHandTracked > 0;

            // Which hand is free is the mirror of which one is working. The
            // plasmid lives in the LEFT hand and the actor is posed from the left
            // controller there, so the right hand is the one with nothing to do.
            g_freeHand = HandsProbe_AbilityMode() ? HAND_RIGHT : HAND_LEFT;

            // Gated on the SEQUENCE, not on motion: the whole-actor hide above
            // already covers the still stretches, and letting this follow the
            // motion signal too would have the two paths fighting each other
            // over the same bones.
            if (hideHand && handsFree && !g_leftTrackOn)
                ArmHide_UpdateInactiveHand(handsActor,
                    HandsProbe_AbilityMode() ? 0 : 1);
            else
                ArmHide_ReleaseInactiveHand();

            // Mode 3 drives itself -- it ignores the controller entirely, so it
            // does not wait on DriveHands. Releasing whenever we are not driving
            // is what hands the array back before a scripted sequence, which is
            // what keeps M7's motion signal honest.
            if (g_leftTrackOn && g_cfg.offHandTracked == 3)
                ArmHide_SweepFreeHand(handsActor, g_freeHand);
            else if (!g_leftTrackOn)
                ArmHide_ReleaseFreeHand();
        }
        else ArmHide_Reset();
    }

    // HEAD BOB. The engine adds walk bob, the landing dip and damage shake to
    // the camera location inside CalcView. Rather than filter that out after the
    // fact -- which cannot tell bob from a lift, a stair or a slope -- take the
    // view origin the engine started from: the pawn's own Location plus eye
    // height. Everything downstream (HMD translation, height offset, IPD) still
    // stacks on top, and pair-lock below now caches the stable point for both
    // eyes rather than a bobbed one.
    //
    // NOT during theater: a cinematic camera position is authored, not derived.
    if (g_cfg.disableHeadBob && !theater && CameraLocation)
    {
        float eye[3];
        if (GameState_GetPawnEyePoint(eye))
        {
            CameraLocation->x = eye[0];
            CameraLocation->y = eye[1];
            CameraLocation->z = eye[2];

            static bool once = false;
            if (!once) { once = true; Log(">>> HEADBOB: camera base = Pawn.Location + EyeHeight"); }
        }
    }

    // PHASE 10a: which delta-receiving object does the RENDER view's controller
    // reach? That one is the player world. Throttled -- this is a scan, and it
    // stops permanently the moment it locks (rule 1, section 11).
    if (g_cfg.deltaClamp && !g_targetLocked && (g_deltaObjA || g_deltaObjB))
    {
        static int probeTick = 0;
        if (((probeTick++) & 0x3F) == 0)
        {
            for (unsigned O = 0; O <= 0x400; O += 4)
            {
                if (!IsMemoryValid((const char*)pThis + O, 4)) continue;
                const uint32_t v = *(const uint32_t*)((const char*)pThis + O);
                if (v && (v == g_deltaObjA || v == g_deltaObjB))
                {
                    g_targetObj = v; g_targetLocked = true;
                    Log(">>> TARGET: player world 0x%08X via pThis+0x%X (phase 10 found +0xFC)",
                        v, O);
                    break;
                }
            }
        }
    }

    // --- TAG THIS FRAME'S EYE and push it to Present. The eye is owned HERE,
    //     on the game thread, by strict alternation of the producer index. It
    //     rides the FIFO out to the render thread with the frame it belongs to. ---
    const long w = g_eyeWr;
    const int  eye = (int)(w & 1);          // 0 == LEFT, 1 == RIGHT

    // PAIR LOCK: eye 0 snapshots the clean camera; eye 1 re-renders FROM that
    // snapshot instead of its own (4.2ms newer) camera. Only when we are
    // actually writing the camera -- read-only mode must stay read-only.
    if (eye == 0)
    {
        // Pair-lock runs BEFORE the main write block, so gating only the write
        // still lets it stamp eye 0's camera onto eye 1 during a cutscene.
        if (!theater)
        {
            g_pairRot = *CameraRotation;
            g_pairLoc = *CameraLocation;
            g_pairValid = true;
        }
        else g_pairValid = false;
    }
    else if (!theater && g_cfg.pairLock && g_pairValid &&
        g_cfg.cameraWrite && g_calls >= kArmAfterCalls)
    {
        // Measure BEFORE discarding it.
        const double dx = (double)CameraLocation->x - (double)g_pairLoc.x;
        const double dy = (double)CameraLocation->y - (double)g_pairLoc.y;
        const double dz = (double)CameraLocation->z - (double)g_pairLoc.z;
        g_ieLast = sqrt(dx * dx + dy * dy + dz * dz);
        g_ieSum += g_ieLast; ++g_ieN;
        if (g_ieLast > g_ieMax) g_ieMax = g_ieLast;

        *CameraRotation = g_pairRot;
        *CameraLocation = g_pairLoc;
    }

    // Latch the HMD pose ONCE per pair (LEFT frame), hold for both eyes so the
    // two eyes never render from different head rotations (§6).
    if (eye == 0)
    {
        float hq[4];
        XR_GetHeadQuat(hq);
        HeadQuatToDeg(hq, g_headPitch, g_headYaw, g_headRoll);

        // ---- MOTION AIM (S41) -------------------------------------------
        // The controller aim quaternion goes through the SAME conversion as the
        // head, so both land in the game's rotator frame and are directly
        // comparable. Latched once per pair, like the head pose, so the two eyes
        // never disagree about where the gun points.
        //
        // Clamped PER AXIS rather than as a cone. That is not laziness: a
        // controller pointed near vertical has a meaningless yaw (the gimbal
        // degeneracy the 12:37 pose log showed, where yaw jumped to +171 at
        // pitch +53). A per-axis clamp bounds that garbage to +-AimClampDeg
        // instead of letting it swing the gun, so the failure mode is "aim
        // saturates" rather than "aim flies away".
        g_aimHandValid = false;
        if (!AimUsesHeadNow())
        {
            HandPose hpose = {};
            // Plasmids are cast from the left hand, so the aim -- and therefore
            // the crosshair, which now reads the APPLIED offset -- follows it.
            const int aimHand = HandsProbe_AbilityMode() ? HAND_LEFT : HAND_RIGHT;
            if (Input_GetHandPose(aimHand, &hpose) && hpose.aimValid)
            {
                // The gun is a rigid prop in the hands mesh, so its muzzle
                // points somewhere fixed relative to the controller. The SHOT
                // takes the same offset as the crosshair, from one value, so
                // they cannot diverge.
                float qAim[4];
                CameraHook_OffsetQuat(hpose.aimQuat, g_cfg.cursorRot, qAim);

                double ap, ay, ar;
                HeadQuatToDeg(qAim, ap, ay, ar);

                // PALM AIM. The runtime's aim pose points where an extended
                // index finger would -- correct for a gun, wrong for a plasmid
                // cast, where the controller is held tilted up and the same pose
                // therefore aims high. Rotate it back down by a fixed amount.
                // NEGATIVE pulls the aim down. Plasmid hand only; DriveHands is
                // deliberately left alone so the hand model still tracks the
                // controller and keeps the casting pose.
                if (HandsProbe_AbilityMode()) ap += (double)g_cfg.plasmidAimPitch;

                double dY = WrapDeg180(ay - g_headYaw);
                double dP = ap - g_headPitch;

                const double c = (double)g_cfg.aimClampDeg;
                if (dY > c) dY = c;   if (dY < -c) dY = -c;
                if (dP > c) dP = c;   if (dP < -c) dP = -c;

                // Smooth the OFFSET, not the absolute angle -- so head motion
                // stays instant and only hand tremor gets damped.
                const double a = (double)g_cfg.aimSmooth;
                const double sy = g_aimOffYaw * a + dY * (1.0 - a);
                const double sp = g_aimOffPitch * a + dP * (1.0 - a);

                _InterlockedIncrement(&g_aimOffSeq);
                MemoryBarrier();
                g_aimOffYaw = (float)sy;
                g_aimOffPitch = (float)sp;
                MemoryBarrier();
                _InterlockedIncrement(&g_aimOffSeq);

                g_aimHandYaw = g_headYaw + sy;
                g_aimHandPitch = g_headPitch + sp;
                g_aimHandValid = true;

                // THE SEAM. Empty today -- see the AimOverride banner. It sits
                // here, after the controller aim is composed and before anything
                // reads g_aimHandYaw, because a feature that owns the aim owns
                // the FINAL answer: the clamp, the smoothing and the plasmid
                // pitch trim above are all corrections to a CONTROLLER pose, and
                // a barrel or a wrench tip is not one.
                double ovP = g_aimHandPitch, ovY = g_aimHandYaw;
                if (AimOverride(&ovP, &ovY))
                {
                    g_aimHandPitch = ovP;
                    g_aimHandYaw = ovY;
                }
            }
        }

        float hp[3];
        XR_GetHeadPos(hp);

        // Kept for the 6-DOF hands write, which needs the controller pose
        // relative to the HEAD rather than to the recentre origin.
        g_lastHeadPos[0] = hp[0]; g_lastHeadPos[1] = hp[1]; g_lastHeadPos[2] = hp[2];

        // Recenter: first real (nonzero) sample, or Numpad-Del re-captures.
        static bool prevDec = false;
        const bool decDown = (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0;
        const bool wantRecenter = (decDown && !prevDec);
        prevDec = decDown;

        // Numpad + : cycle which ROTSCAN candidate we drive (+0x1E4 / +0x328).
        // Only one of them is the gun; poke and watch.
        {
            static bool prevAdd = false;
            const bool d = (GetAsyncKeyState(VK_ADD) & 0x8000) != 0;
            if (d && !prevAdd)
            {
                g_aimCand ^= 1;
                g_aimInit = false;                 // re-baseline on the new field
                Log(">>> HEAD-AIM candidate -> +0x%X", kAimOffsets[g_aimCand & 1]);
            }
            prevAdd = d;
        }

        const bool haveSample = (fabs(hp[0]) + fabs(hp[1]) + fabs(hp[2])) > 1e-4;

        // ---- S36: DO NOT seed the origin from the first non-zero pose. -----
        // MEASURED (02:49 log): the runtime's first pose arrived 0.2s after XR
        // init with y = -0.986 m -- the headset was still on the desk. That
        // became the origin, so once it was on a head the up offset was ~+99cm,
        // clamped to +20, and PINNED there for the whole session. The camera
        // then rode 20cm above the pawn: world looks fine (you are just taller),
        // but the arms and weapon are drawn at the PAWN's eye height and fall
        // out of the bottom of the frame, leaving the head-locked crosshair
        // floating ~35 deg above the gun. WeaponScale had nothing to do with it.
        //
        // Whether this bit was pure luck about where the headset physically was
        // when the runtime woke up, which is why it looked like a regression.
        //
        // Gate on the hook being ARMED: by then the game is rendering a real
        // view, so the headset is on a head, not a desk.
        const bool mayAutoSeed = haveSample && (g_calls >= kArmAfterCalls);

        if ((!g_posOriginSet && mayAutoSeed) || (wantRecenter && haveSample))
        {
            g_posOrigin[0] = hp[0]; g_posOrigin[1] = hp[1]; g_posOrigin[2] = hp[2];
            g_posOriginSet = true;
            Log("camera: HEAD POSITION recentered (origin %.3f %.3f %.3f m)%s",
                hp[0], hp[1], hp[2], wantRecenter ? "  [manual]" : "  [auto]");
        }

        if (g_posOriginSet)
        {
            // XR LOCAL: +x right, +y up, +z BACK. Metres -> cm.
            g_posRight = (double)(hp[0] - g_posOrigin[0]) * 100.0;
            g_posUp = (double)(hp[1] - g_posOrigin[1]) * 100.0;
            g_posFwd = -(double)(hp[2] - g_posOrigin[2]) * 100.0;

            // ---- SATURATION WATCHDOG (self-heal) ---------------------------
            // A bad origin shows up as an axis pinned at its clamp with a RAW
            // delta far past it. Real head motion never parks 60cm off-centre
            // for seconds. If it does, the origin is wrong -- re-seed rather
            // than quietly rendering from the wrong height for an hour.
            {
                const double rawUp = g_posUp, rawFwd = g_posFwd, rawSide = g_posRight;
                const bool wayOff =
                    fabs(rawUp) > kPosUpMax * 3.0 ||
                    fabs(rawSide) > kPosSide * 3.0 ||
                    fabs(rawFwd) > kPosFwdMax * 3.0;

                static DWORD offSince = 0;
                const DWORD nowTick = GetTickCount();

                if (wayOff)
                {
                    if (!offSince) offSince = nowTick;
                    else if (nowTick - offSince > 3000)
                    {
                        Log("!!! camera: POSITION ORIGIN BAD. raw offset %.0f/%.0f/%.0f cm",
                            rawSide, rawUp, rawFwd);
                        Log("!!! camera: (origin was probably captured with the headset off");
                        Log("!!! camera:  your head.) Re-seeding from the current pose.");
                        g_posOrigin[0] = hp[0]; g_posOrigin[1] = hp[1]; g_posOrigin[2] = hp[2];
                        g_posRight = g_posUp = g_posFwd = 0.0;
                        offSince = 0;
                    }
                }
                else offSince = 0;
            }

            if (g_posRight > kPosSide)   g_posRight = kPosSide;
            if (g_posRight < -kPosSide)   g_posRight = -kPosSide;
            if (g_posUp > kPosUpMax)  g_posUp = kPosUpMax;
            if (g_posUp < -kPosDownMax)g_posUp = -kPosDownMax;
            if (g_posFwd > kPosFwdMax) g_posFwd = kPosFwdMax;
            if (g_posFwd < -kPosBackMax)g_posFwd = -kPosBackMax;
        }

        // Publish the LATCHED pose back to the render thread (§2). Position is
        // the APPLIED head center -- origin + CLAMPED offset mapped back to XR
        // axes -- so a saturated clamp can't re-open the render/layer mismatch.
        // Valid only when the camera actually follows this pose.
        {
            float px, py, pz;
            if (g_posOriginSet)
            {
                px = g_posOrigin[0]; py = g_posOrigin[1]; pz = g_posOrigin[2];
                if (g_cfg.headPosition)
                {
                    px += (float)(g_posRight * 0.01);   // cm -> m, XR +x right
                    py += (float)(g_posUp * 0.01);   //           XR +y up
                    pz -= (float)(g_posFwd * 0.01);   //           XR +z BACK
                }
            }
            else { px = hp[0]; py = hp[1]; pz = hp[2]; }

            const bool applied = !theater && g_cfg.cameraWrite && g_cfg.headTracking &&
                (g_calls >= kArmAfterCalls);

            // Which pose did the image ACTUALLY render from?
            // S41: motion aim composes the view from the CURRENT head quat and
            // writes it outright, so there is no frame of indirection left to
            // compensate for. Publishing the previous pose here made the
            // compositor reproject with a pose the image was not rendered from
            // -- the S2 flicker, reopened backwards. Head-only, because
            // g_prevQuat is a head quaternion.
            const bool lag = (g_cfg.headAim && ModeUsesHead() && g_prevQuatValid);
            const float* sq = lag ? g_prevQuat : hq;

            _InterlockedIncrement(&g_lpSeq);        // odd == writing
            MemoryBarrier();
            g_lpQuat[0] = sq[0]; g_lpQuat[1] = sq[1];
            g_lpQuat[2] = sq[2]; g_lpQuat[3] = sq[3];
            g_lpPos[0] = px; g_lpPos[1] = py; g_lpPos[2] = pz;
            g_lpValid = applied ? 1 : 0;
            MemoryBarrier();
            _InterlockedIncrement(&g_lpSeq);        // even == done

            g_prevQuat[0] = hq[0]; g_prevQuat[1] = hq[1];
            g_prevQuat[2] = hq[2]; g_prevQuat[3] = hq[3];
            g_prevQuatValid = true;
        }

    }

    // Theater edge. On the way out, clear the origin and the aim base so the
    // next frame re-seeds from where you are NOW instead of jumping back to
    // wherever you were standing when the cutscene started.
    {
        static bool wasTheater = false;
        const bool nowTheater = TheaterMode();
        if (wasTheater && !nowTheater)
        {
            g_posOriginSet = false;
            g_aimInit = false;
            g_lpValid = 0;
            Log(">>> THEATER off -- head origin and aim base cleared, re-seeding");
        }
        else if (!wasTheater && nowTheater)
        {
            Log(">>> THEATER on -- scripted camera left untouched");
        }
        wasTheater = nowTheater;
    }

    // --- THE WRITE (§6e). Only when armed. ---
    if (!theater && g_cfg.cameraWrite && g_calls >= kArmAfterCalls)
    {
        if (!IsMemoryWritable(CameraLocation, sizeof(FVector)))
        {
            if (!g_armLogged) { g_armLogged = true; Log("camera: !!! CameraLocation NOT WRITABLE. No stereo."); }
        }
        else
        {
            // Head tracking (Phase 11): compose the latched HMD orientation onto
            // the clean mouse heading, then WRITE it. Aim stays on
            // Controller.Rotation (untouched), so the gun won't follow the head.
            const FRotator cleanRot = *CameraRotation;    // mouse heading, pre-head

            // ---- ADVANCE g_aimBase BEFORE THE VIEW IS COMPOSED --------------
            // The VIEW is built from g_aimBase a few lines below; the AIM field,
            // which the GUN is drawn from, is written from it much further down.
            // Advancing the base in between left the gun ONE FRAME OF YAW ahead
            // of the view -- an error proportional to turn rate whose sign flips
            // with direction, so the weapon swells turning one way and shrinks
            // turning the other. No ForegroundFovValue can cancel it, because
            // the FOV is not what is changing. All three consumers -- view, aim
            // field and DriveHands -- now read the SAME base.

            FRotator* aimField = nullptr;
            bool      aimNowCut = false;

            // MEASURED 19:26-19:27: DrawHook_MenuUp() is a draw-signature
            // heuristic and count 1095 flipped it 12 times in one spot during
            // gameplay. Freezing the base here stops the view turning while the
            // character, arms and head tracking all keep working -- with nothing
            // on screen to say why. Trust the game's own pause state instead, and
            // only believe the heuristic when we are NOT in gameplay.
            const bool uiUp = GameState_Paused() ||
                (DrawHook_MenuUp() && !GameState_InGame());

            // ---- M7-S2: GIVE THE SEQUENCE ITS DIRECTION BACK ----------------
            // Controller.Rotation (+0x1E4) drives the view, the weapon trace AND
            // THE WALK DIRECTION -- see the warning in docs/ENGINE-MAP.md. With
            // head-aim on, the mod writes the head's heading into that field, so
            // during a forced-walk sequence turning your head steered where the
            // game walked you. That is the reported bug.
            //
            // The fix is to not WRITE the field while a scripted animation runs.
            // Controller.Rotation then stays exactly as the game set it.
            //
            // HEAD LOOK IS UNAFFECTED, which is the whole point. The view is
            // composed into *CameraRotation on a separate path that never
            // touches this field, so you keep full freedom to look around while
            // the game keeps full control of where you go.
            //
            // ---- M7-S3: READING AND WRITING ARE TWO DIFFERENT DECISIONS -----
            // FIXED HERE: the first cut suppressed the write by leaving aimField
            // null, which skipped the WHOLE block -- including the part that
            // advances g_aimBase by the game's own rotation delta. The view is
            // composed from g_aimBase, so the scripted camera stopped turning
            // the player at all: the crawl scene never rotated and the balcony
            // landing faced away from the action.
            //
            // So we still enter the block and still follow the game's rotation.
            // Only the write back into Controller.Rotation is suppressed.
            const bool scriptedAim = ScriptedAimReleased();

            // ---- THE CINEMATIC REFERENCE, DROPPED ON BOTH EDGES -------------
            // ScriptedCameraFollow differences the game's own camera yaw frame to
            // frame, so it needs a reference -- and the reference is only valid
            // WITHIN one window.
            //
            // ⚠ THE FIRST VERSION NEVER DROPPED IT, and the symptom was reported
            // before the cause was found: "both runs had the balcony fall land in
            // different spots -- first almost perfect, second way off". A latch
            // set on the first scripted frame and never cleared means the SECOND
            // window of a session differences against a value left over from the
            // END of the first, which is an arbitrary jump straight into
            // g_aimBase. The first scene of a run is clean; every one after it
            // inherits garbage.
            //
            // DROPPED HERE, at the edge detector, rather than inside the follow
            // block itself -- that block sits behind headAim, headTracking, the
            // UI gate and the starvation gate, so a window that ends while any of
            // those is false would never clear it. This runs on every CalcView.
            // Both edges, because a scene must open from where it actually starts
            // rather than from wherever the last one ended.
            static bool s_wasScriptedAim = false;
            if (s_wasScriptedAim != scriptedAim)
            {
                g_cineHave = false;

                // Same rule, same reason: an offset that spans two windows is an
                // arbitrary jump into the second one. Both edges.
                g_scriptedManualYaw = 0;
                g_scriptedCancelled = 0;
            }

            // Re-arm on the way out, or the base is stale by however far the
            // sequence turned you and the view snaps. Same reason the cutscene
            // path below clears g_aimInit.
            if (s_wasScriptedAim && !scriptedAim)
            {
                g_aimInit = false;
                Log(">>> SCRIPTED: aim released back to the player");
                if (g_cfg.scriptedRotProbe) g_edgePending = 2;
            }
            else if (!s_wasScriptedAim && scriptedAim)
            {
                Log(">>> SCRIPTED: aim handed to the game (head look kept)");
                if (g_cfg.scriptedRotProbe) g_edgePending = 1;
            }
            s_wasScriptedAim = scriptedAim;

            if (g_cfg.headAim && g_cfg.headTracking &&
                !uiUp && !CameraHook_Starved())
            {
                const unsigned off = kAimOffsets[g_aimCand & 1];
                FRotator* const a = (FRotator*)((uint8_t*)pThis + off);
                if (IsMemoryWritable(a, sizeof(FRotator)))
                {
                    aimField = a;

                    ScriptedEdgeReport(aimField);

                    static bool s_wasCut = false;
                    aimNowCut = GameState_Cutscene();
                    if (s_wasCut && !aimNowCut) g_aimInit = false;
                    s_wasCut = aimNowCut;

                    if (!g_aimInit)
                    {
                        // ---- ARMING IS A WRITE, SO IT GETS THE HARDER CHECK --
                        // Whatever lands in g_aimBase becomes the player's frame
                        // of reference: the aim field is written from it, and two
                        // of the four movement modes subtract terms from it to
                        // steer walking. Adopting a bad reading here corrupts
                        // both at once.
                        //
                        // MEASURED 2026-08-11, the Little Sister crawl: at a
                        // one-frame collapse of the scripted window this read
                        // exactly (0,0) while the player's true heading was
                        // -19.64. It was adopted, and the aim field then sat
                        // 18.6 deg off the pawn for the whole 58-second scene.
                        //
                        // EXACTLY ZERO IN BOTH is the transient's fingerprint,
                        // and it is the right test rather than a comparison
                        // against the pawn: the pawn's rotator and the aim field
                        // legitimately diverge inside a scene, so their gap
                        // cannot decide anything here.
                        //
                        // Not a hard refusal -- a genuine (0,0) heading must
                        // never hang the aim, so a run of rejections arms anyway
                        // and says so.
                        static int s_armRejects = 0;
                        const bool zeroRead = (a->pitch == 0 && a->yaw == 0);

                        if (zeroRead && s_armRejects < 8)
                        {
                            ++s_armRejects;
                            if (s_armRejects == 1)
                                Log("!!! HEAD-AIM arm REFUSED: field reads "
                                    "p=%d y=%d r=%d -- keeping base y %+.2f. "
                                    "See ONE SCENE, ONE WINDOW in GameState.",
                                    a->pitch, a->yaw, a->roll,
                                    YawDeg(g_aimBase.yaw));
                        }
                        else
                        {
                            if (s_armRejects)
                                Log("!!! HEAD-AIM armed after %d refused "
                                    "frame(s)%s", s_armRejects,
                                    zeroRead ? " -- STILL ZERO, arming anyway"
                                    : "");
                            s_armRejects = 0;

                            // The jump this arm makes to the frame of reference.
                            // Logged so a future session can decide whether a
                            // magnitude threshold is safe -- it is not yet, since
                            // with ScriptedCameraFollow off a legitimate exit
                            // re-arm IS a large jump.
                            const double jump = g_aimInitEver
                                ? YawDeg(a->yaw - g_aimBase.yaw) : 0.0;

                            g_aimBase = *a;
                            g_aimLastWrote = *a;
                            g_aimInit = true;
                            g_aimInitEver = true;
                            Log(">>> HEAD-AIM armed on +0x%X  (base y %+.2f, "
                                "jump %+.2f)", off, YawDeg(g_aimBase.yaw), jump);
                        }
                    }
                    else
                    {
                        // Only the GAME's own change since our last write.
                        const int dP = RotDelta(a->pitch, g_aimLastWrote.pitch);
                        const int dY = RotDelta(a->yaw, g_aimLastWrote.yaw);
                        const int dR = RotDelta(a->roll, g_aimLastWrote.roll);

                        // Published BEFORE we overwrite the rendered pitch, so
                        // the error reflects what the ENGINE currently believes.
                        PublishPitchError(a->pitch);

                        g_aimGameDPitch += fabs((double)dP);

                        g_gameDYaw = dY;        // S77, read by the turn gate below

                        // ---- M7-S5: IS THE GAME SLEWING US RIGHT NOW? -------
                        // DIAGNOSTIC ONLY, gates nothing.
                        //
                        // Testing the entry-stall hypothesis: that
                        // StartForcePlayerMove interpolates the player into
                        // position and heading BEFORE the scripted animation
                        // begins, and that we spend that whole window still
                        // writing the aim field and fighting the slew. That
                        // would explain the random 1-6s duration, the weapon
                        // still being up, the controller dragging the view, the
                        // audio desync, and ending up off-centre.
                        //
                        // A large rotation delta with the stick CENTRED is the
                        // signature: the player is not asking to turn, so
                        // anything this big is the game doing it. 182 units is
                        // one degree.
                        {
                            float sx = 0.0f;
                            const bool haveStick = Input_GetTurnX(&sx);
                            const bool centred = !haveStick || fabsf(sx) <= 0.02f;
                            const int mag = (dY < 0 ? -dY : dY) +
                                (dP < 0 ? -dP : dP);

                            static DWORD s_lastSlew = 0;
                            const DWORD nowSlew = GetTickCount();
                            if (centred && mag > 182 && nowSlew - s_lastSlew >= 250)
                            {
                                s_lastSlew = nowSlew;
                                Log(">>> SLEW: game moved aim %.2f deg (p %.2f y %.2f) "
                                    "stick centred, scripted=%d",
                                    mag / 182.0444, dP / 182.0444, dY / 182.0444,
                                    scriptedAim ? 1 : 0);
                            }
                        }

                        // CUTSCENE HEAD-OVERRIDE: during a scripted camera the
                        // game slews aim to point the view where the script
                        // wants; accumulating that swings the whole world around
                        // your locked head. Freeze the heading so ONLY the head
                        // rotates the view. Position still follows the script.
                        // 
                        // FreezeGameRotation: the game may not rotate the view
                        // at all. Screenshake, recoil kick, camera-anim and
                        // scripted slews ALL arrive through these three lines
                        // and nowhere else, so dropping them removes the whole
                        // class at once. YAW MUST BE INCLUDED -- mode 2 already
                        // discards base pitch, so yaw is the axis you actually
                        // feel, and leaving it in is why a yaw-only version
                        // looked like it did nothing.
                        //
                        // Requires ModYaw: the player's own stick turn also
                        // arrives as dY, so freezing without mod-side yaw would
                        // mean you could not turn at all.
                        const bool freeze = (g_cfg.freezeGameRot && g_cfg.modYaw);

                        // ==========================================================
                        //  TEMPORARY -- M7-S3, and it has a KNOWN GAP. Read this.
                        // ==========================================================
                        // FreezeGameplayRotation discards the game's own rotation
                        // during ordinary play: screenshake, recoil kick and the
                        // subtle auto-pan toward enemies all arrive as these three
                        // deltas and nowhere else.
                        //
                        // Gated on the stick being CENTRED, which is what lets it
                        // work without ModYaw: your own turning still arrives as dY
                        // and must survive. That deliberately keeps grave 12's
                        // stick-zeroing out of this entirely.
                        //
                        // ⚠ THE GAP: A BATHYSPHERE RIDE IS NOT A SCRIPTED ANIMATION,
                        // so this freezes there too and the ride's camera stops
                        // following the sphere. Excluding it by level name was
                        // considered and is NOT possible today -- the mod does not
                        // know what map it is on; g_level is used only for
                        // Level.Pauser.
                        //
                        // THE REAL FIX IS ALREADY IDENTIFIED, and this block should
                        // be revisited the moment it lands:
                        // ActionEnableBathysphereModeForPlayer sets
                        // ShockPlayer.bCannotFall = true for the whole ride. One
                        // Tier 0 bool. Find its offset and AND it in here.
                        // See .planning/ROADMAP.md.
                        //
                        // Ships default 0 for that reason.
                        // M7-S4: THE GAP IS CLOSED. A bathysphere ride is not a
                        // scripted animation, so the freeze used to apply there
                        // and stop the camera following the sphere. It is now
                        // excluded by its own signal -- Pawn.bCannotFall, which
                        // the ride sets for its whole duration. The level-name
                        // gate that was considered instead was impossible: the
                        // mod does not know what map it is on.
                        bool gameplayFreeze = false;
                        if (g_cfg.freezeGameplayRot && !scriptedAim &&
                            !GameState_Bathysphere())
                        {
                            float tx = 0.0f;
                            const bool haveStick = Input_GetTurnX(&tx);
                            gameplayFreeze = !haveStick || fabsf(tx) <= 0.02f;
                        }

                        // M7-S4 comfort: following the game's rotation through a
                        // scripted sequence is what turns you to face the action
                        // -- and it is also exactly the kind of unrequested
                        // motion that makes some people sick. At 0 the view
                        // holds still and the player turns themselves with the
                        // right stick, which already works during sequences.
                        const bool scriptedRotBlocked =
                            scriptedAim && !g_cfg.scriptedRotFollow;

                        // ---- ONE SOURCE FOR SCRIPTED YAW --------------------
                        // MEASURED 2026-08-11, and it cost three runs to see.
                        // These two paths -- the aim-field delta here, and
                        // ScriptedCameraFollow below -- were BOTH advancing
                        // g_aimBase. Most of the balcony they do not overlap:
                        // the scene puts 0.00 deg/s on the aim field and up to
                        // 125 deg/s on its own camera. But the opening SNAP
                        // moves both, identically:
                        //
                        //   game injected 41.03 deg/s into the AIM FIELD,
                        //                 41.03 deg/s onto its own CAMERA
                        //
                        // so it landed TWICE and the view finished a whole snap
                        // past the authored heading (measured at exactly -90.0,
                        // held for twelve seconds). The error equalled the snap
                        // in all three runs, sign included: +41, -4, -77 against
                        // the tester's "45 right", "almost perfect", "90 left".
                        //
                        // The camera is DOWNSTREAM of the aim field, so it
                        // already carries anything the scene did to
                        // Controller.Rotation. Following it alone counts each
                        // rotation once, whichever field the scene used.
                        //
                        // ⚠ WHY THE ORIGINAL MEASUREMENT MISSED IT.
                        // ScriptedCameraFollow was justified by a deg/s average
                        // over a 67-second window, and a per-second rate cannot
                        // see a one-frame event -- the one second where both
                        // moved together averaged into invisibility.
                        //
                        // YAW ONLY. The follow below never handled pitch or
                        // roll, so blocking those here would silently drop
                        // scripted pitch instead of deduplicating it.
                        const bool cameraOwnsYaw =
                            scriptedAim && g_cfg.scriptedCameraFollow != 0;

                        if (!aimNowCut && !freeze && !gameplayFreeze &&
                            !scriptedRotBlocked)
                        {
                            g_aimBase.pitch += dP;
                            if (!cameraOwnsYaw) g_aimBase.yaw += dY;
                            g_aimBase.roll += dR;
                        }

                        TurnRateProbe(dY);
                        ScriptedRotProbe(scriptedAim, dY, cleanRot.yaw,
                            aimNowCut, freeze, gameplayFreeze,
                            scriptedRotBlocked);

                        // ---- THE BALCONY FALL -- CONFIRMED AND ON -----------
                        // MEASURED 2026-08-11: across the 67-second balcony
                        // window the game injected rotation into the aim field
                        // exactly ONCE, 1.85 degrees, at the very start, while
                        // putting up to 125 deg/s onto its OWN camera rotation --
                        // with every gate open, so nothing was being discarded.
                        // The Little Sister scene, which always turned the player
                        // correctly, does the opposite and uses the aim field.
                        // TWO SCENES, TWO DIFFERENT FIELDS. cleanRot is the
                        // game's own value, read before we touch anything.
                        //
                        // ⚠ THE REFERENCE MUST BE DROPPED ON BOTH EDGES, and the
                        // first version of this did not drop it at all. s_have
                        // was set on the first scripted frame and never cleared,
                        // so the SECOND scripted window of a session differenced
                        // against a value left over from the END of the first --
                        // an arbitrary jump straight into g_aimBase. Measured as
                        // "both runs had the balcony fall land in different
                        // spots; first almost perfect, second way off": the first
                        // scene of a run is clean and every one after it inherits
                        // garbage.
                        //
                        // Dropping on entry as well as exit is what makes each
                        // scene open from where it actually starts rather than
                        // from wherever the last one ended -- the same rule the
                        // reference mod states for its cinematic reference.
                        if (g_cfg.scriptedCameraFollow && scriptedAim)
                        {
                            if (g_cineHave)
                            {
                                const int d =
                                    (int)(short)(cleanRot.yaw - g_cinePrev);
                                g_aimBase.yaw += d;

                                ScriptedRecentre(d);
                            }
                            g_cinePrev = cleanRot.yaw;
                            g_cineHave = true;
                        }
                    }
                }
            }

            // The aim block above consumes the latch when it runs. Reaching
            // here with it still set means the block was SKIPPED -- head aim
            // off, the hook starved, or a UI up -- and that is worth a line of
            // its own, because a silent log at a window boundary otherwise
            // reads as a window that never happened.
            ScriptedEdgeReport(nullptr);

            // ---- MOD-SIDE SMOOTH YAW ---------------------------------------
            // MUST run BEFORE finalRot is composed. The view, the aim field and
            // DriveHands are all built from g_aimBase below; advancing it after
            // they read it leaves the gun one frame of yaw ahead of the view,
            // which reads as the weapon flickering while you turn.
            //
            // Rotating the heading ourselves is what gives turning authority:
            // it works even in states where the game ignores the pad entirely,
            // which is how you get stick look during a scripted sequence.
            // M7-S3: ALSO run this while a scripted animation is playing, even
            // with ModYaw off. It rotates g_aimBase, which the VIEW is composed
            // from, and never touches Controller.Rotation -- so it gives stick
            // look during a sequence WITHOUT influencing where the sequence
            // walks you. That is exactly the ask, and it is why the aim write
            // above must stay suppressed for this to be safe.
            //
            // No double-turn: during these sequences the game has pushed
            // NullInput and is discarding the pad, which is the case
            // Input_GetTurnX was written for. If a sequence ever turns out NOT
            // to suppress the pad, turning there will feel doubled -- that is
            // the symptom to report.
            if (g_cfg.modYaw || scriptedAim)
            {
                static LARGE_INTEGER s_freq = {};
                static LARGE_INTEGER s_prev = {};
                if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);

                LARGE_INTEGER nowQ;
                QueryPerformanceCounter(&nowQ);
                float dt = s_prev.QuadPart
                    ? (float)((double)(nowQ.QuadPart - s_prev.QuadPart) /
                        (double)s_freq.QuadPart)
                    : 0.0f;
                s_prev = nowQ;

                if (dt > 0.10f) dt = 0.0f;      // hitch, load or alt-tab

                float tx = 0.0f;
                if (dt > 0.0f && Input_GetTurnX(&tx))
                {
                    const float mag = fabsf(tx);
                    if (mag > 0.001f)
                    {
                        const float curve = (tx < 0.0f) ? -(mag * mag)
                            : (mag * mag);
                        const int32_t step =
                            (int32_t)(curve * g_cfg.modYawSpeed * dt * 182.0444f);
                        g_aimBase.yaw += step;

                        // Player-added yaw, so ScriptedRecentre can hand it back
                        // to the scene. Same value, one place.
                        ScriptedManualYaw(step, scriptedAim);
                    }
                }
            }

            FRotator finalRot = cleanRot;
            // Theater: the scripted camera goes through untouched. The screen is
            // world-locked, so turning your head must move your gaze ACROSS it,
            // not pan what is drawn on it.
            if (g_cfg.headTracking)
            {
                if (g_cfg.headAim && !ModeUsesHead() && g_aimInit)
                {
                    // MOTION AIM: the aim field now carries the CONTROLLER, so
                    // the view can no longer be inherited from it. Compose the
                    // view from the head and write it outright. This also closes
                    // the one-frame indirection that made the weapon swell while
                    // turning.
                    //
                    // NOTE: lean and camera-anim (headbob) are dropped on this
                    // path. Headbob is already zero via the mod; PC lean is
                    // unbound on a controller.
                    finalRot = ComposeHeadLocal(g_aimBase, g_headYaw, g_headPitch,
                        g_cfg.headAimMode >= 2);
                    finalRot.roll = g_aimBase.roll;
                    finalRot = ApplyWorldSpaceYaw(finalRot, 0.0, 0.0,
                        g_cfg.headRoll ? g_headRoll : 0.0);
                }
                else if (g_cfg.headAim)
                {
                    // Head yaw/pitch already reached the view THROUGH the aim
                    // field (+0x1E4 -> Controller.Rotation -> CameraRotation).
                    // Adding them here too applies the head TWICE and the view
                    // swims as if the mouse were being dragged. Roll only.
                    finalRot = ApplyWorldSpaceYaw(*CameraRotation,
                        0.0, 0.0, g_cfg.headRoll ? g_headRoll : 0.0);
                }
                else
                {
                    finalRot = ApplyWorldSpaceYaw(*CameraRotation,
                        g_headYaw, g_headPitch, g_headRoll);
                }

                // S41: say which branch is live. Three paths now write the view
                // and picking the wrong one looks like a tracking fault rather
                // than a code fault -- which cost a session.
                {
                    static int lastBranch = -1;
                    const int b = (g_cfg.headAim && !ModeUsesHead() && g_aimInit) ? 2
                        : (g_cfg.headAim ? 1 : 0);
                    if (b != lastBranch)
                    {
                        lastBranch = b;
                        Log(">>> VIEW PATH: %s",
                            b == 2 ? "motion aim (view=head, aim=controller)"
                            : b == 1 ? "head-aim (view inherited from aim field, roll only)"
                            : "legacy additive (head applied to view directly)");
                    }
                }

                // DISABLED. S75/S78/S79 measurably made things worse: the
                // "script outranks our offset" unwind fired three times in one
                // session at 14.9, -44.0 and 56.5 degrees, dragging the view
                // around during scripted events. Combined with the pitch-based
                // cutscene latch it produced the camera-pull complaints.
                //
                // Reverting to the pre-1.0.2 policy: the game owns the camera
                // during scripted events and the mod does not fight it. Manual
                // turning during cutscenes will be rebuilt separately, against
                // an authoritative signal rather than a stick heuristic.
                if (false)
                {

                    // S75: RENDER-SIDE CUTSCENE TURN. MEASURED from the decompile:
                    // ShockPlayerController::Use pushes input context NullInput, so
                    // during the balcony the game discards the stick and no delta
                    // ever reaches Rotation. We read the stick ourselves and rotate
                    // only the VIEW -- Controller.Rotation is never touched, so the
                    // game's idea of where you point stays correct and scripted
                    // triggers still fire.
                    {
                        static LARGE_INTEGER s_freq = {}, s_last = {};
                        static double s_cutYaw = 0.0;          // rotator units
                        const double kTurnDegPerSec = 90.0;    // raise/lower to taste

                        if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
                        LARGE_INTEGER now; QueryPerformanceCounter(&now);
                        const double dt = s_last.QuadPart
                            ? (double)(now.QuadPart - s_last.QuadPart) / (double)s_freq.QuadPart
                            : 0.0;
                        s_last = now;

                        static double s_pushSec = 0.0, s_deadSec = 0.0;
                        static double s_liveSec = 0.0, s_idleSec = 0.0;
                        static bool   s_ignored = false;

                        // S79: the offset must never survive anything. MEASURED: it
                        // reached -159.2 deg and rode straight through a save load,
                        // leaving the view a half turn from the character -- hence
                        // the inverted stick and the backwards head parallax.
                        if (CameraHook_Starved() || DrawHook_MenuUp() || GameState_Paused())
                        {
                            s_cutYaw = 0.0;
                            s_ignored = false;
                            s_pushSec = s_deadSec = s_liveSec = s_idleSec = 0.0;
                        }

                        float tx = 0.0f;
                        const bool haveStick = Input_GetTurnX(&tx);
                        const bool pushing = haveStick && fabsf(tx) > 0.30f;
                        // 4 units == 0.02 deg. Deliberately SENSITIVE: ANY real
                        // response must count. At 30 a gentle stick push during
                        // normal play read as "ignored" -- five false arms before
                        // the first cutscene even started.
                        const bool gameMoved = (g_gameDYaw > 4 || g_gameDYaw < -4);
                        const bool goodDt = (dt > 0.0 && dt < 0.25);

                        if (goodDt)
                        {
                            if (pushing) { s_pushSec += dt; s_idleSec = 0.0; }
                            else { s_pushSec = 0.0; s_idleSec += dt; }

                            if (pushing && !gameMoved) s_deadSec += dt; else s_deadSec = 0.0;
                            if (pushing && gameMoved) s_liveSec += dt; else s_liveSec = 0.0;
                        }

                        const bool blocked = DrawHook_MenuUp() || GameState_Paused();

                        if (!s_ignored)
                        {
                            if (!blocked && s_pushSec > 0.30 && s_deadSec > 0.30)
                                s_ignored = true;               // ARM
                        }
                        else
                        {
                            // RELEASE only for a real reason. Letting go of the
                            // stick is NOT one -- that is what chattered 40 times.
                            if (blocked || s_liveSec > 0.25 || s_idleSec > 5.0)
                                s_ignored = false;
                        }

                        if (s_ignored && pushing && goodDt)
                            s_cutYaw += (double)tx * kTurnDegPerSec * 182.0444 * dt;
                        else if (goodDt && ((gameMoved && !pushing) ||
                            g_gameDYaw > 150 || g_gameDYaw < -150))
                        {
                            // S78: the SCRIPT OUTRANKS our offset. StartForcePlayerMove
                            // (ShockPlayerController::Use -- the syringe) slews your yaw
                            // at ForceMoveRotationDeltaPerSecond=65536, a full turn per
                            // second, to plant you in the scripted pose. Our offset rode
                            // on top of it, so the shot framed ~45 deg off and afterwards
                            // "forward" walked you across the room. Whenever the game
                            // turns you and you are NOT on the stick, hand the framing
                            // back -- rate limited, so it eases instead of snapping.
                            static bool s_unwinding = false;
                            if (!s_unwinding && (s_cutYaw > 900.0 || s_cutYaw < -900.0))
                            {
                                s_unwinding = true;
                                Log(">>> STICK: script is turning you -- unwinding %.1f deg",
                                    s_cutYaw / 182.0444);
                            }

                            const double step = 120.0 * 182.0444 * dt;   // deg/s
                            if (s_cutYaw > step) s_cutYaw -= step;
                            else if (s_cutYaw < -step) s_cutYaw += step;
                            else { s_cutYaw = 0.0; s_unwinding = false; }
                        }

                        static bool s_wasIgnored = false;
                        if (s_ignored != s_wasIgnored)
                        {
                            s_wasIgnored = s_ignored;
                            Log(">>> STICK: game is %s input (render-side turn %s)",
                                s_ignored ? "IGNORING" : "accepting",
                                s_ignored ? "ON" : "off");
                        }

                        finalRot.yaw += (int)s_cutYaw;
                    }
                }
                // ---- end of the disabled S75/S78/S79 block -------------------

                // ---- SNAP TURN ---------------------------------------------
                // OUTSIDE the disabled block. Rotates the HEADING, not the
                // stick: feeding a burst of right-stick X asks the GAME to turn
                // you at its own rate, which is smooth turning in a short burst
                // -- the sliding this feature exists to eliminate. Adding to
                // g_aimBase.yaw puts the view somewhere else on the very next
                // frame with nothing in between.
                if (g_cfg.snapTurn)
                {
                    static bool  snapArmed = true;
                    float stx = 0.0f;
                    const bool haveTx = Input_GetTurnX(&stx);

                    if (!haveTx || fabsf(stx) < 0.35f) snapArmed = true;
                    else if (snapArmed && fabsf(stx) > 0.75f)
                    {
                        snapArmed = false;
                        const int step = (int)(g_cfg.snapTurnDeg * 182.0444f);
                        const int signed_ = (stx > 0.0f) ? step : -step;
                        g_aimBase.yaw += signed_;
                        ScriptedManualYaw(signed_, scriptedAim);
                        Log(">>> SNAP TURN: %+.0f deg", (stx > 0.0f)
                            ? g_cfg.snapTurnDeg : -g_cfg.snapTurnDeg);
                    }
                }

                // FREEZE the view while an in-game menu is up. Pause, map,
                // inventory and vending all render the world behind the UI, and
                // having it swing with your head while you read is disorienting.
                // Latch on ENTRY rather than skipping the write, so the view
                // holds where it was instead of snapping to the game's rotation.
                {
                    static FRotator s_menuRot = {};
                    static bool     s_menuHeld = false;
                    if (ViewHeldForUi())
                    {
                        if (!s_menuHeld) { s_menuHeld = true; s_menuRot = finalRot; }
                        finalRot = s_menuRot;
                    }
                    else s_menuHeld = false;
                }

                *CameraRotation = finalRot;
            }

            // Positional tracking: head-frame offset rotated into world XY by
            // the CLEAN yaw (mouse heading), so leaning forward goes into the
            // screen regardless of where the head is turned. UE: fwd=+X, right=+Y.
            if (g_cfg.headPosition)
            {
                // CLEAN yaw only. With head-aim, *CameraRotation CONTAINS head
                // yaw (it comes from Controller.Rotation), so cleanRot is no
                // longer clean -- using it rotates the positional offset by the
                // head turn and sweeps the camera around an arc (§16: walk into
                // a wall, turn your head, drift off it counterclockwise).
                // g_aimBase is the mouse-only heading: it accumulates ONLY
                // game-driven deltas, which is exactly the room frame we want.
                const double cy = UnitsToRad(
                    (g_cfg.headAim && g_aimInit) ? g_aimBase.yaw : cleanRot.yaw);
                const double cs = cos(cy), sn = sin(cy);

                // Hold the head-position offset too. Freezing rotation alone
                // still let you slide the world by leaning. Latch on entry, so
                // the view holds where it was instead of snapping back to the
                // pawn's eye.
                static double mFwd = 0.0, mRight = 0.0, mUp = 0.0;
                static bool   mHeld = false;
                double pf = g_posFwd, pr = g_posRight, pu = g_posUp;
                if (ViewHeldForUi())
                {
                    if (!mHeld) { mHeld = true; mFwd = g_posFwd; mRight = g_posRight; mUp = g_posUp; }
                    pf = mFwd; pr = mRight; pu = mUp;
                }
                else mHeld = false;

                CameraLocation->x += (float)(pf * cs - pr * sn);
                CameraLocation->y += (float)(pf * sn + pr * cs);
                CameraLocation->z += (float)pu;
            }

            // S40: STATURE. The pawn's eye height is authored for a monitor and
            // reads short in VR -- a head below the splicers. Independent of the
            // head-position channel above: that one is your real head moving
            // around a recentred origin, this is a constant offset to the origin
            // itself, so it must apply even with EnableHeadPosition=0.
            //
            // It also fixes the shoulders. Hands.UpdateLocation() anchors the
            // arms to PawnOwner.Location + EyeHeight, NOT to the camera we
            // write -- so raising the camera leaves the arms where they were and
            // they drop relative to your view, which is the other half of the
            // complaint. One knob, both symptoms.
            //
            // The collision capsule does NOT move. At large values you will see
            // through low ceilings before your head bumps them.
            if (g_cfg.heightOffset != 0.0f)
                CameraLocation->z += g_cfg.heightOffset;

            // S57: the hands must sit at ONE world position for both eyes. The
            // per-eye IPD offset below moves the camera +-EyeSeparation, and if
            // the hands are placed relative to THAT they travel with the eye --
            // which cancels their disparity exactly. Result: correct in each eye
            // alone, painted flat onto the world with both open, and read as
            // huge because zero parallax means "very far away".
            g_lastCamCenter = *CameraLocation;   // hands still need this
            double s = (eye == 0 ? -1.0 : 1.0) * (double)g_cfg.eyeSep;
            if (g_cfg.swapEyes) s = -s;

            // Eye offset along the FINAL (head-rotated) right vector (§6).
            const Vec3 right = RotatorRight(finalRot);

            CameraLocation->x += (float)(right.x * s);
            CameraLocation->y += (float)(right.y * s);
            CameraLocation->z += (float)(right.z * s);

            // --- HEAD-AIM WRITE ---
            // g_aimBase was advanced at the top of this block, from the same
            // value the view was composed from. Only the write remains here.
            // M7-S3: THE SUPPRESSION LIVES HERE NOW, not on the block above.
            // Controller.Rotation (+0x1E4) drives the walk direction as well as
            // the view, so writing our heading during a scripted sequence steers
            // it. Skipping just this write is what makes sequences land where
            // they intend -- do not move this condition back up to the read.
            // ---- PUBLISH WHO OWNS THE AIM FIELD -------------------------
            // Set from the two conditions the fork below actually branches on,
            // so it cannot drift from the thing it describes. GAME THREAD
            // writes; the XInput detour reads. See CameraHook_OwnsAimField.
            _InterlockedExchange(&g_ownsAimField,
                (aimField && !scriptedAim) ? 1 : 0);

            // The game owns the field -- nothing of ours is in it, so the two
            // subtractive modes have nothing to subtract. Modes 2 and 3 keep the
            // head's contribution to the view, which is plain head-relative
            // movement and means the same thing whoever wrote the field.
            if (!aimField || scriptedAim)
                PublishWalkRotation(finalRot, g_aimBase, g_aimBase, pThis);

            if (aimField && scriptedAim)
            {
                // The delta above is measured against g_aimLastWrote ("only the
                // game's own change since our last write"). With the write
                // suppressed that reference goes stale and next frame's delta
                // would compound the same rotation again, spinning the view.
                // Re-baseline every frame we decline to write.
                g_aimLastWrote = *aimField;
            }
            else if (aimField)
            {
                FRotator want;
                if (g_cfg.headAimMode <= 0)
                {
                    // LEGACY. Kept only so the artifact can be A/B'd live.
                    want = g_aimBase;
                    want.pitch += (int)(g_headPitch * 182.0444);
                    want.yaw += (int)(g_headYaw * 182.0444);
                }
                else
                {
                    // Motion aim feeds the CONTROLLER direction here while the
                    // view above keeps the head. That split is the whole feature.
                    //
                    // EMPTY HANDS AIM WITH THE HEAD. With nothing equipped there
                    // is no crosshair -- so there is nothing on screen saying
                    // where the controller points, and reaching for the radio at
                    // the start of the game becomes a guess. Looking at a thing
                    // to pick it up is the natural fallback.
                    //
                    // DELIBERATELY THE NARROWEST POSSIBLE CHANGE, because it was
                    // asked for with "don't tie this to anything else": ONLY the
                    // direction written into the aim field moves. The view
                    // composition reads ModeUsesHead() instead, which ignores
                    // the empty-handed case -- so nothing about how the world is
                    // presented changes as you pick a weapon up or put it down.
                    //
                    // Not extended to scripted sequences even though the
                    // crosshair is hidden there too: M7 already releases the aim
                    // in that window on purpose, and reaching into it for a
                    // convenience is how a working thing gets broken.
                    const bool handAim = !AimUsesHeadNow() && g_aimHandValid;

                    const double aimY = handAim ? g_aimHandYaw : g_headYaw;
                    const double aimP = handAim ? g_aimHandPitch : g_headPitch;

                    want = ComposeHeadLocal(g_aimBase, aimY, aimP,
                        g_cfg.headAimMode >= 2);
                    // The controller rotator cannot carry head roll (S6); roll
                    // still reaches the view through the compose above.
                    want.roll = g_aimBase.roll;
                }

                // The crosshair reads THIS -- the aim we are about to write,
                // against the view we just composed. Both final, both from the
                // same frame.
                PublishShotDir(finalRot, want);

                // And so does locomotion, for exactly the same reason. `want` is
                // the post-composition yaw the walk direction will be measured
                // from; deriving it a second time on the other thread is what
                // left the residual coupling.
                PublishWalkRotation(finalRot, want, g_aimBase, pThis);

                // S74: do NOT write during a cutscene. Always-writing latches
                // the detector ON forever.
                if (!aimNowCut)
                {
                    *aimField = want;
                    g_aimLastWrote = want;
                }
                else
                {
                    g_aimLastWrote = *aimField;
                }
            }

            // Hands last: the camera is final by now, and the hands are
            // positioned relative to it.
            g_lastCleanYaw = (double)cleanRot.yaw;
            DriveHands(g_lastCamCenter, g_lastHeadPos);
            DriveQuestArrow(g_lastCamCenter);

            if (eye == 0) ++g_wLeft; else ++g_wRight;

            if (!g_armLogged)
            {
                g_armLogged = true;
                Log(">>> CAMERA WRITE ARMED. site%d (ret mod+0x%X)  halfIPD %.2f units  swap=%d",
                    leader, (unsigned)((uint8_t*)g_sites[leader].ret - g_modBase),
                    g_cfg.eyeSep, (int)g_cfg.swapEyes);
                Log("camera:   right vec %.3f %.3f %.3f   s=%+.2f (eye %d)",
                    right.x, right.y, right.z, s, eye);
            }
        }
    }

    // Publish the tag AFTER the write, so the frame and its tag are consistent.
    g_eyeQ[w & 63] = (unsigned char)eye;
    MemoryBarrier();
    _InterlockedIncrement(&g_eyeWr);
    g_lastPushTick = (long)GetTickCount();
    if (!g_lastPushTick) g_lastPushTick = 1;   // 0 is reserved for "never"

    // --- heartbeat, once a second. NEVER per-frame. ---
    DWORD now = GetTickCount();
    if (now - g_lastTick >= 1000)
    {
        g_lastTick = now;
        const double injDeg = g_aimGameDPitch / 182.0444;
        Log("  MOUSE-Y: game injected %.1f deg of pitch this second   (0.0 == mouse Y is dead)",
            injDeg);
        GameState_PitchSample(injDeg);
        g_aimGameDPitch = 0.0;

        Log("--- camera: %llu calls, %d site(s), leader=site%d | writes L=%llu R=%llu ---",
            g_calls, g_siteCount, g_leader, g_wLeft, g_wRight);
        Log("  HEAD: yaw%7.1f  pitch%7.1f  roll%7.1f  deg   %s",
            g_headYaw, g_headPitch, g_headRoll,
            g_cfg.headTracking ? "(WRITTEN to camera)" : "(computed, not written)");
        Log("  INTEREYE: avg%7.2f  max%7.2f  units   clamp=%d %s",
            g_ieN ? (g_ieSum / (double)g_ieN) : 0.0, g_ieMax,
            (int)g_cfg.deltaClamp, g_targetLocked ? "(locked)" : "(NOT locked)");
        g_ieSum = 0.0; g_ieMax = 0.0; g_ieN = 0;
        Log("  DELTA: FrameDelta min %.5f  max %.5f   clamp=%d",
            (g_fdMinBits == 0xFFFFFFFFu) ? 0.0f : *(const float*)&g_fdMinBits,
            * (const float*)&g_fdMaxBits, (int)g_cfg.deltaClamp);
            g_fdMinBits = 0xFFFFFFFFu; g_fdMaxBits = 0;
        Log("  POS : right%7.1f%s  up%7.1f%s  fwd%7.1f%s  cm   %s",
            g_posRight, (fabs(g_posRight) >= kPosSide - 0.05) ? "*" : " ",
            g_posUp, (g_posUp >= kPosUpMax - 0.05 ||
                g_posUp <= -kPosDownMax + 0.05) ? "*" : " ",
            g_posFwd, (g_posFwd >= kPosFwdMax - 0.05 ||
                g_posFwd <= -kPosBackMax + 0.05) ? "*" : " ",
            g_cfg.headPosition ? "(WRITTEN)" : "(computed, not written)");

        for (int i = 0; i < g_siteCount; ++i)
        {
            const CallSite& s2 = g_sites[i];
            Log("  %s%d mod+0x%-7X n=%-8llu pos %9.1f %9.1f %9.1f   p%7.1f y%7.1f r%7.1f",
                (i == g_leader) ? "*site" : " site", i,
                (unsigned)((uint8_t*)s2.ret - g_modBase), s2.count,
                s2.loc.x, s2.loc.y, s2.loc.z,
                UnitsToDeg(s2.rot.pitch), UnitsToDeg(s2.rot.yaw), UnitsToDeg(s2.rot.roll));
        }
    }
}

// ---------------------------------------------------------------- the FIFO API

int CameraHook_NextEye()
{
    const long wr = g_eyeWr;      // volatile read
    long rd = g_eyeRd;
    long depth = wr - rd;

    if (depth <= 0)
    {
        // No camera tag waiting: menu, loading screen, movie. Keep alternating
        // so the compositor never stalls.
        _InterlockedIncrement(&g_underruns);
        g_needResync = 1;         // tag<->frame alignment is now unknown
        g_deepPops = 0;
        if (g_qMin > 0) g_qMin = 0;
        if (g_qMax < 0) g_qMax = 0;
        g_lastEye ^= 1;
        return g_lastEye;
    }

    // Depth should be EXACTLY 1 in steady state (measured, §6/§13). Two ways
    // it goes stale:
    //   1. after an UNDERRUN period (menus/tutorials) -- the §13 bug.
    //   2. DRIFT with no underrun at all: a producer burst during a streaming
    //      hitch leaves an extra tag queued forever. No underrun ever fires,
    //      so the §13 resync never arms -- every tag is then one frame stale,
    //      which SWAPS the eyes persistently. (Movement + fast turns == the
    //      hitchy case. Pair-lock made the resulting inverted stereo blatant.)
    // Both collapse to the same cure: jump to the NEWEST tag.
    if (depth > 1) ++g_deepPops; else g_deepPops = 0;

    if (g_needResync || g_deepPops >= 8)
    {
        if (depth > 1)
        {
            Log("camera: EYEQ RESYNC (%s) -- dropped %ld stale tag(s)",
                g_needResync ? "post-underrun" : "DRIFT", depth - 1);
            rd = wr - 1;
            g_eyeRd = rd;
            depth = 1;
        }
        g_needResync = 0;
        g_deepPops = 0;
    }

    if (depth > 32)               // producer ran far ahead (a stall): take newest
    {
        rd = wr - 1;
        g_eyeRd = rd;
        depth = 1;
    }

    if (depth < g_qMin) g_qMin = depth;
    if (depth > g_qMax) g_qMax = depth;

    const int eye = (int)g_eyeQ[rd & 63];
    _InterlockedIncrement(&g_eyeRd);
    g_lastEye = eye;
    return eye;
}

bool CameraHook_GetLatchedPose(float quat[4], float pos[3])
{
    for (;;)
    {
        const long s0 = g_lpSeq;
        if (s0 & 1) continue;               // writer mid-update
        MemoryBarrier();
        const long valid = g_lpValid;
        quat[0] = g_lpQuat[0]; quat[1] = g_lpQuat[1];
        quat[2] = g_lpQuat[2]; quat[3] = g_lpQuat[3];
        pos[0] = g_lpPos[0]; pos[1] = g_lpPos[1]; pos[2] = g_lpPos[2];
        MemoryBarrier();
        if (s0 == g_lpSeq) return valid != 0;
    }
}

// TRUE when the camera hook hasn't produced a view for >250ms: menu, loading,
// movie, or pre-level. The trigger for the menu quad-screen.
bool CameraHook_Starved()
{
    const long t = g_lastPushTick;
    if (!t) return true;                       // camera never ticked yet
    return (GetTickCount() - (DWORD)t) > 250;
}

void CameraHook_EyeQueueStats(int* minDepth, int* maxDepth, unsigned* underruns)
{
    if (minDepth)  *minDepth = (g_qMin == 0x7FFFFFFF) ? -1 : (int)g_qMin;
    if (maxDepth)  *maxDepth = (int)g_qMax;
    if (underruns) *underruns = (unsigned)g_underruns;
    g_qMin = 0x7FFFFFFF;
    g_qMax = -1;
}

// TRUE when no camera view has been produced for >250ms (menu/loading/movie).
bool CameraHook_Starved();

// ---------------------------------------------------------------- delta scan
//
// FINDING THE FRAME-DELTA FUNCTION WITHOUT A FIXED OFFSET.
//
// kDelta_FnOff was measured on the Steam build. MEASURED: the Epic Games Store
// build has the same code at DIFFERENT addresses -- module+0x53D850 there lands
// in the middle of an unrelated instruction, so the prologue check refused and
// DeltaClamp was silently off for every Epic player.
//
// So the offset is now only a FIRST GUESS. If the prologue is there, nothing
// changes and the scan never runs -- the shipping Steam path is untouched. If
// it is not, we search executable module memory for the function's own bytes.
//
// The four bytes at index 6..9 are the absolute address pushed for the SEH
// scope table. That value lives in the data section, which moves between
// builds, so it is WILDCARDED. Everything else is register/stack code.
static const uint8_t kDeltaSig[] = {
    0x55,0x8B,0xEC,0x6A,0xFF,0x68,0x00,0x00,0x00,0x00,0x64,0xA1,0x00,0x00,0x00,
    0x00,0x50,0x64,0x89,0x25,0x00,0x00,0x00,0x00,0x83,0xEC,0x44,0x53,0x56,0x8B,
    0xF1,0xC7,0x45,0xE4,0x00,0x00,0x00,0x00,0x57,0x89,0x75,0xDC,0x8B,0x46,0x44,
    0x8B,0x38
};
// 'x' == must match, '?' == wildcard. Must be the same length as kDeltaSig.
static const char kDeltaMask[] =
"xxxxxx" "????" "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
static_assert(sizeof(kDeltaMask) - 1 == sizeof(kDeltaSig),
    "kDeltaMask must have exactly one character per kDeltaSig byte");

// EXACTLY ONE match, or nothing. Zero or several means the signature stopped
// being discriminating on that build, and hooking the wrong function here
// corrupts the simulation clock -- so we refuse, the same way the prologue
// check already does.
static void* FindDeltaFn()
{
    std::vector<Region> regs;
    EnumReadableRegions(regs);

    const size_t n = sizeof(kDeltaSig);
    void* hit = nullptr;
    int   hits = 0;

    for (const Region& r : regs)
    {
        if (r.size < n) continue;
        if (!IsExecutable(r.base)) continue;

        uint8_t* e = r.base + r.size - n;
        for (uint8_t* p = r.base; p <= e; ++p)
        {
            size_t i = 0;
            for (; i < n; ++i)
                if (kDeltaMask[i] == 'x' && p[i] != kDeltaSig[i]) break;
            if (i != n) continue;

            ++hits;
            if (!hit) hit = (void*)p;
            if (hits > 1) break;
        }
        if (hits > 1) break;
    }

    if (hits == 1)
    {
        Log(">>> delta: signature scan found ONE match at module+0x%X",
            (unsigned)((uint8_t*)hit - g_modBase));
        return hit;
    }
    Log("!!! delta: signature scan found %d matches. Refusing to hook.", hits);
    return nullptr;
}

// The fixed offset first, the scan only as a fallback.
static void* ResolveDeltaFn()
{
    uint8_t* guess = g_modBase + kDelta_FnOff;
    if (IsMemoryValid(guess, 5) && guess[0] == 0x55 && guess[1] == 0x8B &&
        guess[2] == 0xEC && guess[3] == 0x6A && guess[4] == 0xFF)
    {
        Log("delta: module+0x%X still looks right. No scan needed.", kDelta_FnOff);
        return (void*)guess;
    }

    Log("delta: module+0x%X is NOT the function on this build -- expected on", kDelta_FnOff);
    Log("delta: non-Steam builds. Scanning for the signature instead...");

    const DWORD t0 = GetTickCount();
    void* found = FindDeltaFn();
    Log("delta: scan took %lu ms", GetTickCount() - t0);
    return found;      // null == refuse; the prologue check below refuses too
}

// ---------------------------------------------------------------- install

bool CameraHook_Install()
{
    HMODULE h = GetModuleHandleA(nullptr);
    MODULEINFO mi = {};
    if (!h || !GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi)))
    {
        Log("camera: GetModuleInformation FAILED. No hook.");
        return false;
    }
    g_modBase = (uint8_t*)mi.lpBaseOfDll;
    g_modSize = mi.SizeOfImage;
    Log("camera: BioshockHD.exe base 0x%08X  size 0x%08X (%.1f MB)",
        (unsigned)(uintptr_t)g_modBase, (unsigned)g_modSize, g_modSize / 1048576.0);

    DWORD t0 = GetTickCount();
    void* fn = FindCalcView();
    Log("camera: search took %lu ms", GetTickCount() - t0);

    if (!fn) return false;     // §6a: any stage fails -> install NOTHING

    g_fnAddr = fn;

    MH_STATUS s = MH_CreateHook(fn, &hkCalcView, (LPVOID*)&g_orig);
    if (s != MH_OK) { Log("camera: MH_CreateHook -> %d. No hook.", (int)s); g_fnAddr = nullptr; return false; }

    s = MH_EnableHook(fn);
    if (s != MH_OK) { Log("camera: MH_EnableHook -> %d. No hook.", (int)s); g_fnAddr = nullptr; return false; }

    // Separate hook, separate failure. If this doesn't take, the camera still
    // works and the mod runs exactly as it does today.
    if (g_cfg.deltaClamp)
    {
        void* dfn = ResolveDeltaFn();
        const uint8_t* pb = (const uint8_t*)dfn;

        // DIAGNOSTIC: dump what is actually at this address on THIS build.
        // On the build the offset was derived from, this is the reference
        // signature -- it becomes a pattern scan so the offset stops being
        // build-specific. On any other build it shows what is there instead.
        // Read-only; delete once the scan replaces the fixed offset.
        if (IsMemoryValid(pb, 48))
        {
            char hex[160] = {};
            for (int i = 0; i < 48; ++i)
                _snprintf_s(hex + i * 3, 4, _TRUNCATE, "%02X ", pb[i]);
            Log("delta: bytes at module+0x%X: %s",
                (unsigned)(pb - g_modBase), hex);
        }
        else
        {
            Log("delta: module+0x%X is not readable on this build.", kDelta_FnOff);
        }

        // Prologue confirmed in phase 10: 55 8B EC 6A FF. If this build differs,
        // REFUSE -- hooking the wrong address here corrupts the sim clock.
        if (IsMemoryValid(pb, 5) && pb[0] == 0x55 && pb[1] == 0x8B &&
            pb[2] == 0xEC && pb[3] == 0x6A && pb[4] == 0xFF)
        {
            if (MH_CreateHook(dfn, &hkDelta, (LPVOID*)&g_origDelta) == MH_OK &&
                MH_EnableHook(dfn) == MH_OK)
            {
                g_deltaFnAddr = dfn;
                Log(">>> DELTA HOOK ARMED at module+0x%X (one world advance per eye pair)",
                    (unsigned)((uint8_t*)dfn - g_modBase));
            }
            else Log("!!! delta: MinHook failed at module+0x%X. Clamp OFF.", kDelta_FnOff);
        }
        else
        {
            Log("!!! delta: prologue MISMATCH at module+0x%X (expected 55 8B EC 6A FF).",
                kDelta_FnOff);
            Log("!!! delta: wrong build or the offset moved. NOTHING hooked.");
        }
    }
    else Log("delta: DeltaClamp=0. One advance per EYE -- fast scenes will double.");

    Log(">>> CAMERA HOOK ARMED (write=%d). Load a level and move.", (int)g_cfg.cameraWrite);
    return true;
}

void CameraHook_Remove()
{
    if (g_fnAddr) { MH_DisableHook(g_fnAddr); g_fnAddr = nullptr; }
    if (g_deltaFnAddr) { MH_DisableHook(g_deltaFnAddr); g_deltaFnAddr = nullptr; }
}