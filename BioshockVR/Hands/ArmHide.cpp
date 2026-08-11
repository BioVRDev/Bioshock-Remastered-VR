// BioshockVR/Hands/ArmHide.cpp
//
// TWO features, one skeleton. See ArmHide.h.
//
//   1. SLEEVE HIDING       the ten forearm bones, always, both hands.
//   2. INACTIVE HAND       the whole non-active hand, when only one is in use.
//
// They share LocateSkeleton() deliberately. Both write into the same bone
// array, and every guard in this file -- the vtable checks, the owner check,
// the 47-bone count, the dirty byte -- has to hold for both or neither.
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

#include "Hands/ArmHide.h"

#include <windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>            // M7-S4 motion sampling
#include "Core/Config.h"

static void MotionReset();        // defined with the M7-S4 block below
static void LeftClusterReset();   // defined with the M6-S1 block below

extern void LogFile(const char* msg);

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

// The full hand clusters, for INACTIVE HAND hiding. These are the bones the
// sleeve pass deliberately leaves alone.
static const int kLeftClusterFirst = 6;
static const int kLeftClusterLast = 21;
static const int kRightClusterFirst = 27;
static const int kRightClusterLast = 44;

// Same bone 43, same reason, different treatment. Zeroing its scale breaks the
// attachment decomposition. It gets pushed far below the world instead, with
// its scale left exactly as the engine wrote it.
static const int kWeaponAttachBone = 43;
static const float kFarBelow[3] = { 0.0f, 0.0f, -5000.0f };

#define HAND_LEFT   0
#define HAND_RIGHT  1

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

// Inactive-hand state. Its own save array: the right cluster is 18 bones plus
// 5 sleeve, which does not fit in the sleeve pass's ten slots.
static SavedBone g_handSaved[24] = {};
static int  g_hiddenHand = -1;
static bool g_loggedHandOk = false;

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

static SavedBone* HandSlotFor(int index)
{
    for (int i = 0; i < 24; ++i)
        if (g_handSaved[i].valid && g_handSaved[i].index == index)
            return &g_handSaved[i];
    for (int i = 0; i < 24; ++i)
        if (!g_handSaved[i].valid)
        {
            g_handSaved[i].index = index;
            return &g_handSaved[i];
        }
    return nullptr;
}

static void ClearHandSaved()
{
    memset(g_handSaved, 0, sizeof(g_handSaved));
    for (int i = 0; i < 24; ++i) g_handSaved[i].index = -1;
    g_hiddenHand = -1;
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
    const bool handsOk = (g_cfg.armHideHandsVt <= 0) || VtableIs(hands, g_cfg.armHideHandsVt);
    const bool skelOk = (g_cfg.armHideSkelVt <= 0) || VtableIs(skel, g_cfg.armHideSkelVt);

    if (!handsOk || !skelOk)
    {
        if (!g_loggedFail)
        {
            g_loggedFail = true;
            Log("!!! ARMHIDE: vtable mismatch -- this is a DIFFERENT BUILD of the game.");
            Log("!!! ARMHIDE:   AHands            expected 0x%X   ACTUAL 0x%X",
                (unsigned)g_cfg.armHideHandsVt, VtableRvaOf(hands));
            Log("!!! ARMHIDE:   SkeletonInstance  expected 0x%X   ACTUAL 0x%X",
                (unsigned)g_cfg.armHideSkelVt, VtableRvaOf(skel));
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
        // NEW SKELETON. Every saved reference pose belonged to the old one and
        // is now meaningless -- restoring through a stale pointer after a level
        // change silently corrupts reused memory, which is the single most
        // expensive bug this file can produce. Drop the saves, do not replay
        // them.
        g_actor = hands; g_skeleton = skel; g_bones = bones; g_boneCount = count;
        ClearSaved();
        ClearHandSaved();
        MotionReset();      // the previous sample described a different rig
        LeftClusterReset(); // and so did the reference pose
        Log(">>> ARMHIDE: skeleton locked: actor=0x%08X skel=0x%08X bones=0x%08X count=%d",
            (unsigned)(uintptr_t)hands, (unsigned)(uintptr_t)skel,
            (unsigned)(uintptr_t)bones, count);
    }
    return true;
}

// ===========================================================================
//  M7-S4: MOTION SAMPLING AND WHOLE-ACTOR HIDING
//
// Both exist to answer one question the script cannot: is the rig ACTUALLY
// animating right now? See ArmHide.h for why the flags were falsified.
// ===========================================================================

static const int      kMotionBone = kRightWrist;   // 27
static const unsigned kDrawScale3DOff = 0x2B0;     // X/Y/Z floats, measured
static const float    kHiddenScale = 0.0001f;      // NEVER exactly zero

static float g_motPrevPos[3] = {};
static float g_motPrevRot[4] = {};
static bool  g_motHave = false;
static float g_motSmoothed = 0.0f;

static void MotionReset()
{
    g_motHave = false;
    g_motSmoothed = 0.0f;
}

bool ArmHide_HandMotion(float* outSmoothed, float* outRaw)
{
    if (!g_bones || kMotionBone >= g_boneCount) return false;

    BoneTransform cur = {};
    if (!SafeRead(&g_bones[kMotionBone], &cur, sizeof(cur))) return false;

    float raw = 0.0f;
    if (g_motHave)
    {
        const float dx = cur.position[0] - g_motPrevPos[0];
        const float dy = cur.position[1] - g_motPrevPos[1];
        const float dz = cur.position[2] - g_motPrevPos[2];
        const float dPos = sqrtf(dx * dx + dy * dy + dz * dz);

        // Quaternion difference: 1 - |dot| is 0 for an identical orientation and
        // grows with the angle between them. A wrist can rotate in place without
        // its position moving at all, so position alone would miss it.
        float dot = 0.0f;
        for (int i = 0; i < 4; ++i) dot += cur.rotation[i] * g_motPrevRot[i];
        if (dot < 0.0f) dot = -dot;
        const float dRot = 1.0f - ((dot > 1.0f) ? 1.0f : dot);

        // Scaled so a small rotation is comparable to a small translation. The
        // constant is arbitrary; the LOGGED components are what calibrate it.
        raw = dPos + dRot * 50.0f;
    }

    memcpy(g_motPrevPos, cur.position, sizeof(g_motPrevPos));
    memcpy(g_motPrevRot, cur.rotation, sizeof(g_motPrevRot));
    g_motHave = true;

    // Peak-hold with decay. A single frame of motion should not be lost between
    // samples, and an animation that eases in and out should not chatter.
    g_motSmoothed *= 0.90f;
    if (raw > g_motSmoothed) g_motSmoothed = raw;

    if (outSmoothed) *outSmoothed = g_motSmoothed;
    if (outRaw)      *outRaw = raw;
    return true;
}

static void*  g_scaleActor = nullptr;
static float  g_savedScale3D[3] = {};
static bool   g_scaleSaved = false;
static bool   g_actorHidden = false;

void ArmHide_SetActorHidden(void* handsActor, bool hidden)
{
    // Actor changed: the saved scale belonged to an object that may already be
    // destroyed and its address reused. Drop it WITHOUT restoring, the same
    // reasoning ArmHide_Reset documents.
    if (handsActor != g_scaleActor)
    {
        g_scaleActor = handsActor;
        g_scaleSaved = false;
        g_actorHidden = false;
    }
    if (!handsActor) return;

    uint8_t* const p = (uint8_t*)handsActor + kDrawScale3DOff;

    if (hidden && !g_actorHidden)
    {
        float cur[3] = {};
        if (!SafeRead(p, cur, sizeof(cur))) return;

        // Refuse to save a scale that is already collapsed -- restoring THAT
        // would leave the hands invisible permanently.
        if (cur[0] <= kHiddenScale * 10.0f) return;

        memcpy(g_savedScale3D, cur, sizeof(g_savedScale3D));
        g_scaleSaved = true;

        const float tiny[3] = { kHiddenScale, kHiddenScale, kHiddenScale };
        if (SafeWrite(p, tiny, sizeof(tiny))) g_actorHidden = true;
    }
    else if (!hidden && g_actorHidden)
    {
        if (g_scaleSaved) SafeWrite(p, g_savedScale3D, sizeof(g_savedScale3D));
        g_actorHidden = false;
    }
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

// ===========================================================================
//  INACTIVE HAND
//
//  BioShock only ever uses one hand at a time -- the weapon hand or the
//  plasmid hand. The other one is still animated and still drawn, hanging in
//  the view doing nothing. This collapses it entirely.
//
//  Same technique as the sleeve pass, three differences:
//
//    1. The whole cluster goes, not just the forearm.
//    2. Bone 43 is MOVED, never scaled. Zeroing the weapon attachment's scale
//       makes the attachment path divide by zero and fling the weapon through
//       the near plane -- exactly the failure the sleeve pass avoids by
//       leaving 43 alone entirely.
//    3. It has to be UNDONE when the player switches hands, and undone from a
//       real engine pose. Driving a hand whose scale is still zero moves an
//       invisible hand around.
// ===========================================================================

static void RestoreHand()
{
    if (!g_skeleton || !g_bones || g_hiddenHand < 0) return;

    for (int i = 0; i < 24; ++i)
    {
        const SavedBone& s = g_handSaved[i];
        if (!s.valid || s.index < 0 || s.index >= g_boneCount) continue;
        SafeWrite(g_bones[s.index].position, s.position, sizeof(s.position));
        SafeWrite(g_bones[s.index].scale, s.scale, sizeof(s.scale));
    }

    ClearHandSaved();
    SetDirty(1);          // fresh engine pose for the hand coming back
    Log(">>> HANDHIDE: inactive hand restored, engine evaluation requested.");
}

static bool HideBone(int idx, const float target[3])
{
    if (!g_bones || idx < 0 || idx >= g_boneCount) return false;

    BoneTransform cur = {};
    if (!SafeRead(&g_bones[idx], &cur, sizeof(cur))) return false;

    // Same rule as the sleeve pass: only ever save a pose the ENGINE wrote.
    // The second eye would otherwise save our own zeroes over the reference.
    if (ScaleLooksNormal(cur.scale))
    {
        SavedBone* s = HandSlotFor(idx);
        if (s)
        {
            memcpy(s->position, cur.position, sizeof(s->position));
            memcpy(s->scale, cur.scale, sizeof(s->scale));
            s->valid = true;
        }
    }

    if (idx == kWeaponAttachBone)
    {
        // MOVE, do not scale. See the note above.
        return SafeWrite(g_bones[idx].position, kFarBelow, sizeof(kFarBelow));
    }

    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    if (!SafeWrite(g_bones[idx].position, target, sizeof(float) * 3)) return false;
    if (!SafeWrite(g_bones[idx].scale, zero, sizeof(zero))) return false;
    return true;
}

// hand = HAND_LEFT / HAND_RIGHT: which hand to HIDE. -1 hides neither.
static bool HideHand(int hand)
{
    if (hand != HAND_LEFT && hand != HAND_RIGHT) return false;

    const int first = (hand == HAND_LEFT) ? kLeftClusterFirst : kRightClusterFirst;
    const int last = (hand == HAND_LEFT) ? kLeftClusterLast : kRightClusterLast;
    const int* sleeve = (hand == HAND_LEFT) ? kLeftSleeve : kRightSleeve;
    const int  wrist = (hand == HAND_LEFT) ? kLeftWrist : kRightWrist;

    // Everything collapses onto the wrist, which is where the sleeve pass
    // already anchors. Reading it BEFORE any write means the anchor is a real
    // engine position rather than one of our own zeroes.
    if (wrist < 0 || wrist >= g_boneCount) return false;
    BoneTransform anchor = {};
    if (!SafeRead(&g_bones[wrist], &anchor, sizeof(anchor))) return false;

    bool any = false;
    for (int b = first; b <= last; ++b) any = HideBone(b, anchor.position) || any;
    for (int i = 0; i < 5; ++i)         any = HideBone(sleeve[i], anchor.position) || any;
    return any;
}

bool ArmHide_UpdateInactiveHand(void* handsActor, int activeHand)
{
    // The per-slot decision is made by the caller now, so this must not also
    // gate on the global -- a slot override of 1 with the global at 0 would
    // otherwise be ignored.
    if (!handsActor) return false;
    if (!LocateSkeleton(handsActor)) return false;

    if (activeHand != HAND_LEFT && activeHand != HAND_RIGHT)
    {
        RestoreHand();
        return false;
    }

    const int inactive = 1 - activeHand;

    // SWITCHED HANDS. Put the old one back from its reference pose first --
    // otherwise it starts being driven again while its scale is still zero,
    // and it moves around invisibly forever.
    if (g_hiddenHand >= 0 && g_hiddenHand != inactive)
    {
        RestoreHand();
        Log(">>> HANDHIDE: active hand changed -> now hiding %s",
            inactive == HAND_LEFT ? "LEFT" : "RIGHT");
    }

    if (!HideHand(inactive)) return false;

    g_hiddenHand = inactive;
    SetDirty(0);          // stop the render pass rebuilding over us this frame

    if (!g_loggedHandOk)
    {
        g_loggedHandOk = true;
        Log(">>> HANDHIDE: inactive %s hand collapsed (bone %d moved, not scaled).",
            inactive == HAND_LEFT ? "left" : "right", kWeaponAttachBone);
    }
    return true;
}

void ArmHide_ReleaseInactiveHand()
{
    // Only ever restore through a skeleton we have just re-verified. A pointer
    // from the previous level looks valid and writes into reused memory.
    if (g_hiddenHand < 0) return;
    if (!g_skeleton || !g_bones) { ClearHandSaved(); return; }
    RestoreHand();
}

// ===========================================================================
//  M6-S1: THE RIG PROBE -- READ ONLY, AND IT WRITES NOTHING
//
// M6 builds a rigid transform on one bone cluster, and the whole design rests
// on the render bone array being in a COMMON (MODEL) SPACE rather than
// parent-relative. That has never been measured. It is INFERRED, from
// CollapseBone(idx, wrist.position) pinning sleeve bones at ANOTHER bone's
// position -- which would be meaningless parent-relative -- and from kFarBelow
// working the same way. Strong, but inference.
//
// So measure it before building on it. Four questions, one dump:
//
//   1. MODEL SPACE OR PARENT-RELATIVE. The two wrists (6 and 27) must be an
//      anatomical distance apart on one lane. Parent-relative gives two small
//      vectors that look alike.
//   2. WHICH LANE IS WHICH. Lateral is the lane whose SIGN FLIPS between the
//      wrists. The other two are shared, and the actor origin sits at the eye
//      with the arms authored forward and down -- so forward is the large
//      shared positive and down the large shared negative.
//   3. UNITS. Wrist 6 -> bone 21, the far end of the same cluster, is a hand.
//      Wrist to wrist is a shoulder span. Both against centimetres.
//   4. IS THE ARRAY LIVE. ArmHide clears the dirty byte on every CalcView while
//      the sleeves are hidden, so the reference pose the cluster transform
//      captures may be a FROZEN one. Repeated dumps answer it directly.
//
// Bone 43 is read like any other. Reading it is not the hazard; scaling it is.
//
// ---- IT WAITS FOR A RIG THIS FILE HAS NOT ALREADY COLLAPSED --------------
// The dump would otherwise fire on skeleton lock, which happens DURING THE
// LOAD, holding whatever weapon the save spawns with. On any slot that hides
// the inactive hand the left cluster is pinned at the wrist with zero scale --
// so the lateral measurement would read OUR OWN WRITES and confidently report a
// separation of zero. Same trap as M7-S4 in a new place: you cannot hide a bone
// and measure it at the same time.
//
// So the run holds until the left cluster carries an engine scale again, which
// is what equipping the shotgun or the machine gun does. No timing is asked of
// the tester -- put the two-handed weapon up whenever, and the dumps follow.
//
// The two SLEEVE bones in the list (3 and 24) are deliberate and are a
// self-check, not data: ArmHide collapses them, so they should read exactly the
// wrist position at zero scale. If they do, this probe is demonstrably reading
// the same array ArmHide writes to.
// ===========================================================================

static const int kProbeBones[] = { 0, 3, 6, 21, 24, 27, 44 };
static const int kProbeDumpsWanted = 6;      // one on lock, then every 2 s
static const int kProbeIntervalMs = 2000;

static void* g_probeSkel = nullptr;
static int   g_probeDumps = 0;
static DWORD g_probeLast = 0;
static float g_probePrev27[3] = {};
static bool  g_probeHavePrev = false;

static bool ProbeRead(int idx, BoneTransform* out)
{
    if (!g_bones || idx < 0 || idx >= g_boneCount) return false;
    return SafeRead(&g_bones[idx], out, sizeof(BoneTransform));
}

void ArmHide_RigProbe(void* handsActor)
{
    if (!g_cfg.handRigProbe || !handsActor) return;
    if (!LocateSkeleton(handsActor)) return;

    // A new skeleton is a new rig and a new question. Start the run again.
    if (g_skeleton != g_probeSkel)
    {
        g_probeSkel = g_skeleton;
        g_probeDumps = 0;
        g_probeLast = 0;
        g_probeHavePrev = false;
    }
    if (g_probeDumps >= kProbeDumpsWanted) return;

    const DWORD now = GetTickCount();
    if (g_probeDumps > 0 && (now - g_probeLast) < (DWORD)kProbeIntervalMs) return;

    // WAIT FOR BOTH HANDS. See the banner: a collapsed left cluster makes the
    // whole measurement read our own writes. Checked on the wrist and on the far
    // end of the cluster, because the inactive-hand pass zeroes both.
    {
        BoneTransform w = {}, f = {};
        const bool ready = ProbeRead(kLeftWrist, &w) && ProbeRead(kLeftClusterLast, &f) &&
            ScaleLooksNormal(w.scale) && ScaleLooksNormal(f.scale);
        if (!ready)
        {
            static DWORD lastWait = 0;
            if (now - lastWait >= 10000)
            {
                lastWait = now;
                Log(">>> RIGPROBE: waiting -- the left hand is collapsed by this mod, so its");
                Log(">>> RIGPROBE: bones are ours, not the engine's. Equip the SHOTGUN or the");
                Log(">>> RIGPROBE: MACHINE GUN (both keep two hands) and the dumps start.");
            }
            return;
        }
    }

    g_probeLast = now;
    ++g_probeDumps;

    Log(">>> RIGPROBE: dump %d of %d   bone: pos x,y,z | quat x,y,z,w | scale x,y,z",
        g_probeDumps, kProbeDumpsWanted);

    for (int i = 0; i < (int)(sizeof(kProbeBones) / sizeof(kProbeBones[0])); ++i)
    {
        const int idx = kProbeBones[i];
        BoneTransform b = {};
        if (!ProbeRead(idx, &b))
        {
            Log(">>> RIGPROBE:   bone %2d  UNREADABLE", idx);
            continue;
        }
        Log(">>> RIGPROBE:   bone %2d  %9.2f %9.2f %9.2f | %6.3f %6.3f %6.3f %6.3f | %5.2f %5.2f %5.2f",
            idx,
            b.position[0], b.position[1], b.position[2],
            b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3],
            b.scale[0], b.scale[1], b.scale[2]);
    }

    // The derived numbers, so the answer is IN THE LOG rather than waiting on
    // arithmetic done from memory afterwards.
    //
    // NOT measured against the clavicle, which would be the obvious choice: bone
    // 3 is a SLEEVE bone and this file has already collapsed it onto the wrist.
    // Every length here is between bones the sleeve pass leaves alone.
    BoneTransform wl = {}, wr = {}, fl = {}, root = {};
    const bool haveWl = ProbeRead(kLeftWrist, &wl);
    const bool haveWr = ProbeRead(kRightWrist, &wr);
    const bool haveFl = ProbeRead(kLeftClusterLast, &fl);    // bone 21, far end
    const bool haveRoot = ProbeRead(0, &root);

    if (haveWl && haveWr)
    {
        const float dx = wr.position[0] - wl.position[0];
        const float dy = wr.position[1] - wl.position[1];
        const float dz = wr.position[2] - wl.position[2];
        Log(">>> RIGPROBE:   wrist27 - wrist6 = %.2f %.2f %.2f   len %.2f   "
            "(the lane that DOMINATES here is lateral)",
            dx, dy, dz, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    if (haveWl && haveFl)
    {
        const float dx = fl.position[0] - wl.position[0];
        const float dy = fl.position[1] - wl.position[1];
        const float dz = fl.position[2] - wl.position[2];
        Log(">>> RIGPROBE:   bone21 - wrist6 = %.2f %.2f %.2f   len %.2f   "
            "(across one hand: ~8-15 means centimetres)",
            dx, dy, dz, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    if (haveWl && haveRoot)
    {
        const float dx = wl.position[0] - root.position[0];
        const float dy = wl.position[1] - root.position[1];
        const float dz = wl.position[2] - root.position[2];
        Log(">>> RIGPROBE:   wrist6 - bone0 = %.2f %.2f %.2f   len %.2f   "
            "(bone 0 near the origin means a shared frame, not parent-relative)",
            dx, dy, dz, sqrtf(dx * dx + dy * dy + dz * dz));
    }

    // LIVENESS. Not a nicety: if the array is frozen by our own dirty-byte
    // clear during ordinary play, the cluster transform captures a frozen
    // reference and every later measurement inherits that.
    if (haveWr)
    {
        if (g_probeHavePrev)
        {
            const float dx = wr.position[0] - g_probePrev27[0];
            const float dy = wr.position[1] - g_probePrev27[1];
            const float dz = wr.position[2] - g_probePrev27[2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            Log(">>> RIGPROBE:   bone 27 moved %.4f since the last dump -> %s",
                d, (d > 0.0001f) ? "the array is LIVE" : "the array is FROZEN");
        }
        memcpy(g_probePrev27, wr.position, sizeof(g_probePrev27));
        g_probeHavePrev = true;
    }

    if (g_probeDumps >= kProbeDumpsWanted)
        Log(">>> RIGPROBE: done. Set HandRigProbe=0 to silence this.");
}

// ===========================================================================
//  M6-S1: THE LEFT HAND CLUSTER
//
// A RIGID TRANSFORM ON SIXTEEN BONES. Capture the cluster's pose once, then
// each frame rewrite every bone as  target x (ref_wrist^-1 x ref_bone)  so the
// hand keeps its own shape while the wrist goes where the controller is.
//
// WHY THIS IS THE WHOLE OF M6. Left-handed mode, detached hands and a
// two-handed grip are the same write with a different target. Build it once.
//
// WHAT M6-S1 MEASURED, and why this code is shaped the way it is:
//
//   - The array is in a COMMON MODEL SPACE. Confirmed, not inferred: bones are
//     spread across a shared frame, tens of units apart.
//   - The LEFT cluster is STATIC. Bones 6 and 21 read byte-identical across
//     four dumps spanning eight seconds while the right cluster moved every
//     time. A rigid transform here fights no animation.
//   - The array stays LIVE even though this file clears the dirty byte every
//     CalcView -- bone 27 moved between every pair of dumps. The engine
//     re-flags and re-evaluates each tick; our writes stick because they land
//     after evaluation. So this does NOT freeze the gun hand.
//
// LEFT CLUSTER ONLY, BONES 6-21, and that is a structural guard rather than a
// runtime one: bone 43, the weapon attachment, lives in the RIGHT cluster and
// can never be reached from here. Nothing in this section writes SCALE at all,
// so the inverse-decomposition that makes 43 dangerous is unreachable twice
// over.
//
// QUATERNION ORDER is assumed x,y,z,w -- Havok's, and the same order HandPose
// uses. Position mode does not depend on it; rotation mode is where it gets
// tested. If mode 2 produces a hand at a wild angle while mode 1 is correct,
// suspect this before anything else.
// ===========================================================================

static const int kLeftCount = kLeftClusterLast - kLeftClusterFirst + 1;   // 16

struct ClusterBone
{
    float position[3];
    float rotation[4];
};

static ClusterBone g_leftRef[kLeftCount] = {};
static bool  g_leftRefValid = false;
static bool  g_leftDriven = false;

static void LeftClusterReset()
{
    g_leftRefValid = false;
    g_leftDriven = false;
}

static void QMul(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void QRotate(const float q[4], const float v[3], float out[3])
{
    // v + 2w(q x v) + 2(q x (q x v)) -- no matrix, no normalisation assumed
    const float cx = q[1] * v[2] - q[2] * v[1];
    const float cy = q[2] * v[0] - q[0] * v[2];
    const float cz = q[0] * v[1] - q[1] * v[0];

    const float ccx = q[1] * cz - q[2] * cy;
    const float ccy = q[2] * cx - q[0] * cz;
    const float ccz = q[0] * cy - q[1] * cx;

    out[0] = v[0] + 2.0f * (q[3] * cx + ccx);
    out[1] = v[1] + 2.0f * (q[3] * cy + ccy);
    out[2] = v[2] + 2.0f * (q[3] * cz + ccz);
}

// Refreshed every frame while we are NOT driving, frozen while we are. The
// authored pose changes on a weapon switch, and a reference from the wrong
// weapon would put the fingers in the wrong shape.
//
// ALL SIXTEEN BONES MUST LOOK ENGINE-WRITTEN OR NOTHING IS CAPTURED. A cluster
// mid-hide has zero scales, and capturing that would save OUR values as the
// reference -- the same trap the sleeve and inactive-hand passes avoid with the
// same test.
static bool CaptureLeftRef()
{
    if (g_leftDriven && g_leftRefValid) return true;
    if (!g_bones || kLeftClusterLast >= g_boneCount) return false;

    ClusterBone tmp[kLeftCount] = {};
    for (int i = 0; i < kLeftCount; ++i)
    {
        BoneTransform b = {};
        if (!SafeRead(&g_bones[kLeftClusterFirst + i], &b, sizeof(b))) return false;
        if (!ScaleLooksNormal(b.scale)) return false;
        memcpy(tmp[i].position, b.position, sizeof(tmp[i].position));
        memcpy(tmp[i].rotation, b.rotation, sizeof(tmp[i].rotation));
    }

    memcpy(g_leftRef, tmp, sizeof(g_leftRef));
    g_leftRefValid = true;
    return true;
}

// targetPos is model space. targetQuat may be null, which leaves every bone at
// its authored orientation and slides the cluster bodily -- position mode.
static bool WriteLeftCluster(const float targetPos[3], const float targetQuat[4])
{
    const ClusterBone& wrist = g_leftRef[kLeftWrist - kLeftClusterFirst];

    float qDelta[4] = { 0.f, 0.f, 0.f, 1.f };
    if (targetQuat)
    {
        const float invWrist[4] = { -wrist.rotation[0], -wrist.rotation[1],
                                    -wrist.rotation[2],  wrist.rotation[3] };
        QMul(targetQuat, invWrist, qDelta);
    }

    bool any = false;
    for (int i = 0; i < kLeftCount; ++i)
    {
        const int idx = kLeftClusterFirst + i;

        float rel[3] = { g_leftRef[i].position[0] - wrist.position[0],
                         g_leftRef[i].position[1] - wrist.position[1],
                         g_leftRef[i].position[2] - wrist.position[2] };
        if (targetQuat)
        {
            float rot[3];
            QRotate(qDelta, rel, rot);
            rel[0] = rot[0]; rel[1] = rot[1]; rel[2] = rot[2];
        }

        const float pos[3] = { targetPos[0] + rel[0],
                               targetPos[1] + rel[1],
                               targetPos[2] + rel[2] };
        if (!SafeWrite(g_bones[idx].position, pos, sizeof(pos))) continue;

        if (targetQuat)
        {
            float q[4];
            QMul(qDelta, g_leftRef[i].rotation, q);
            SafeWrite(g_bones[idx].rotation, q, sizeof(q));
        }
        any = true;
    }

    if (!any) return false;

    // ---- THE SLEEVE HAS TO COME WITH IT ---------------------------------
    // MEASURED, first headset run: a sharp stretched spike out of the palm,
    // fixed at one end and following the RIGHT hand in rotation mode while the
    // rest of the hand followed the left. That is the signature of vertices
    // weighted across a bone we moved and a bone we did not.
    //
    // The five left sleeve bones are the ones we did not. `CollapseArm` pins
    // them at the wrist every frame -- but it runs in the ARMS block, BEFORE
    // this does, and it reads the wrist the ENGINE just wrote, which is the hand
    // still hanging at your side. So the forearm end stayed behind while the
    // hand walked off, and the skin stretched between the two.
    //
    // Pin them at the wrist we ACTUALLY put there. Position only: the sleeve
    // pass has already zeroed their scale, so their orientation cannot matter,
    // and their scale is not ours to touch.
    if (g_hidden)
        for (int i = 0; i < 5; ++i)
        {
            const int idx = kLeftSleeve[i];
            if (idx >= 0 && idx < g_boneCount)
                SafeWrite(g_bones[idx].position, targetPos, sizeof(float) * 3);
        }

    SetDirty(0);          // stop the render pass rebuilding over us this frame
    g_leftDriven = true;
    return true;
}

bool ArmHide_DriveLeftCluster(void* handsActor, const float targetPos[3],
    const float targetQuat[4])
{
    if (!handsActor || !targetPos) return false;
    if (!LocateSkeleton(handsActor)) return false;
    if (!CaptureLeftRef()) return false;

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log(">>> LEFTHAND: cluster reference captured, bones %d-%d. Tracking %s.",
            kLeftClusterFirst, kLeftClusterLast,
            targetQuat ? "position and rotation" : "position only");
    }
    return WriteLeftCluster(targetPos, targetQuat);
}

// ---- MODE 3: WHICH MODEL LANE IS WHICH ----------------------------------
// The axis map is a PREDICTION read off the M6-S1 rest pose, and this is what
// makes a wrong prediction cost an ini edit instead of a build. It ignores the
// controller entirely and slides the cluster along one model lane at a time, so
// the tester can simply say which way the hand went.
bool ArmHide_SweepLeftCluster(void* handsActor)
{
    if (!handsActor) return false;
    if (!LocateSkeleton(handsActor)) return false;
    if (!CaptureLeftRef()) return false;

    static const float kSweepAmp = 25.0f;      // model units, ~20 cm rendered
    static const DWORD kSweepMs = 3000;

    const int lane = (int)((GetTickCount() / kSweepMs) % 3);

    static int lastLane = -1;
    if (lane != lastLane)
    {
        lastLane = lane;
        Log(">>> LEFTHAND SWEEP: now pushing model lane %d by +%.0f. "
            "Which way did the hand move?", lane, kSweepAmp);
    }

    const ClusterBone& wrist = g_leftRef[kLeftWrist - kLeftClusterFirst];
    float target[3] = { wrist.position[0], wrist.position[1], wrist.position[2] };
    target[lane] += kSweepAmp;

    return WriteLeftCluster(target, nullptr);
}

void ArmHide_ReleaseLeftCluster()
{
    if (!g_leftDriven) return;
    g_leftDriven = false;

    // Only ever restore through a skeleton we still hold. A pointer from the
    // previous level looks valid and writes into reused memory -- the same scar
    // ArmHide_Reset and ArmHide_ReleaseInactiveHand both carry.
    if (!g_skeleton || !g_bones || !g_leftRefValid) return;
    if (kLeftClusterLast >= g_boneCount) return;

    for (int i = 0; i < kLeftCount; ++i)
    {
        const int idx = kLeftClusterFirst + i;
        SafeWrite(g_bones[idx].position, g_leftRef[i].position, sizeof(g_leftRef[i].position));
        SafeWrite(g_bones[idx].rotation, g_leftRef[i].rotation, sizeof(g_leftRef[i].rotation));
    }

    SetDirty(1);          // hand the rig back, and keep M7's motion signal honest
    Log(">>> LEFTHAND: cluster released, engine evaluation requested.");
}

void ArmHide_Reset()
{
    g_actor = nullptr;
    g_skeleton = nullptr;
    g_bones = nullptr;
    g_boneCount = 0;
    ClearSaved();
    ClearHandSaved();
    g_loggedFail = false;
    g_probeSkel = nullptr;      // a fresh rig deserves a fresh set of dumps
    LeftClusterReset();
}