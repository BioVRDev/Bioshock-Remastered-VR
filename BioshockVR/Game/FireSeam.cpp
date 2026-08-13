// BioshockVR/Game/FireSeam.cpp
//
// ============================================================================
//  AWeapon::GetPerfectFireStart -- LOCATED SINCE 2026-08-11, CALLED SINCE NOW
// ============================================================================
// The header says why. This file says how, and every guard in it exists because
// this is the firing path.
//
// WHAT THE ENGINE DOES HERE, measured (docs/ENGINE-MAP.md, and confirmed live by
// an independently developed mod against the same executable):
//
//     __thiscall void GetPerfectFireStart(FVector* outA, FVector* outB,
//                                         FVector* outC)      // ret 0xC
//
//     outA <- ownerPawn+0x1D8        the pawn's Location. THE BUG.
//     outB <- [pawn+0x450]+0x1E4     the controller's Rotation -- and this IS
//                                    the fire direction, confirmed by
//                                    substitution moving the decals.
//
// AWeapon::ApplyAimError runs AFTER this and reads what we leave behind, so
// per-weapon spread survives substitution untouched. That is the reason this is
// the right seam and not the trace itself.
//
// ---- THE TRAP THAT COST ANOTHER PROJECT A WHOLE SESSION --------------------
// AN FRotator READS AS "0.000 0.000 0.000". Rotation units are int32s whose
// float reinterpretation is a denormal, so an out-param holding a perfectly good
// rotator prints as three zeros and looks like an untouched slot. Every value
// here is therefore CLASSIFIED BY BIT PATTERN, never by index, and the log
// prints a rotator as integers.
//
// ---- WHY THE SLOT AND NOT THE RVA -----------------------------------------
// We hold the weapon pointer already, so slot +0x304 of its vtable is one read.
// The rva it resolves to is not our constant to maintain, it is a thing we log.
// docs/ENGINE-MAP.md § Storefront divergence is the argument, made concrete
// twice already in this project.

#include "Game/FireSeam.h"
#include "Hands/HandsProbe.h"
#include "Core/Config.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <intrin.h>         // _InterlockedIncrement

#include <MinHook.h>

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
    return g_modBase ? (unsigned)((const uint8_t*)p - g_modBase) : 0u;
}

// Deliberate copies of EngineBridge's shapes rather than a shared header: those
// two files answer to different risk budgets and neither should be able to
// change the other's guards by accident.
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
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
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

// ---------------------------------------------------------------- snapshot
//
// WRITTEN by CalcView (game thread), READ by the detour (game thread, inside an
// engine call). Same thread, so no locking -- but they are far apart in time
// and the timestamp is what makes that safe rather than lucky.

static double g_origin[3] = {};
static bool   g_originValid = false;
static DWORD  g_originMs = 0;

static int    g_aim[3] = {};
static bool   g_aimValid = false;
static DWORD  g_aimMs = 0;

void FireSeam_PublishOrigin(const double originWorld[3])
{
    g_origin[0] = originWorld[0];
    g_origin[1] = originWorld[1];
    g_origin[2] = originWorld[2];
    g_originValid = true;
    g_originMs = GetTickCount();
}

void FireSeam_PublishAim(int pitch, int yaw, int roll)
{
    g_aim[0] = pitch; g_aim[1] = yaw; g_aim[2] = roll;
    g_aimValid = true;
    g_aimMs = GetTickCount();
}

// A FIXED FEW FRAMES, deliberately NOT the watch interval. Those two numbers
// look alike and mean opposite things: one is how often we are willing to talk,
// the other is how old a pose may be before aiming from it is a lie. Tying them
// together would let someone quieten the log and silently widen the window
// where a stale hand can still fire.
static const DWORD kStaleMs = 250;

static bool Fresh(bool valid, DWORD stamp)
{
    return valid && (GetTickCount() - stamp) <= kStaleMs;
}

// ---------------------------------------------------------------- census
//
// Per weapon slot, because "which weapons even reach this seam" is the question
// the tester's report actually asks. A weapon that fires with zero calls here
// uses a different path and is a different bug -- the wrench is expected to be
// one of those, its damage being a Havok phantom with no aim seam at all.

static unsigned g_callsBySlot[10] = {};    // 9 slots + one bucket for unknown
static unsigned g_subs = 0;
static unsigned g_skipNotOurs = 0;         // an AI's weapon
static unsigned g_skipStale = 0;           // hands not ours this frame
static unsigned g_skipUnwritable = 0;
static DWORD    g_lastWatchMs = 0;

static const char* kWepName[9] = {
    "Wrench", "Pistol", "Shotgun", "Crossbow", "GrenadeLauncher",
    "MachineGun", "ChemicalThrower", "ResearchCamera", "Plasmid"
};

void FireSeam_LogCensus()
{
    char line[256] = {};
    size_t n = 0;
    for (int s = 0; s < 10; ++s)
    {
        if (!g_callsBySlot[s] || n + 32 >= sizeof(line)) continue;
        const int w = _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
            "%s%s=%u", n ? " " : "",
            (s < 9) ? kWepName[s] : "unknown", g_callsBySlot[s]);
        if (w > 0) n += (size_t)w;
    }
    if (!n) _snprintf_s(line, sizeof(line), _TRUNCATE, "(no calls)");

    Log(">>> FIRESEAM census: %s | subs %u  skipped: notOurs %u stale %u "
        "unwritable %u", line, g_subs, g_skipNotOurs, g_skipStale,
        g_skipUnwritable);
}

void FireSeam_Reset()
{
    g_originValid = false;
    g_aimValid = false;
    memset(g_callsBySlot, 0, sizeof(g_callsBySlot));
    g_subs = 0;
    g_skipNotOurs = g_skipStale = g_skipUnwritable = 0;
}

// ---------------------------------------------------------------- classify
//
// WHAT DID THE ENGINE JUST PUT IN THIS 12-BYTE OUT-PARAM? Decided from the
// value, because the fire-start functions hand back a mix and trusting a fixed
// index means trusting a disassembly we did not do ourselves.
//
//   Rotator   three rotation-unit int32s (65536 per turn). Their FLOAT
//             reinterpretation is a denormal -- see the banner.
//   Direction ordinary floats, length about 1.
//   Position  ordinary floats, thousands of Unreal units.
//   Unused    all three words zero. The engine left it alone; so do we.

enum SlotKind { kUnused, kRotator, kDirection, kPosition };

static SlotKind Classify(const float v[3])
{
    int32_t i[3];
    memcpy(i, v, sizeof(i));
    if (!i[0] && !i[1] && !i[2]) return kUnused;

    bool allSmallInts = true;
    for (int k = 0; k < 3; ++k)
    {
        const int32_t a = (i[k] < 0) ? -i[k] : i[k];
        if (a > (1 << 21)) allSmallInts = false;  // 2M rotation units is nonsense
    }
    if (allSmallInts) return kRotator;

    const double len2 = (double)v[0] * v[0] + (double)v[1] * v[1] +
        (double)v[2] * v[2];
    return (len2 < 4.0) ? kDirection : kPosition;
}

static const char* KindName(SlotKind k)
{
    return (k == kRotator) ? "rot" : (k == kDirection) ? "dir"
        : (k == kPosition) ? "pos" : "-";
}

// An FRotator to a unit direction, the engine's own convention: X forward,
// Y right, Z up, 65536 units per turn.
static void RotToDir(const int rot[3], float out[3])
{
    const double kToRad = 3.14159265358979323846 / 32768.0;
    const double p = (double)(int16_t)(rot[0] & 0xFFFF) * kToRad;
    const double y = (double)(int16_t)(rot[1] & 0xFFFF) * kToRad;
    out[0] = (float)(cos(p) * cos(y));
    out[1] = (float)(cos(p) * sin(y));
    out[2] = (float)sin(p);
}

// ---------------------------------------------------------------- ownership
//
// AI weapons inherit AWeapon, so this seam runs for every splicer in the level.
// Substituting blindly would aim the whole of Rapture with the player's
// controller. The owning pawn sits at [weapon+0x454] -- the implementation
// itself reads it -- and we already hold the player's pawn by identity, which
// is a stronger test than any vtable compare.

static bool OwnerIsPlayer(const void* weapon)
{
    const void* pawn = HandsProbe_GetPawn();
    if (!weapon || !pawn) return false;

    const unsigned off = (g_cfg.weaponOwnerOff > 0)
        ? (unsigned)g_cfg.weaponOwnerOff : 0x454u;
    if (!Readable((const uint8_t*)weapon + off, 4)) return false;

    const void* owner = *(void* const*)((const uint8_t*)weapon + off);

    // ONE SHOT, AND IT CLOSES A DIAGNOSTIC HOLE. If the offset were wrong this
    // returns false for every call, the log fills with nothing, and the honest
    // reading of that -- "the seam never fires" -- would be exactly backwards.
    // Print the first pair either way and the ambiguity cannot survive one run.
    static bool announced = false;
    if (!announced)
    {
        announced = true;
        Log(">>> FIRESEAM: first call -- [weapon+0x%X] = 0x%08X, our pawn = "
            "0x%08X  -> %s", off, (unsigned)(uintptr_t)owner,
            (unsigned)(uintptr_t)pawn,
            (owner == pawn) ? "OURS" : "not ours (an AI, or a wrong offset)");
    }

    return owner == pawn;
}

// ---------------------------------------------------------------- the detour

typedef void(__fastcall* FireStartFn)(void* self, void* edx,
    float* outA, float* outB, float* outC);

static FireStartFn g_orig = nullptr;
static void* g_target = nullptr;

static void Watch(const void* self, const float a[3], const float b[3],
    const float c[3], bool subbed, const char* why)
{
    const DWORD every = (g_cfg.fireSeamWatchMs > 0)
        ? (DWORD)g_cfg.fireSeamWatchMs : 250u;
    const DWORD now = GetTickCount();
    if (g_lastWatchMs && (now - g_lastWatchMs) < every) return;
    g_lastWatchMs = now;

    // The engine's own fire start is whichever slot classified as a POSITION.
    const float* eng = nullptr;
    const float* slots[3] = { a, b, c };
    for (int i = 0; i < 3; ++i)
        if (Classify(slots[i]) == kPosition) { eng = slots[i]; break; }

    double d = -1.0;
    if (eng && g_originValid)
    {
        const double dx = g_origin[0] - eng[0];
        const double dy = g_origin[1] - eng[1];
        const double dz = g_origin[2] - eng[2];
        d = sqrt(dx * dx + dy * dy + dz * dz);
    }

    // A rotator is printed as INTEGERS. Printed as floats it reads 0.000 0.000
    // 0.000 and looks like an empty slot -- see the banner.
    int32_t bi[3]; memcpy(bi, b, sizeof(bi));
    const int slot = HandsProbe_WeaponSlot();

    Log("FIRESEAM: %-16s A[%s]=(%.1f %.1f %.1f) B[%s]=(%d %d %d) C[%s]=(%.1f "
        "%.1f %.1f) | ours=(%.1f %.1f %.1f) d=%.1f cm  sub=%d %s",
        (slot >= 0 && slot < 9) ? kWepName[slot] : "?",
        KindName(Classify(a)), a[0], a[1], a[2],
        KindName(Classify(b)), bi[0], bi[1], bi[2],
        KindName(Classify(c)), c[0], c[1], c[2],
        g_originValid ? g_origin[0] : 0.0,
        g_originValid ? g_origin[1] : 0.0,
        g_originValid ? g_origin[2] : 0.0,
        d, subbed ? 1 : 0, why);
}

static void __fastcall hkFireStart(void* self, void* edx,
    float* outA, float* outB, float* outC)
{
    // THE ORIGINAL RUNS FIRST, ALWAYS. Its numbers are what we log, what we
    // measure against, and what survives every refusal below -- so the worst
    // case here is exactly today's behaviour plus one log line.
    g_orig(self, edx, outA, outB, outC);

    const int slot = HandsProbe_WeaponSlot();
    ++g_callsBySlot[(slot >= 0 && slot < 9) ? slot : 9];

    // THE CENSUS HAS TO REACH THE LOG WITHOUT A LEVEL CHANGE. A tester who
    // fires every weapon at a wall and then quits would otherwise leave nothing
    // but rate-limited watch lines, and "which weapons even reach this seam" is
    // the question the whole observe run exists to answer.
    {
        static DWORD lastCensus = 0;
        const DWORD now = GetTickCount();
        if (!lastCensus) lastCensus = now;
        else if (now - lastCensus >= 30000) { lastCensus = now; FireSeam_LogCensus(); }
    }

    float a[3] = {}, b[3] = {}, c[3] = {};
    if (Readable(outA, 12)) memcpy(a, outA, 12);
    if (Readable(outB, 12)) memcpy(b, outB, 12);
    if (Readable(outC, 12)) memcpy(c, outC, 12);

    if (!OwnerIsPlayer(self))
    {
        ++g_skipNotOurs;                  // a splicer's gun. Not ours to aim.
        return;
    }

    if (g_cfg.fireSeam < 2)
    {
        Watch(self, a, b, c, false, "observe");
        return;
    }

    // FRESHNESS IS THE GAMEPLAY GATE -- see the header. A stale origin means
    // DriveHands stood down this frame, which means a cutscene, a UI panel, a
    // scripted window or sixDofHands=0, and in every one of those the hands are
    // not ours to fire from.
    const bool wantPos = (g_cfg.fireOriginSub != 0) && Fresh(g_originValid, g_originMs);
    const bool wantRot = (g_cfg.fireAimSub != 0) && Fresh(g_aimValid, g_aimMs);

    if (!wantPos && !wantRot)
    {
        ++g_skipStale;
        Watch(self, a, b, c, false, "stale");
        return;
    }

    float dir[3] = {};
    if (wantRot) RotToDir(g_aim, dir);

    const float pos[3] = { (float)g_origin[0], (float)g_origin[1],
                           (float)g_origin[2] };

    float* const outs[3] = { outA, outB, outC };
    const float* const vals[3] = { a, b, c };
    bool wrote = false;

    for (int i = 0; i < 3; ++i)
    {
        if (!outs[i] || !Writable(outs[i], 12)) { ++g_skipUnwritable; continue; }

        switch (Classify(vals[i]))
        {
        case kPosition:
            if (!wantPos) break;
            memcpy(outs[i], pos, 12);
            wrote = true;
            break;

        case kRotator:
        {
            if (!wantRot) break;
            // ROLL PASSES THROUGH UNTOUCHED. Aim owns pitch and yaw; the
            // weapon's own roll is none of our business, and a canted gun is
            // something the animation is entitled to.
            int32_t r[3];
            memcpy(r, vals[i], sizeof(r));
            r[0] = g_aim[0];
            r[1] = g_aim[1];
            memcpy(outs[i], r, 12);
            wrote = true;
            break;
        }

        case kDirection:
            if (!wantRot) break;
            memcpy(outs[i], dir, 12);
            wrote = true;
            break;

        case kUnused:
        default:
            break;                         // the engine left it alone; so do we
        }
    }

    if (wrote) ++g_subs;
    Watch(self, a, b, c, wrote, wrote ? "SUB" : "nothing to write");
}

// ---------------------------------------------------------------- install

static bool g_settled = false;             // hooked, or refused for good
static void* g_seen[4] = {};               // distinct vtable slot values
static int   g_nSeen = 0;

// THE ONE CHECK THAT PREVENTS SILENT STACK CORRUPTION.
//
// We are about to call this function with three stack arguments and let it
// clean them up. If the real function takes a different number, the stack ends
// up misaligned on return -- and in a Release build that does not crash, it
// corrupts. So find the function's own `ret imm16` and require the count to
// match before hooking anything.
//
// Scanning for the FIRST `C2 xx 00` is enough: MSVC emits one epilogue shape per
// function and the immediate is the same at every exit.
static bool RetImmMatches(const uint8_t* fn, unsigned want, unsigned* found)
{
    const unsigned kSpan = 512;
    if (!Readable(fn, kSpan)) return false;

    for (unsigned i = 0; i + 2 < kSpan; ++i)
    {
        if (fn[i] != 0xC2) continue;
        if (fn[i + 2] != 0x00) continue;   // imm16 above 255 is not a real frame
        if (found) *found = fn[i + 1];
        return fn[i + 1] == want;
    }
    return false;
}

static void NoteTarget(void* fn)
{
    for (int i = 0; i < g_nSeen; ++i) if (g_seen[i] == fn) return;
    if (g_nSeen >= 4) return;

    g_seen[g_nSeen++] = fn;
    if (g_nSeen > 1)
    {
        // A SUBCLASS OVERRIDES THE IMPLEMENTATION, and our single hook does not
        // cover it. This is exactly what "some of them werent even firing from
        // the gun barrel" would look like, so it is logged loudly rather than
        // counted quietly.
        Log("!!! FIRESEAM: a SECOND implementation at 0x%08X (rva 0x%X) -- "
            "weapon slot %d. The installed hook does NOT cover it.",
            (unsigned)(uintptr_t)fn, Rva(fn), HandsProbe_WeaponSlot());
    }
}

void FireSeam_TryInstall(const void* heldWeapon)
{
    if (!heldWeapon || !g_cfg.fireSeam) return;
    if (!ModuleBounds()) return;

    const unsigned vtOff = (g_cfg.fireSeamVtOff > 0)
        ? (unsigned)g_cfg.fireSeamVtOff : 0x304u;

    if (!Readable(heldWeapon, 4)) return;
    void* const vt = *(void* const*)heldWeapon;
    if (!vt || !Readable((const uint8_t*)vt + vtOff, 4)) return;

    void* const fn = *(void* const*)((const uint8_t*)vt + vtOff);
    if (!fn) return;

    NoteTarget(fn);                        // census runs even after we settle
    if (g_settled) return;

    // ---- fail closed, and say which gate refused ------------------------
    if (!InModule(fn) || !IsExecutable(fn))
    {
        g_settled = true;
        Log("!!! FIRESEAM: vtable +0x%X -> 0x%08X is not executable game code. "
            "NOT HOOKED.", vtOff, (unsigned)(uintptr_t)fn);
        return;
    }

    const uint8_t* const b = (const uint8_t*)fn;
    if (!Readable(b, 8) || (b[0] != 0x55 && b[0] != 0x53 && b[0] != 0x83))
    {
        g_settled = true;
        Log("!!! FIRESEAM: rva 0x%X prologue %02X %02X %02X %02X is not a "
            "function entry. NOT HOOKED.", Rva(fn), b[0], b[1], b[2], b[3]);
        return;
    }

    const unsigned wantRet = (g_cfg.fireSeamRetImm > 0)
        ? (unsigned)g_cfg.fireSeamRetImm : 0xCu;
    unsigned gotRet = 0;
    if (!RetImmMatches(b, wantRet, &gotRet))
    {
        g_settled = true;
        Log("!!! FIRESEAM: rva 0x%X cleans up 0x%X bytes, we would call it with "
            "0x%X. THAT CORRUPTS THE STACK SILENTLY IN A RELEASE BUILD. "
            "NOT HOOKED.", Rva(fn), gotRet, wantRet);
        return;
    }

    MH_STATUS s = MH_CreateHook(fn, &hkFireStart, (LPVOID*)&g_orig);
    if (s != MH_OK)
    {
        g_settled = true;
        Log("!!! FIRESEAM: MH_CreateHook -> %d. NOT HOOKED.", (int)s);
        return;
    }

    s = MH_EnableHook(fn);
    if (s != MH_OK)
    {
        g_settled = true;
        Log("!!! FIRESEAM: MH_EnableHook -> %d. NOT HOOKED.", (int)s);
        return;
    }

    g_settled = true;
    g_target = fn;
    Log(">>> FIRESEAM: hooked GetPerfectFireStart @ 0x%08X (rva 0x%X) via "
        "vtable +0x%X, ret 0x%X, prologue %02X %02X %02X %02X %02X %02X -- "
        "mode %d (%s)",
        (unsigned)(uintptr_t)fn, Rva(fn), vtOff, gotRet,
        b[0], b[1], b[2], b[3], b[4], b[5],
        g_cfg.fireSeam,
        (g_cfg.fireSeam >= 2) ? "SUBSTITUTING" : "observe only, writes nothing");

    if (g_cfg.fireSeam >= 2)
        Log(">>> FIRESEAM: origin %s, aim %s",
            g_cfg.fireOriginSub ? "SUBSTITUTED" : "engine",
            g_cfg.fireAimSub ? "SUBSTITUTED" : "engine");
}
