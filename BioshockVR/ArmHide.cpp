// BioshockVR/ArmHide.cpp
//
// See ArmHide.h for what this does and why every write is guarded.
//
// MEASURED LAYOUT (BioShock Remastered, Win32):
//   AHands           + 0x3FC -> SkeletonInstance*
//   SkeletonInstance + 0x04  -> owning actor, used to prove the link
//   SkeletonInstance + 0x48  -> hkQsTransform* render bone array
//   SkeletonInstance + 0x4C  -> bone count, expected 47
//   SkeletonInstance + 0x88  -> evaluate-if-dirty byte
//
// hkQsTransform is three float4s: position, quaternion, scale. Only the xyz
// lanes are touched; the w lanes stay engine-owned.
//
// THE DIRTY BYTE IS NOT OPTIONAL. The bone array is lazily rebuilt: write the
// scales, leave the flag set, and the render path re-evaluates the animation
// straight over the top in the same frame. Clearing it is what makes the write
// stick. Setting it back to 1 on disable is what gets a clean pose back.

#include "ArmHide.h"

#include <windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern void LogFile(const char* msg);
extern int  g_cfgArmHideHandsVt;   // dllmain.cpp, ini-overridable
extern int  g_cfgArmHideSkelVt;    // dllmain.cpp, ini-overridable

static const unsigned kActorSkelOff = 0x3FC;
static const unsigned kSkelActorOff = 0x04;
static const unsigned kSkelBonesOff = 0x48;
static const unsigned kSkelCountOff = 0x4C;
static const unsigned kSkelDirtyOff = 0x88;
static const int      kExpectedBones = 47;

// The ten sleeve bones. NOTHING ELSE. In particular right-hand bone 43 is the
// WEAPON ATTACHMENT: the attachment path inverse-decomposes bone scale, so a
// zero there divides by zero and throws the weapon across the near plane.
static const int kRightWrist = 27;
static const int kLeftWrist = 6;
static const int kRightSleeve[5] = { 24, 25, 26, 45, 46 };   // clavicle..twist
static const int kLeftSleeve[5] = { 3,  4,  5, 22, 23 };
// Untouched: right hand 27-44, left hand 6-21.

struct BoneTransform
{
    float position[4];
    float rotation[4];
    float scale[4];
};
static_assert(sizeof(BoneTransform) == 48, "unexpected hkQsTransform size");

struct SavedBone
{
    int   index;
    float position[3];
    float scale[3];
    bool  valid;
};

static void* g_actor = nullptr;
static void* g_skeleton = nullptr;
static BoneTransform* g_bones = nullptr;
static int   g_boneCount = 0;
static SavedBone g_saved[10] = {};
static bool  g_hidden = false;
static bool  g_loggedOk = false;
static bool  g_loggedFail = false;

static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

static bool SafeRead(const void* src, void* dst, size_t n)
{
    if (!src || !dst || !n) return false;
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeWrite(void* dst, const void* src, size_t n)
{
    if (!dst || !src || !n) return false;
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static unsigned RvaOf(const void* addr)
{
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    if (!base || !addr) return 0;
    return (unsigned)((const uint8_t*)addr - base);
}

// What vtable does this object ACTUALLY have? Used only for logging, so a
// build with different addresses reports the numbers you need instead of just
// refusing. Returns 0 if the object cannot be read.
static unsigned VtableRvaOf(const void* obj)
{
    if (!obj) return 0;
    void* vt = nullptr;
    if (!SafeRead(obj, &vt, sizeof(vt))) return 0;
    return RvaOf(vt);
}

static bool VtableIs(const void* obj, int expectedRva)
{
    if (!obj || expectedRva <= 0) return false;
    void* vt = nullptr;
    if (!SafeRead(obj, &vt, sizeof(vt))) return false;
    return RvaOf(vt) == (unsigned)expectedRva;
}

static bool ScaleLooksNormal(const float s[4])
{
    const float m = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
    return (m == m) && m > 0.000001f;      // m==m rejects NaN
}

static SavedBone* SlotFor(int index)
{
    for (int i = 0; i < 10; ++i) if (g_saved[i].index == index) return &g_saved[i];
    for (int i = 0; i < 10; ++i)
        if (g_saved[i].index == 0 && !g_saved[i].valid)
        {
            g_saved[i].index = index;
            return &g_saved[i];
        }
    return nullptr;
}

static void ClearSaved()
{
    memset(g_saved, 0, sizeof(g_saved));
    for (int i = 0; i < 10; ++i) g_saved[i].index = -1;
    g_hidden = false;
}

static bool LocateSkeleton(void* hands)
{
    if (!hands) return false;

    // The skeleton pointer is read FIRST, so a build with different addresses
    // can report BOTH actual vtables in one run instead of costing a round trip
    // per value. Read-only and SEH-guarded either way.
    void* skel = nullptr;
    if (!SafeRead((uint8_t*)hands + kActorSkelOff, &skel, sizeof(skel)) || !skel)
        return false;

    // A configured value of 0 means "do not check this vtable". That is safe
    // here because it is not the only guard: HandsProbe identifies the actor
    // POSITIONALLY (it tracks the camera and the view rotator), the skeleton
    // must point back at this same actor, and the bone count must be exactly
    // 47. A wrong object fails all three.
    const bool handsOk = (g_cfgArmHideHandsVt <= 0) || VtableIs(hands, g_cfgArmHideHandsVt);
    const bool skelOk = (g_cfgArmHideSkelVt <= 0) || VtableIs(skel, g_cfgArmHideSkelVt);

    if (!handsOk || !skelOk)
    {
        if (!g_loggedFail)
        {
            g_loggedFail = true;
            Log("!!! ARMHIDE: vtable mismatch -- this is a DIFFERENT BUILD of the game.");
            Log("!!! ARMHIDE:   AHands            expected 0x%X   ACTUAL 0x%X",
                (unsigned)g_cfgArmHideHandsVt, VtableRvaOf(hands));
            Log("!!! ARMHIDE:   SkeletonInstance  expected 0x%X   ACTUAL 0x%X",
                (unsigned)g_cfgArmHideSkelVt, VtableRvaOf(skel));
            Log("!!! ARMHIDE: Refusing all writes. To enable arm hiding on this build,");
            Log("!!! ARMHIDE: put the two ACTUAL values into BioshockVR.ini:");
            Log("!!! ARMHIDE:   ArmHideHandsVt=0x%X", VtableRvaOf(hands));
            Log("!!! ARMHIDE:   ArmHideSkelVt=0x%X", VtableRvaOf(skel));
            Log("!!! ARMHIDE: or set HideArmSleeves=0 to silence this.");
        }
        return false;
    }

    // Prove the skeleton belongs to THIS actor before writing into it.
    void* owner = nullptr;
    if (!SafeRead((uint8_t*)skel + kSkelActorOff, &owner, sizeof(owner)) ||
        owner != hands)
        return false;

    BoneTransform* bones = nullptr;
    int count = 0;
    if (!SafeRead((uint8_t*)skel + kSkelBonesOff, &bones, sizeof(bones))) return false;
    if (!SafeRead((uint8_t*)skel + kSkelCountOff, &count, sizeof(count)))  return false;

    if (!bones || count != kExpectedBones)
    {
        if (!g_loggedFail)
        {
            g_loggedFail = true;
            Log("!!! ARMHIDE: bone array %p count %d, expected %d. Refusing writes --",
                (void*)bones, count, kExpectedBones);
            Log("!!! ARMHIDE: the bone INDICES are only valid for the 47-bone rig.");
        }
        return false;
    }

    if (hands != g_actor || skel != g_skeleton || bones != g_bones)
    {
        g_actor = hands; g_skeleton = skel; g_bones = bones; g_boneCount = count;
        ClearSaved();
        Log(">>> ARMHIDE: skeleton locked: actor=0x%08X skel=0x%08X bones=0x%08X count=%d",
            (unsigned)(uintptr_t)hands, (unsigned)(uintptr_t)skel,
            (unsigned)(uintptr_t)bones, count);
    }
    return true;
}

static void SetDirty(uint8_t v)
{
    if (!g_skeleton) return;
    SafeWrite((uint8_t*)g_skeleton + kSkelDirtyOff, &v, sizeof(v));
}

static bool CollapseBone(int idx, const float anchor[4])
{
    if (!g_bones || idx < 0 || idx >= g_boneCount) return false;

    BoneTransform cur = {};
    if (!SafeRead(&g_bones[idx], &cur, sizeof(cur))) return false;

    // Only save a pose the ENGINE produced. Without this test the second eye
    // saves our own zeroed values and "restore" restores nothing.
    if (ScaleLooksNormal(cur.scale))
    {
        SavedBone* s = SlotFor(idx);
        if (s)
        {
            memcpy(s->position, cur.position, sizeof(s->position));
            memcpy(s->scale, cur.scale, sizeof(s->scale));
            s->valid = true;
        }
    }

    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    if (!SafeWrite(g_bones[idx].position, anchor, sizeof(float) * 3)) return false;
    if (!SafeWrite(g_bones[idx].scale, zero, sizeof(zero))) return false;
    return true;
}

static bool CollapseArm(const int* idx, int n, int wristIdx)
{
    if (!g_bones || wristIdx < 0 || wristIdx >= g_boneCount) return false;

    BoneTransform wrist = {};
    if (!SafeRead(&g_bones[wristIdx], &wrist, sizeof(wrist))) return false;

    bool any = false;
    for (int i = 0; i < n; ++i)
        any = CollapseBone(idx[i], wrist.position) || any;
    return any;
}

static void RestoreSkeleton()
{
    if (!g_skeleton || !g_bones || !g_hidden) return;

    for (int i = 0; i < 10; ++i)
    {
        const SavedBone& s = g_saved[i];
        if (!s.valid || s.index < 0 || s.index >= g_boneCount) continue;
        SafeWrite(g_bones[s.index].position, s.position, sizeof(s.position));
        SafeWrite(g_bones[s.index].scale, s.scale, sizeof(s.scale));
    }

    SetDirty(1);          // ask the engine for a completely fresh pose
    g_hidden = false;
    Log(">>> ARMHIDE: sleeve bones restored, engine evaluation requested.");
}

bool ArmHide_Update(void* handsActor, bool hide)
{
    if (!handsActor) { ArmHide_Reset(); return false; }
    if (!LocateSkeleton(handsActor)) return false;

    if (!hide) { RestoreSkeleton(); return true; }

    const bool r = CollapseArm(kRightSleeve, 5, kRightWrist);
    const bool l = CollapseArm(kLeftSleeve, 5, kLeftWrist);
    if (!r && !l) return false;

    SetDirty(0);          // stop the render pass rebuilding over us this frame
    g_hidden = true;

    if (!g_loggedOk)
    {
        g_loggedOk = true;
        Log(">>> ARMHIDE: sleeve bones collapsed (hands and weapon preserved).");
    }
    return true;
}

void ArmHide_Reset()
{
    g_actor = nullptr;
    g_skeleton = nullptr;
    g_bones = nullptr;
    g_boneCount = 0;
    ClearSaved();
    g_loggedFail = false;
}
