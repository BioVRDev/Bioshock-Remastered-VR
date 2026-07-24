// BioshockVR/CameraHook.cpp
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

#include "CameraHook.h"
#include "GameState.h"
#include "InputHook.h"
#include "HandsProbe.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>
#include <intrin.h>     // _ReturnAddress, _InterlockedIncrement

#include <MinHook.h>

#pragma comment(lib, "psapi.lib")

extern void LogFile(const char* msg);

// From dllmain.cpp / BioshockVR.ini
extern bool  g_cfgCameraWrite;   // default 0 -- the Phase 6 kill switch
extern float g_cfgEyeSep;        // half-IPD in game units (== cm). default 3.2
extern bool  g_cfgSwapEyes;      // 1 == invert the eye polarity
extern void  XR_GetHeadQuat(float out[4]);   // from XRSession.cpp (render thread)
extern bool  g_cfgHeadTracking;   // EnableHeadTracking kill switch (dllmain.cpp)
extern void  XR_GetHeadPos(float out[3]);
extern bool  g_cfgHeadPosition;   // EnableHeadPosition kill switch (dllmain.cpp)
extern int   g_cfgDeltaClamp;     // 0 off, 1 player world, 2 both (dllmain.cpp)
extern int   g_cfgHeadAimMode;    // 0 legacy additive, 1 local compose, 2 pitch-decoupled
extern bool  g_cfgPairLock;
extern bool  g_cfgHeadAim;
extern bool g_cfgHeadRoll;
extern int   g_cfgAimSource;     // 0 head, 1 right controller
extern float g_cfgAimClampDeg;   // max angle between aim and view
extern float g_cfgAimSmooth;     // 0 none .. 0.95 heavy
extern bool g_cfg6DofHands;   // Enable6DofHands
extern float g_cfgHandsGrip[3];   // HandsGripOffset: fwd, right, up (cm)
extern float g_cfgHandsScale;   // HandsScale, DrawScale for the hands

bool GameState_Cutscene();   // GameState.cpp
bool GameState_Paused();     // GameState.cpp
void GameState_PitchSample(double degThisSecond);   // GameState.cpp
bool DrawHook_MenuUp();   // DrawHook.cpp

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
extern float g_cfgHeightOffset;   // CameraHeightOffset, cm
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

volatile long g_vqCount = 0;   // VirtualQuery calls, drained by the heartbeat

static bool IsMemoryValid(const void* addr, size_t size)
{
    if (!addr || !size) return false;
    _InterlockedIncrement(&g_vqCount);

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
static int      g_aimCand = 0;      // Numpad + cycles the ROTSCAN candidates
static const unsigned kAimOffsets[2] = { 0x1E4, 0x328 };

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
    if (!g_cfg6DofHands || !g_hwValid || !g_hwObj) return;
    if (GameState_Paused()) return;   // render-thread half of the same freeze

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
static bool g_fdChecked = false, g_fdOk = false;   // validate the page once

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
            g_fdChecked = false; g_fdOk = false;
            Log(">>> DELTA: world objects changed (0x%08X). Carries reset, re-locking.",
                self);
        }
    }

    uint32_t passDelta = deltaBits;

    if (g_cfgDeltaClamp)
    {
        const bool isTarget = (g_targetLocked && self == g_targetObj);
        // Mode 2: clamp BOTH delta-receiving objects. FrameDelta proves the
        // player world is already freezing, yet the camera still moves 1.2
        // units between eyes -- so whatever carries the bathysphere is being
        // ticked by the other one.
        const bool doClamp = (g_cfgDeltaClamp == 2) ? true : isTarget;

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
    if (g_targetLocked && self == g_targetObj)
    {
        // Validate ONCE per world, not 235 times a second. VirtualQuery is a
        // kernel transition and this sits in the engine's hottest function.
        if (!g_fdChecked)
        {
            g_fdChecked = true;
            g_fdOk = IsMemoryValid((const char*)thisPtr + 0xC8, 4);
        }
        if (g_fdOk)
        {
            const uint32_t stored = *(const uint32_t*)((const char*)thisPtr + 0xC8);
            if (stored < g_fdMinBits) g_fdMinBits = stored;
            if (stored > g_fdMaxBits) g_fdMaxBits = stored;
        }
    }

    return ret;
}

static void DriveHands(const FVector& camLoc, const float headPos[3])
{
    if (!g_cfg6DofHands) return;
    if (GameState_Cutscene()) return;
    if (GameState_Paused()) return;   // hands hold still behind a menu

    void* obj = nullptr; unsigned locOff = 0, rotOff = 0;
    if (!HandsProbe_GetTargets(&obj, &locOff, &rotOff)) return;

    HandPose hp = {};
    if (!Input_GetHandPose(HAND_RIGHT, &hp)) return;
    if (!hp.aimValid && !hp.gripValid) return;

    if (!IsMemoryWritable((uint8_t*)obj + locOff, sizeof(FVector))) return;
    if (!IsMemoryWritable((uint8_t*)obj + rotOff, sizeof(FRotator))) return;

    // ---- rotation: yaw and pitch only ------------------------------------
    double cp, cy, cr;
    HeadQuatToDeg(hp.aimQuat, cp, cy, cr);

    FRotator want = ComposeHeadLocal(g_aimBase, cy, cp, g_cfgHeadAimMode >= 2);
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
        (g_cfgHeadAim && g_aimInit) ? g_aimBase.yaw : g_lastCleanYaw);
    const double cs = cos(roomYaw), sn = sin(roomYaw);

    double wx = camLoc.x + (relFwd * cs - relRight * sn);
    double wy = camLoc.y + (relFwd * sn + relRight * cs);
    double wz = camLoc.z + relUp;

    // The Hands actor origin sits at the EYE (PlayerViewOffset is 0,0,0), with
    // the arm authored extending forward and down from there. Subtract where the
    // hand sits in mesh space, rotated by the orientation that will ACTUALLY be
    // rendered -- which is `want`, now that we no longer ask for a roll the game
    // refuses to keep.
    if (g_cfgHandsGrip[0] || g_cfgHandsGrip[1] || g_cfgHandsGrip[2])
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

        const double gX = g_cfgHandsGrip[0];
        const double gY = g_cfgHandsGrip[1];
        const double gZ = g_cfgHandsGrip[2];

        wx -= (Fx * gX + Rx * gY + Ux * gZ);
        wy -= (Fy * gX + Ry * gY + Uy * gZ);
        wz -= (Fz * gX + Rz * gY + Uz * gZ);
    }

    FVector* L = (FVector*)((uint8_t*)obj + locOff);
    L->x = (float)wx;
    L->y = (float)wy;
    L->z = (float)wz;

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

    // The game's own UI state, read off the controller we already have in hand.
    GameState_Observe(pThis);
    HandsProbe_Observe(pThis, (const float*)CameraLocation, (const int*)CameraRotation);

    void* ret = _ReturnAddress();

    if (g_calls == 0)
    {
        Log(">>> CAMERA HOOK FIRED. First call.");
        Log("camera:   pThis  = 0x%08X  (APlayerController*)", (unsigned)(uintptr_t)pThis);
        Log("camera:   thread = %lu   (GAME thread -- Present is on a DIFFERENT one)",
            GetCurrentThreadId());
        Log("camera:   write  = %s", g_cfgCameraWrite ? "ENABLED (EnableCameraWrite=1)"
            : "disabled (EnableCameraWrite=0)");
        g_lastTick = GetTickCount();
    }
    ++g_calls;

    if (!CameraLocation || !CameraRotation) return;
    if (!IsMemoryValid(CameraLocation, sizeof(FVector)))  return;
    if (!IsMemoryValid(CameraRotation, sizeof(FRotator))) return;

    // Numpad 9: one-shot rotation scan. Rotation here is still CLEAN -- the
    // head compose happens further down, at kArmAfterCalls.
    {
        static bool k9 = false;
        const bool down = (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0;
        if (down && !k9) ScanForRotation(pThis, *CameraRotation);
        k9 = down;
    }

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

    // PHASE 10a: which delta-receiving object does the RENDER view's controller
    // reach? That one is the player world. Throttled -- this is a scan, and it
    // stops permanently the moment it locks (rule 1, section 11).
    if (g_cfgDeltaClamp && !g_targetLocked && (g_deltaObjA || g_deltaObjB))
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
        g_pairRot = *CameraRotation;
        g_pairLoc = *CameraLocation;
        g_pairValid = true;
    }
    else if (g_cfgPairLock && g_pairValid &&
        g_cfgCameraWrite && g_calls >= kArmAfterCalls)
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
        if (g_cfgAimSource == 1)
        {
            HandPose hpose = {};
            if (Input_GetHandPose(HAND_RIGHT, &hpose) && hpose.aimValid)
            {
                double ap, ay, ar;
                HeadQuatToDeg(hpose.aimQuat, ap, ay, ar);

                double dY = WrapDeg180(ay - g_headYaw);
                double dP = ap - g_headPitch;

                const double c = (double)g_cfgAimClampDeg;
                if (dY > c) dY = c;   if (dY < -c) dY = -c;
                if (dP > c) dP = c;   if (dP < -c) dP = -c;

                // Smooth the OFFSET, not the absolute angle -- so head motion
                // stays instant and only hand tremor gets damped.
                const double a = (double)g_cfgAimSmooth;
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
                if (g_cfgHeadPosition)
                {
                    px += (float)(g_posRight * 0.01);   // cm -> m, XR +x right
                    py += (float)(g_posUp * 0.01);   //           XR +y up
                    pz -= (float)(g_posFwd * 0.01);   //           XR +z BACK
                }
            }
            else { px = hp[0]; py = hp[1]; pz = hp[2]; }

            const bool applied = g_cfgCameraWrite && g_cfgHeadTracking &&
                (g_calls >= kArmAfterCalls);

            // Which pose did the image ACTUALLY render from?
            // S41: motion aim composes the view from the CURRENT head quat and
            // writes it outright, so there is no frame of indirection left to
            // compensate for. Publishing the previous pose here made the
            // compositor reproject with a pose the image was not rendered from
            // -- the S2 flicker, reopened backwards. Head-only, because
            // g_prevQuat is a head quaternion.
            const bool lag = (g_cfgHeadAim && g_cfgAimSource != 1 && g_prevQuatValid);
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

    // --- THE WRITE (§6e). Only when armed. ---
    if (g_cfgCameraWrite && g_calls >= kArmAfterCalls)
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
            FRotator finalRot = cleanRot;
            if (g_cfgHeadTracking)
            {
                if (g_cfgHeadAim && g_cfgAimSource == 1 && g_aimInit)
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
                        g_cfgHeadAimMode >= 2);
                    finalRot.roll = g_aimBase.roll;
                    finalRot = ApplyWorldSpaceYaw(finalRot, 0.0, 0.0,
                        g_cfgHeadRoll ? g_headRoll : 0.0);
                }
                else if (g_cfgHeadAim)
                {
                    // Head yaw/pitch already reached the view THROUGH the aim
                    // field (+0x1E4 -> Controller.Rotation -> CameraRotation).
                    // Adding them here too applies the head TWICE and the view
                    // swims as if the mouse were being dragged. Roll only.
                    finalRot = ApplyWorldSpaceYaw(*CameraRotation,
                        0.0, 0.0, g_cfgHeadRoll ? g_headRoll : 0.0);
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
                    const int b = (g_cfgHeadAim && g_cfgAimSource == 1 && g_aimInit) ? 2
                        : (g_cfgHeadAim ? 1 : 0);
                    if (b != lastBranch)
                    {
                        lastBranch = b;
                        Log(">>> VIEW PATH: %s",
                            b == 2 ? "motion aim (view=head, aim=controller)"
                            : b == 1 ? "head-aim (view inherited from aim field, roll only)"
                            : "legacy additive (head applied to view directly)");
                    }
                }

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

                // S83: FREEZE the view while an in-game menu is up. Pause, map,
                // inventory and vending all render the world behind the UI, and
                // having it swing with your head while you read a menu is
                // disorienting. Latch on entry rather than skipping the write,
                // so the view holds where it was instead of snapping to the
                // game's rotation.
                {
                    static FRotator s_menuRot = {};
                    static bool     s_menuHeld = false;
                    if (GameState_Paused())
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
            if (g_cfgHeadPosition)
            {
                // CLEAN yaw only. With head-aim, *CameraRotation CONTAINS head
                // yaw (it comes from Controller.Rotation), so cleanRot is no
                // longer clean -- using it rotates the positional offset by the
                // head turn and sweeps the camera around an arc (§16: walk into
                // a wall, turn your head, drift off it counterclockwise).
                // g_aimBase is the mouse-only heading: it accumulates ONLY
                // game-driven deltas, which is exactly the room frame we want.
                const double cy = UnitsToRad(
                    (g_cfgHeadAim && g_aimInit) ? g_aimBase.yaw : cleanRot.yaw);
                const double cs = cos(cy), sn = sin(cy);
                // S85: hold the head-position offset too while a menu is up.
                // Freezing rotation alone still let you slide the world by
                // leaning. Latch on entry rather than zeroing, so the view holds
                // where it was instead of snapping back to the pawn's eye.
                static double mFwd = 0.0, mRight = 0.0, mUp = 0.0;
                static bool   mHeld = false;
                double pf = g_posFwd, pr = g_posRight, pu = g_posUp;
                if (GameState_Paused())
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
            if (g_cfgHeightOffset != 0.0f)
                CameraLocation->z += g_cfgHeightOffset;

            // S57: the hands must sit at ONE world position for both eyes. The
            // per-eye IPD offset below moves the camera +-EyeSeparation, and if
            // the hands are placed relative to THAT they travel with the eye --
            // which cancels their disparity exactly. Result: correct in each eye
            // alone, painted flat onto the world with both open, and read as
            // huge because zero parallax means "very far away".
            g_lastCamCenter = *CameraLocation;
            double s = (eye == 0 ? -1.0 : 1.0) * (double)g_cfgEyeSep;
            if (g_cfgSwapEyes) s = -s;

            // Eye offset along the FINAL (head-rotated) right vector (§6).
            const Vec3 right = RotatorRight(finalRot);

            CameraLocation->x += (float)(right.x * s);
            CameraLocation->y += (float)(right.y * s);
            CameraLocation->z += (float)(right.z * s);

            // --- HEAD-AIM WRITE ---
            if (g_cfgHeadAim && g_cfgHeadTracking &&
                !DrawHook_MenuUp() && !CameraHook_Starved())
            {
                unsigned off = kAimOffsets[g_aimCand & 1];
                FRotator* aim = (FRotator*)((uint8_t*)pThis + off);

                if (IsMemoryWritable(aim, sizeof(FRotator)))
                {
                    // Re-arm the heading the frame a cutscene ends, so the view
                    // resumes from wherever the game left you facing instead of
                    // snapping by the whole cutscene's accumulated slew.
                    static bool s_wasCut = false;
                    const bool s_nowCut = GameState_Cutscene();
                    if (s_wasCut && !s_nowCut) g_aimInit = false;
                    s_wasCut = s_nowCut;

                    if (!g_aimInit)
                    {
                        g_aimBase = *aim;
                        g_aimLastWrote = *aim;
                        g_aimInit = true;
                        Log(">>> HEAD-AIM armed on +0x%X", off);
                    }
                    else
                    {
                        // Only the GAME's own change since our last write.
                        const int dP = RotDelta(aim->pitch, g_aimLastWrote.pitch);
                        const int dY = RotDelta(aim->yaw, g_aimLastWrote.yaw);
                        const int dR = RotDelta(aim->roll, g_aimLastWrote.roll);

                        g_aimGameDPitch += fabs((double)dP);
                        g_gameDYaw = dY;        // S77, read by the turn gate

                        // CUTSCENE HEAD-OVERRIDE: during a scripted camera the game
                        // slews aim to point the view where the script wants;
                        // accumulating that swings the whole world around your
                        // locked head (nausea). Freeze the world heading so ONLY
                        // the head rotates the view. Position still follows the
                        // script, so you ride the path.
                        if (!s_nowCut)
                        {
                            g_aimBase.pitch += dP;
                            g_aimBase.yaw += dY;
                            g_aimBase.roll += dR;
                        }
                    }

                    FRotator want;
                    if (g_cfgHeadAimMode <= 0)
                    {
                        // LEGACY. Kept only so the artifact can be A/B'd live.
                        want = g_aimBase;
                        want.pitch += (int)(g_headPitch * 182.0444);
                        want.yaw += (int)(g_headYaw * 182.0444);
                    }
                    else
                    {
                        // Motion aim feeds the CONTROLLER direction here while
                        // the view above keeps the head. That split is the whole
                        // feature.
                        const double aimY = (g_cfgAimSource == 1 && g_aimHandValid)
                            ? g_aimHandYaw : g_headYaw;
                        const double aimP = (g_cfgAimSource == 1 && g_aimHandValid)
                            ? g_aimHandPitch : g_headPitch;

                        want = ComposeHeadLocal(g_aimBase, aimY, aimP,
                            g_cfgHeadAimMode >= 2);
                        // The controller rotator cannot carry head roll (S6);
                        // roll still reaches the view through the compose above.
                        want.roll = g_aimBase.roll;
                    }

                    // S74: back to NOT writing during a cutscene. Always-writing
                    // latches the detector ON forever: we freeze g_aimBase, you
                    // push the stick, we overwrite it, and that disagreement is
                    // exactly what the detector measures. It pinned your facing
                    // and killed the projector trigger. The 2-second flag this
                    // restores is wrong but harmless; the real fix is reading the
                    // game's own scripted-sequence state, not this heuristic.
                    if (!s_nowCut)
                    {
                        *aim = want;
                        g_aimLastWrote = want;
                    }
                    else
                    {
                        g_aimLastWrote = *aim;
                    }
                }
            }

            // Hands last: the camera is final by now, and the hands are
            // positioned relative to it.
            g_lastCleanYaw = (double)cleanRot.yaw;
            DriveHands(g_lastCamCenter, g_lastHeadPos);

            if (eye == 0) ++g_wLeft; else ++g_wRight;

            if (!g_armLogged)
            {
                g_armLogged = true;
                Log(">>> CAMERA WRITE ARMED. site%d (ret mod+0x%X)  halfIPD %.2f units  swap=%d",
                    leader, (unsigned)((uint8_t*)g_sites[leader].ret - g_modBase),
                    g_cfgEyeSep, (int)g_cfgSwapEyes);
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
            g_cfgHeadTracking ? "(WRITTEN to camera)" : "(computed, not written)");
        Log("  INTEREYE: avg%7.2f  max%7.2f  units   clamp=%d %s",
            g_ieN ? (g_ieSum / (double)g_ieN) : 0.0, g_ieMax,
            (int)g_cfgDeltaClamp, g_targetLocked ? "(locked)" : "(NOT locked)");
        g_ieSum = 0.0; g_ieMax = 0.0; g_ieN = 0;
        Log("  DELTA: FrameDelta min %.5f  max %.5f   clamp=%d",
            (g_fdMinBits == 0xFFFFFFFFu) ? 0.0f : *(const float*)&g_fdMinBits,
            * (const float*)&g_fdMaxBits, (int)g_cfgDeltaClamp);
            g_fdMinBits = 0xFFFFFFFFu; g_fdMaxBits = 0;
        Log("  VQUERY: %ld VirtualQuery calls in the last second",
            _InterlockedExchange(&g_vqCount, 0));
        Log("  POS : right%7.1f%s  up%7.1f%s  fwd%7.1f%s  cm   %s",
            g_posRight, (fabs(g_posRight) >= kPosSide - 0.05) ? "*" : " ",
            g_posUp, (g_posUp >= kPosUpMax - 0.05 ||
                g_posUp <= -kPosDownMax + 0.05) ? "*" : " ",
            g_posFwd, (g_posFwd >= kPosFwdMax - 0.05 ||
                g_posFwd <= -kPosBackMax + 0.05) ? "*" : " ",
            g_cfgHeadPosition ? "(WRITTEN)" : "(computed, not written)");

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
    if (g_cfgDeltaClamp)
    {
        void* dfn = (void*)(g_modBase + kDelta_FnOff);
        const uint8_t* pb = (const uint8_t*)dfn;
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
                    kDelta_FnOff);
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

    Log(">>> CAMERA HOOK ARMED (write=%d). Load a level and move.", (int)g_cfgCameraWrite);
    return true;
}

void CameraHook_Remove()
{
    if (g_fnAddr) { MH_DisableHook(g_fnAddr); g_fnAddr = nullptr; }
    if (g_deltaFnAddr) { MH_DisableHook(g_deltaFnAddr); g_deltaFnAddr = nullptr; }
}