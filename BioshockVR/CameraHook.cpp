// BioshockVR/CameraHook.cpp
//
// Phase 5: find APlayerController::eventPlayerCalcView by FName chain and hook
// it READ-ONLY. We call the original, then log what it wrote. We change nothing.
//
// The search (handoff 6a) is six stages. Every stage logs. If ANY stage fails
// we install NO hook -- a wrong hook here corrupts the stack and kills the game.

#include "CameraHook.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <intrin.h>     // _ReturnAddress

#include <MinHook.h>

#pragma comment(lib, "psapi.lib")

extern void LogFile(const char* msg);
static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

// ---------------------------------------------------------------- types

struct FVector { float   x, y, z; };            // 1 unit == 1 CENTIMETRE (to verify!)
struct FRotator { int32_t pitch, yaw, roll; };   // 65536 units == 360 degrees

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

    // Try every string hit; the first one that yields a function wins.
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

            // --- STAGE 4: xrefs to that global, skipping any within 200 bytes
            //              of the string xref (the name table is one big init loop) ---
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

// ---------------------------------------------------------------- the detour

// The out-pointers are the CALLER'S STACK LOCALS -- they are NOT stable, and
// the handoff's pointer-cache "optimization" was built on a false premise.
// No cache. Validate cheaply, log nothing per-call.
//
// PHASE 5b: bucket calls by RETURN ADDRESS. The log showed ~1.85 calls per
// frame, so there is more than one call site. We need to know whether they
// agree before Phase 6 starts writing to CameraRotation.

struct CallSite
{
    void* ret;       // absolute return address
    uint64_t count;
    FVector  loc;
    FRotator rot;
};

static CallSite g_sites[8] = {};
static int      g_siteCount = 0;

static uint64_t g_calls = 0;
static DWORD    g_lastTick = 0;
static void* g_pThis = nullptr;

static double UnitsToDeg(int32_t u)
{
    // The game stores rotator components in 0..65535. Reinterpret the low 16
    // bits as signed to get -180..+180.
    return (double)(int16_t)(u & 0xFFFF) * (360.0 / 65536.0);
}

static void __fastcall hkCalcView(void* pThis, void* edx,
    void** ViewActor,
    FVector* CameraLocation,
    FRotator* CameraRotation)
{
    // 1. Call the ORIGINAL first. UnrealScript fills *CameraRotation with the
    //    game's intended view. Phase 5 does NOTHING else -- we only read.
    g_orig(pThis, edx, ViewActor, CameraLocation, CameraRotation);

    void* ret = _ReturnAddress();
    g_pThis = pThis;

    if (g_calls == 0)
    {
        Log(">>> CAMERA HOOK FIRED. First call.");
        Log("camera:   pThis = 0x%08X  (APlayerController*)", (unsigned)(uintptr_t)pThis);
        g_lastTick = GetTickCount();
    }
    ++g_calls;

    if (!CameraLocation || !CameraRotation) return;
    if (!IsMemoryValid(CameraLocation, sizeof(FVector)))  return;
    if (!IsMemoryValid(CameraRotation, sizeof(FRotator))) return;

    // Find or create the bucket for this caller.
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
    if (!site) return;   // more than 8 call sites: something is very wrong

    ++site->count;
    site->loc = *CameraLocation;
    site->rot = *CameraRotation;

    // Report every call site once a second. If they agree, we write to both in
    // Phase 6. If they DISAGREE, we filter on return address.
    DWORD now = GetTickCount();
    if (now - g_lastTick >= 1000)
    {
        g_lastTick = now;
        Log("--- camera: %llu calls total, %d call site(s) ---", g_calls, g_siteCount);
        for (int i = 0; i < g_siteCount; ++i)
        {
            const CallSite& s = g_sites[i];
            Log("  site%d mod+0x%-7X n=%-8llu pos %9.1f %9.1f %9.1f   p%7.1f y%7.1f r%7.1f",
                i, (unsigned)((uint8_t*)s.ret - g_modBase), s.count,
                s.loc.x, s.loc.y, s.loc.z,
                UnitsToDeg(s.rot.pitch), UnitsToDeg(s.rot.yaw), UnitsToDeg(s.rot.roll));
        }
    }
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

    if (!fn) return false;     // handoff 6a: any stage fails -> install NOTHING

    g_fnAddr = fn;

    MH_STATUS s = MH_CreateHook(fn, &hkCalcView, (LPVOID*)&g_orig);
    if (s != MH_OK) { Log("camera: MH_CreateHook -> %d. No hook.", (int)s); g_fnAddr = nullptr; return false; }

    s = MH_EnableHook(fn);
    if (s != MH_OK) { Log("camera: MH_EnableHook -> %d. No hook.", (int)s); g_fnAddr = nullptr; return false; }

    Log(">>> CAMERA HOOK ARMED. Load a level and move.");
    return true;
}

void CameraHook_Remove()
{
    if (g_fnAddr) { MH_DisableHook(g_fnAddr); g_fnAddr = nullptr; }
}