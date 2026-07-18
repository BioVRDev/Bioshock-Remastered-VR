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

static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

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

static const uint64_t kArmAfterCalls = 200;   // let the leader settle before writing

static void __fastcall hkCalcView(void* pThis, void* edx,
    void** ViewActor,
    FVector* CameraLocation,
    FRotator* CameraRotation)
{
    // 1. Call the ORIGINAL first. It rebuilds *CameraRotation from scratch every
    //    call (§6c), so what arrives here is always CLEAN. Absolute offset, no
    //    accumulator, no drift.
    g_orig(pThis, edx, ViewActor, CameraLocation, CameraRotation);

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

    // --- TAG THIS FRAME'S EYE and push it to Present. The eye is owned HERE,
    //     on the game thread, by strict alternation of the producer index. It
    //     rides the FIFO out to the render thread with the frame it belongs to. ---
    const long w = g_eyeWr;
    const int  eye = (int)(w & 1);          // 0 == LEFT, 1 == RIGHT

    // Latch the HMD pose ONCE per pair (LEFT frame), hold for both eyes so the
    // two eyes never render from different head rotations (§6).
    if (eye == 0)
    {
        float hq[4];
        XR_GetHeadQuat(hq);
        HeadQuatToDeg(hq, g_headPitch, g_headYaw, g_headRoll);

        float hp[3];
        XR_GetHeadPos(hp);

        // Recenter: first real (nonzero) sample, or Numpad-Del re-captures.
        static bool prevDec = false;
        const bool decDown = (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0;
        const bool wantRecenter = (decDown && !prevDec);
        prevDec = decDown;

        const bool haveSample = (fabs(hp[0]) + fabs(hp[1]) + fabs(hp[2])) > 1e-4;
        if ((!g_posOriginSet && haveSample) || (wantRecenter && haveSample))
        {
            g_posOrigin[0] = hp[0]; g_posOrigin[1] = hp[1]; g_posOrigin[2] = hp[2];
            g_posOriginSet = true;
            Log("camera: HEAD POSITION recentered (origin %.3f %.3f %.3f m)",
                hp[0], hp[1], hp[2]);
        }

        if (g_posOriginSet)
        {
            // XR LOCAL: +x right, +y up, +z BACK. Metres -> cm.
            g_posRight = (double)(hp[0] - g_posOrigin[0]) * 100.0;
            g_posUp = (double)(hp[1] - g_posOrigin[1]) * 100.0;
            g_posFwd = -(double)(hp[2] - g_posOrigin[2]) * 100.0;

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

            _InterlockedIncrement(&g_lpSeq);        // odd == writing
            MemoryBarrier();
            g_lpQuat[0] = hq[0]; g_lpQuat[1] = hq[1];
            g_lpQuat[2] = hq[2]; g_lpQuat[3] = hq[3];
            g_lpPos[0] = px; g_lpPos[1] = py; g_lpPos[2] = pz;
            g_lpValid = applied ? 1 : 0;
            MemoryBarrier();
            _InterlockedIncrement(&g_lpSeq);        // even == done
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
                finalRot = ApplyWorldSpaceYaw(*CameraRotation,
                    g_headYaw, g_headPitch, g_headRoll);
                *CameraRotation = finalRot;
            }

            // Positional tracking: head-frame offset rotated into world XY by
            // the CLEAN yaw (mouse heading), so leaning forward goes into the
            // screen regardless of where the head is turned. UE: fwd=+X, right=+Y.
            if (g_cfgHeadPosition)
            {
                const double cy = UnitsToRad(cleanRot.yaw); // CLEAN yaw only -- the XR
                // room frame is fixed; adding head yaw double-counts the turn.
                const double cs = cos(cy), sn = sin(cy);
                CameraLocation->x += (float)(g_posFwd * cs - g_posRight * sn);
                CameraLocation->y += (float)(g_posFwd * sn + g_posRight * cs);
                CameraLocation->z += (float)g_posUp;
            }

            double s = (eye == 0 ? -1.0 : 1.0) * (double)g_cfgEyeSep;
            if (g_cfgSwapEyes) s = -s;

            // Eye offset along the FINAL (head-rotated) right vector (§6).
            const Vec3 right = RotatorRight(finalRot);

            CameraLocation->x += (float)(right.x * s);
            CameraLocation->y += (float)(right.y * s);
            CameraLocation->z += (float)(right.z * s);

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
        Log("--- camera: %llu calls, %d site(s), leader=site%d | writes L=%llu R=%llu ---",
            g_calls, g_siteCount, g_leader, g_wLeft, g_wRight);
        Log("  HEAD: yaw%7.1f  pitch%7.1f  roll%7.1f  deg   %s",
            g_headYaw, g_headPitch, g_headRoll,
            g_cfgHeadTracking ? "(WRITTEN to camera)" : "(computed, not written)");
        Log("  POS : right%7.1f  up%7.1f  fwd%7.1f  cm   %s",
            g_posRight, g_posUp, g_posFwd,
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
        if (g_qMin > 0) g_qMin = 0;
        if (g_qMax < 0) g_qMax = 0;
        g_lastEye ^= 1;
        return g_lastEye;
    }

    // RESYNC after an underrun period (the tutorial-mono bug). A stale tag
    // left in the queue after starvation shifts alignment by one FOREVER
    // (MEASURED: depth 1/1 became a permanent 2/2 after the 'press M' popup
    // -> every frame labeled with the WRONG eye -> broken stereo). Steady-
    // state depth is 1 (measured), so on the first real pop after any
    // underrun, jump to the NEWEST tag.
    if (g_needResync)
    {
        g_needResync = 0;
        if (depth > 1)
        {
            Log("camera: EYEQ RESYNC -- dropped %ld stale tag(s) after underrun", depth - 1);
            rd = wr - 1;
            g_eyeRd = rd;
            depth = 1;
        }
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

    Log(">>> CAMERA HOOK ARMED (write=%d). Load a level and move.", (int)g_cfgCameraWrite);
    return true;
}

void CameraHook_Remove()
{
    if (g_fnAddr) { MH_DisableHook(g_fnAddr); g_fnAddr = nullptr; }
}