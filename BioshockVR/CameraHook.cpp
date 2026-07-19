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
extern int   g_cfgHeadAimMode;    // 0 legacy additive, 1 local compose, 2 pitch-decoupled
extern bool  g_cfgPairLock;
extern bool  g_cfgHeadAim;
extern bool g_cfgHeadRoll;

bool DrawHook_MenuUp();   // DrawHook.cpp

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
static long           g_deepPops = 0;      // consecutive pops with depth > 1
extern float g_cfgHeightOffset;   // CameraHeightOffset, cm

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

        float hp[3];
        XR_GetHeadPos(hp);

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
            const bool lag = (g_cfgHeadAim && g_prevQuatValid);
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
                if (g_cfgHeadAim)
                {
                    // Head yaw/pitch already reached the view THROUGH the aim
                    // field (+0x1E4 -> Controller.Rotation -> CameraRotation).
                    // Adding them here too applies the head TWICE and the view
                    // swims as if the mouse were being dragged. Roll only: the
                    // controller rotator can't carry head roll, so it's the one
                    // component still missing from the view.
                    finalRot = ApplyWorldSpaceYaw(*CameraRotation,
                        0.0, 0.0, g_cfgHeadRoll ? g_headRoll : 0.0);
                }
                else
                {
                    finalRot = ApplyWorldSpaceYaw(*CameraRotation,
                        g_headYaw, g_headPitch, g_headRoll);
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
                CameraLocation->x += (float)(g_posFwd * cs - g_posRight * sn);
                CameraLocation->y += (float)(g_posFwd * sn + g_posRight * cs);
                CameraLocation->z += (float)g_posUp;
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
                        // S21 diagnostic: how much pitch is the GAME still
                        // injecting? If mouse-Y is truly dead at the engine
                        // level this stays at 0.0 while you sweep the mouse
                        // up and down. If it does not, the User.ini edit did
                        // not take -- almost always because the live User.ini
                        // is beside the live Bioshock.ini, not where the
                        // guide's default path says.
                        const int dP = RotDelta(aim->pitch, g_aimLastWrote.pitch);
                        const int dY = RotDelta(aim->yaw, g_aimLastWrote.yaw);
                        const int dR = RotDelta(aim->roll, g_aimLastWrote.roll);

                        g_aimGameDPitch += fabs((double)dP);

                        g_aimBase.pitch += dP;
                        g_aimBase.yaw += dY;
                        g_aimBase.roll += dR;
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
                        want = ComposeHeadLocal(g_aimBase, g_headYaw, g_headPitch,
                            g_cfgHeadAimMode >= 2);
                        // The controller rotator cannot carry head roll (S6);
                        // roll still reaches the view through the compose above.
                        want.roll = g_aimBase.roll;
                    }

                    *aim = want;
                    g_aimLastWrote = want;
                }
            }

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
        Log("  MOUSE-Y: game injected %.1f deg of pitch this second   (0.0 == mouse Y is dead)",
            g_aimGameDPitch / 182.0444);
        g_aimGameDPitch = 0.0;

        Log("--- camera: %llu calls, %d site(s), leader=site%d | writes L=%llu R=%llu ---",
            g_calls, g_siteCount, g_leader, g_wLeft, g_wRight);
        Log("  HEAD: yaw%7.1f  pitch%7.1f  roll%7.1f  deg   %s",
            g_headYaw, g_headPitch, g_headRoll,
            g_cfgHeadTracking ? "(WRITTEN to camera)" : "(computed, not written)");
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

    Log(">>> CAMERA HOOK ARMED (write=%d). Load a level and move.", (int)g_cfgCameraWrite);
    return true;
}

void CameraHook_Remove()
{
    if (g_fnAddr) { MH_DisableHook(g_fnAddr); g_fnAddr = nullptr; }
}