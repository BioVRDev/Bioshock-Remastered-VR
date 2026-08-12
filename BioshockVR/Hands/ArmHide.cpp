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
static void ClusterStateReset();  // defined with the M6-S1 block below

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
        ClusterStateReset(); // and so did the reference poses
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

static const unsigned kDrawScale3DOff = 0x2B0;     // X/Y/Z floats, measured
static const float    kHiddenScale = 0.0001f;      // NEVER exactly zero

// Which cluster the free-hand drive is writing, if any. Declared here rather
// than beside the rest of the cluster state because the motion sampler below is
// the first thing that needs them.
// PER CLUSTER, indexed by hand. HAND_LEFT is 0 and HAND_RIGHT is 1, so the hand
// IS the index and there is no separate "which hand" tag to keep in step.
//
// IT USED TO BE ONE SET, and that was correct while exactly one cluster could be
// driven. C1 drives the weapon hand as well, and two callers sharing one buffer
// do something worse than fight over it: CaptureClusterRef's early-out would miss
// every frame, so each caller would re-capture its reference from a pose the
// OTHER one had just written. The rigid drive silently degrades back into a
// sway-follower, and nothing logs a complaint.
static bool g_clDriven[2] = { false, false };

// ===========================================================================
//  WHICH BONE THE MOTION GATE SAMPLES -- AND WHY IT CANNOT BE A CONSTANT
//
// It was kRightWrist (27), fixed. MEASURED 2026-08-11, three runs: during the
// plasmid balcony scene the value read EXACTLY 0.0000 for 189 consecutive
// samples in one run and 223 in another, and the arms stayed hidden for the
// whole scene.
//
// Bone 27 is the RIGHT wrist, and a plasmid puts the weapon in your LEFT hand --
// so the free hand is the RIGHT one and the drive is writing bones 27-44. The
// gate was measuring the bone the mod itself writes. A rigid transform from a
// captured reference reproduces the IDENTICAL pose every frame while the
// controller is still, so the delta is not merely small, it is bit-for-bit zero.
//
// IT LOOKED LIKE A MOVEMENT-MODE BUG. Arms appeared in mode 2 and not in 0 or 3,
// which is coincidence at one run per mode: mode 2 was the run where the tester
// was head-steering and moving their right controller enough for bone 27 to
// move. Nothing about MovementMode reaches the bone array, and the mod's own log
// sequence at the scene start is identical in all three runs.
//
// SO SAMPLE THE WRIST OF THE CLUSTER WE ARE NOT DRIVING. It is still a hand
// bone -- which is what the signal is about -- and it is always the engine's.
// Bones 0-2 are the only ones outside every cluster and sleeve set, but they are
// root and spine and may not move during a HAND animation at all.
//
// This is docs/INVARIANTS.md's "you cannot hide by bone and measure by bone at
// the same time", second instance. The rule was written about hiding; DRIVING
// has exactly the same effect and the wording now covers both.
//
// C1 CAN DRIVE BOTH CLUSTERS, so for the first time there may be NO honest bone
// to return. Say so (-1) rather than handing back one of ours -- a caller that
// believes a driven bone reads exactly 0.0000 forever, which is the whole
// failure this function exists to prevent. The caller's job is to treat -1 as
// "cannot answer" and fail in the safe direction.
//
// The un-driven wrist stays honest even when that hand is HIDDEN: HideBone pins
// each cluster bone at the wrist's own position, so the wrist's write is a no-op
// and its rotation is never touched. A collapsed inactive hand still carries the
// engine's pose on the one bone this reads.
// ===========================================================================
static int MotionBone()
{
    if (!g_clDriven[HAND_RIGHT]) return kRightWrist;
    if (!g_clDriven[HAND_LEFT])  return kLeftWrist;
    return -1;                    // both are ours -- no honest bone exists
}

int ArmHide_MotionBone() { return MotionBone(); }

static float g_motPrevPos[3] = {};
static float g_motPrevRot[4] = {};
static bool  g_motHave = false;
static int   g_motBone = -1;       // which bone g_motPrev* describes
static float g_motSmoothed = 0.0f;

static void MotionReset()
{
    g_motHave = false;
    g_motBone = -1;
    g_motSmoothed = 0.0f;
}

bool ArmHide_HandMotion(float* outSmoothed, float* outRaw)
{
    const int bone = MotionBone();
    if (bone < 0)
    {
        // Both clusters are ours, so every hand bone reports our own transform.
        // Refuse rather than return a number that is guaranteed to be zero.
        // Throttled because the caller's structural guard should make this
        // unreachable -- if it ever prints, that guard has been broken.
        static DWORD lastBlind = 0;
        const DWORD now = GetTickCount();
        if (now - lastBlind >= 5000)
        {
            lastBlind = now;
            Log("!!! MOTION: both clusters driven -- no engine-owned wrist to "
                "measure. The scripted-window release is not standing down.");
        }
        return false;
    }
    if (!g_bones || bone >= g_boneCount) return false;

    BoneTransform cur = {};
    if (!SafeRead(&g_bones[bone], &cur, sizeof(cur))) return false;

    // A HAND SWITCH CHANGES WHICH BONE THIS IS, and the distance between two
    // different bones is not motion. Drop the history instead of measuring
    // across the change -- the same discipline as the dt guard in the turn
    // accumulator, and the reason g_motBone exists at all.
    if (bone != g_motBone)
    {
        g_motBone = bone;
        g_motHave = false;
        g_motSmoothed = 0.0f;
    }

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

// SKELETAL CYCLE 0 WIDENED THIS. 43 and 44 join because 43 is the weapon attach
// point and 44 is the tip of that chain -- the barrel axis for aim-down-sight and
// the wrench tip are the SAME read, which is what collapsed two planned cycles
// into one. The ten sleeve bones join because the visible-arms question needs
// their reference positions and taking them costs nothing while the rig is
// already being dumped. Read, never written.
static const int kProbeBones[] =
{
    0, 6, 21, 27, 43, 44,                  // root, wrists, cluster ends, tip
    3,  4,  5, 22, 23,                     // left sleeve
    24, 25, 26, 45, 46                     // right sleeve
};
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

// ---- CYCLE 0: THE BONE NAME MAP ----------------------------------------
// WHY IT IS WORTH HAVING. Every bone index in this file is hardcoded and
// validated only by COUNT -- kExpectedBones == 47. That is a proxy: any other
// 47-bone skeleton would pass it while every index meant something else. Names
// turn the check into an identity.
//
// THE CHAIN IS UNMEASURED AND IS THEREFORE WALKED, NOT TRUSTED. A second
// source puts a SharedSkeletonData at SkeletonInstance+0x08 and a name map at
// +0xAC (his write-up says +0xB4 and his source says +0xAC -- they disagree,
// which is exactly why this dumps rather than dereferences). Nothing here
// assumes the map's shape.
//
// SELF-VALIDATING, in this file's usual way: the TArray interpretation is only
// believed when its count EQUALS the bone count we already located by a
// different route. A wrong offset cannot produce that agreement.
//
// ONE SHOT, READ-ONLY, and every read goes through SafeRead. Worst case it logs
// four lines of hex that say "not this offset" and the next session reads them.
static void ProbeBoneNames()
{
    static bool done = false;
    if (done || !g_skeleton) return;
    done = true;

    void* shared = nullptr;
    if (!SafeRead((const uint8_t*)g_skeleton + 0x08, &shared, 4) || !shared)
    {
        Log(">>> RIGPROBE: name map -- skel+0x08 is null or unreadable.");
        return;
    }
    Log(">>> RIGPROBE: name map -- skel+0x08 = 0x%08X (SharedSkeletonData?)",
        (unsigned)(uintptr_t)shared);

    // Interpret +0xAC as a TArray {data, count, max}, the shape used everywhere
    // else in this engine, and believe it ONLY on the count agreeing.
    struct { void* data; int count; int maxN; } arr = {};
    if (!SafeRead((const uint8_t*)shared + 0xAC, &arr, sizeof(arr)))
    {
        Log(">>> RIGPROBE: name map -- +0xAC unreadable.");
        return;
    }

    Log(">>> RIGPROBE: name map -- +0xAC = { data 0x%08X, count %d, max %d }  "
        "bones located elsewhere = %d",
        (unsigned)(uintptr_t)arr.data, arr.count, arr.maxN, g_boneCount);

    if (arr.count != g_boneCount || !arr.data ||
        arr.maxN < arr.count || arr.count <= 0)
    {
        // NOT a failure to hide. The next session reads these 64 bytes and
        // finds the real offset without spending a headset cycle on it.
        Log(">>> RIGPROBE: name map -- count disagrees, so this is NOT the map. "
            "Dumping shared+0xA0..0xDF so the layout can be read by eye:");
        for (int line = 0; line < 4; ++line)
        {
            uint8_t b[16] = {};
            if (!SafeRead((const uint8_t*)shared + 0xA0 + line * 16, b, 16))
                break;
            Log(">>> RIGPROBE:   +0x%02X  %02X %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X %02X %02X %02X %02X",
                0xA0 + line * 16,
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }
        return;
    }

    // THE COUNT AGREED, SO +0xAC IS THE MAP. What it is an array OF was guessed
    // wrong first time round and this is the correction.
    //
    // MEASURED 2026-08-11: read as 4-byte FNames the entries came back
    // -1, 26659, 0, 0 / -1, 35607, 0, 1 / -1, 17127, 0, 2 -- a pattern with
    // PERIOD 4 whose last lane counts up. That is not 47 FNames, it is 47
    // records of at least 16 bytes with an index field, read four bytes at a
    // time. The count matching was real; the element type was not.
    //
    // SO STOP GUESSING AND DUMP. Raw bytes for the first four records, which is
    // enough to see the stride and find the name lane by eye -- the same move
    // that decoded the two interface getters from their instruction bytes. A
    // wrong element size costs a cycle; a hex dump costs four lines.
    Log(">>> RIGPROBE: name map -- count AGREES with %d bones, so +0xAC is the "
        "map. Raw bytes (the ELEMENT TYPE is what is still unknown):",
        g_boneCount);

    for (int line = 0; line < 8; ++line)
    {
        uint8_t b[16] = {};
        if (!SafeRead((const uint8_t*)arr.data + line * 16, b, 16)) break;
        Log(">>> RIGPROBE:   data+0x%02X  %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X",
            line * 16,
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }

    // The stride the period-4 pattern implies, tested outright: if element k
    // really is 16 bytes with its index at +0x0C, then lane 3 reads 0,1,2,3...
    // and this line says so in one place instead of leaving it to arithmetic.
    {
        bool ascending = true;
        for (int i = 0; i < arr.count && i < 16 && ascending; ++i)
        {
            int v = -1;
            if (!SafeRead((const uint8_t*)arr.data + (size_t)i * 16 + 0x0C, &v, 4))
            { ascending = false; break; }
            if (v != i) ascending = false;
        }
        Log(">>> RIGPROBE: name map -- stride-16 hypothesis: index lane at "
            "+0x0C %s", ascending ? "COUNTS UP 0,1,2,... -- CONFIRMED"
                                  : "does not count up -- wrong stride");
    }
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

    // Once per session, before the first dump -- it is an identity check on the
    // rig the dumps are about, so it belongs ahead of them in the log.
    ProbeBoneNames();

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

    // ---- CYCLE 0: THE BARREL AXIS ---------------------------------------
    // Bone 43 is the weapon attach point and 44 is the tip of that chain, so
    // 43->44 IS the barrel of the gun and the head of the wrench -- one read
    // serving both, which is why the wrench's separately-tuned WrenchTipOffset
    // turned out to be unnecessary.
    //
    // A SECOND SOURCE PREDICTS THIS NUMBER: an independently developed mod
    // against the same game records "bone 44 muzzle-ish tip at x=+71". If the
    // dominant lane below is x and the magnitude is near 71, the barrel axis is
    // confirmed by two projects that never shared code, and true aim-down-sight
    // stops being a guess. If it disagrees, THAT is the finding.
    {
        BoneTransform b43 = {}, b44 = {};
        if (ProbeRead(43, &b43) && ProbeRead(44, &b44))
        {
            const float dx = b44.position[0] - b43.position[0];
            const float dy = b44.position[1] - b43.position[1];
            const float dz = b44.position[2] - b43.position[2];
            Log(">>> RIGPROBE:   bone44 - bone43 = %.2f %.2f %.2f   len %.2f   "
                "(the BARREL AXIS; a second source predicts x ~ +71)",
                dx, dy, dz, sqrtf(dx * dx + dy * dy + dz * dz));
        }
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
//  M6-S1: THE FREE HAND CLUSTER
//
// A RIGID TRANSFORM ON ONE HAND'S BONES. Capture the cluster's pose once, then
// each frame rewrite every bone as  target x (ref_wrist^-1 x ref_bone)  so the
// hand keeps its own shape while the wrist goes where the controller is.
//
// WHY THIS IS THE WHOLE OF M6. Left-handed mode, detached hands and a
// two-handed grip are the same write with a different target. Build it once.
//
// EITHER CLUSTER. With a weapon the free hand is the LEFT one; with a plasmid
// the left hand holds the plasmid and drives the actor, so the free hand is the
// RIGHT one. Same write, and the caller says which.
//
// WHAT M6-S1 MEASURED, and why this code is shaped the way it is:
//
//   - The array is in a COMMON MODEL SPACE. Confirmed, not inferred: bones are
//     spread across a shared frame, tens of units apart.
//   - The idle LEFT cluster is STATIC. Bones 6 and 21 read byte-identical
//     across four dumps spanning eight seconds while the right cluster moved
//     every time. A rigid transform on an idle hand fights no animation.
//   - The array stays LIVE even though this file clears the dirty byte every
//     CalcView -- bone 27 moved between every pair of dumps. The engine
//     re-flags and re-evaluates each tick; our writes stick because they land
//     after evaluation. So this does NOT freeze the other hand.
//
// > ### BONE 43 IS IN THE RIGHT CLUSTER
// > It is the weapon attachment, and telekinesis release walks the attachment
// > path through it -- telekinesis being a PLASMID, which is exactly when this
// > code drives the right cluster. The two meet in the same window.
// >
// > The rule that keeps it safe is the one HideBone already proved: MOVE it,
// > never scale it, because the attachment path inverse-decomposes bone scale.
// > So bone 43 takes the cluster's POSITION and nothing else -- no rotation,
// > which is untested, and no scale, which is known fatal. Moving it keeps its
// > vertices with the hand instead of stretching a spike across the screen.
//
// Nothing in this section writes SCALE for any bone, on either hand.
//
// QUATERNION ORDER is assumed x,y,z,w -- Havok's, and the same order HandPose
// uses. Position mode does not depend on it; rotation mode is where it gets
// tested. If mode 2 produces a hand at a wild angle while mode 1 is correct,
// suspect this before anything else.
// ===========================================================================

// Sized for the larger cluster: left is 6-21 (16), right is 27-44 (18).
static const int kClusterMax = kRightClusterLast - kRightClusterFirst + 1;   // 18

struct ClusterBone
{
    float position[3];
    float rotation[4];
};

static ClusterBone g_clRef[2][kClusterMax] = {};
static bool  g_clRefValid[2] = { false, false };
// g_clDriven is declared UP with the motion sampler instead -- ArmHide_HandMotion
// has to know which clusters we are writing before it picks a bone to measure,
// and it runs ~700 lines above here.

// ---- WHICH ROLE OWNS WHICH CLUSTER --------------------------------------
// Two drivers now: the tracked free hand (M6-S1) and the weapon hand's rigid
// freeze (C1). A role holds a hand, or -1. This is what g_clHand used to be,
// split in two -- and it is the piece that stops one role releasing the other's
// cluster when the free hand changes sides on a plasmid equip.
//
// THE FREE HAND WINS EVERY TIE, BY CONSTRUCTION. g_freeHand is the mirror of the
// weapon hand so they cannot collide, but a one-frame HandsProbe_AbilityMode()
// flip could ask for it. Refusing the weapon role there is what keeps the
// M6-S1 behaviour the tester signed off exactly as it was.
enum { kRoleFree = 0 };
static int g_roleHand[1] = { -1 };

// The FREEZE role can own BOTH clusters at once, so it is a flag per hand rather
// than one hand per role.
//
// MEASURED, Build V: on the shotgun and the Tommy gun the left hand is neither
// hidden nor tracked (`HideInactiveHand2/5=0` -- both hands belong on the gun),
// so the ENGINE animates it. With the weapon cluster frozen beside it the tester
// saw exactly what that implies: *"the left hands on the tommy and shotgun ...
// werent attached and were animated separately."* Freezing a gun without
// freezing the hand that is also holding it just moves the mismatch.
static bool g_freezeOwns[2] = { false, false };

// M6-S2's grab point, latched only from frames the ENGINE owns the cluster.
// Declared here with the other cluster state because ClusterStateReset clears it.
static float g_anchorPos[2][3] = {};
static bool  g_anchorValid[2] = { false, false };

// C2 only. What the bone array read back immediately AFTER our own write, which
// is the thing HandAnim=1 compares against next frame.
//
// READ BACK RATHER THAN RECONSTRUCTED, deliberately. We do not write every lane
// -- bone 43's rotation is never ours, and position mode writes no rotation at
// all -- so a reconstruction would differ from the array in lanes we never
// touched and every frame would look like a restamp. Reading the array back
// captures our write AND the engine's untouched lanes in one shape, so an exact
// compare next frame means exactly "the engine re-evaluated".
static ClusterBone g_clLastWritten[2][kClusterMax] = {};

// Everything the cluster it belongs to needs, in one place, so no caller has to
// remember that the right hand's sleeve is a different five bones.
struct ClusterSpec
{
    int first, last, wrist, count;
    const int* sleeve;
};

static ClusterSpec SpecFor(int hand)
{
    if (hand == HAND_RIGHT)
        return { kRightClusterFirst, kRightClusterLast, kRightWrist,
                 kRightClusterLast - kRightClusterFirst + 1, kRightSleeve };
    return { kLeftClusterFirst, kLeftClusterLast, kLeftWrist,
             kLeftClusterLast - kLeftClusterFirst + 1, kLeftSleeve };
}

// EVERY SLOT, BOTH ROLES. Called from LocateSkeleton's re-lock branch and from
// ArmHide_Reset -- i.e. exactly when the rig underneath us has been replaced.
//
// MISSING A SLOT HERE IS THE CRASH. A role left holding a hand across a world
// change means the next release writes 18 bones of position and rotation through
// the PREVIOUS level's bone array. The second-source mod hung a save load doing
// precisely that. Clearing without restoring is deliberate and is the same
// reasoning ArmHide_Reset documents: the old actor may already be destroyed.
static void ClusterStateReset()
{
    for (int h = 0; h < 2; ++h)
    {
        g_clRefValid[h] = false;
        g_clDriven[h] = false;
        g_freezeOwns[h] = false;
        g_anchorValid[h] = false;      // a new rig is a new grab point
    }
    g_roleHand[kRoleFree] = -1;
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
// EVERY BONE MUST LOOK ENGINE-WRITTEN OR NOTHING IS CAPTURED. A cluster
// mid-hide has zero scales, and capturing that would save OUR values as the
// reference -- the same trap the sleeve and inactive-hand passes avoid with the
// same test.
//
// A reference belongs to ONE HAND. Switching from a weapon to a plasmid moves
// the free hand to the other side of the body, and replaying the left hand's
// pose onto the right cluster would be gibberish -- so the hand is part of the
// validity test, not just the pointer.
// THE EARLY-OUT MUST BE PER HAND, and this is load-bearing twice over. DriveHands
// runs once per EYE, so both drivers write twice per frame; if the early-out were
// shared, the second eye would re-capture a reference from the pose the first eye
// just wrote, and the freeze would quietly become a follower. ScaleLooksNormal
// does NOT catch that -- the cluster drive never writes scale.
static bool CaptureClusterRef(const ClusterSpec& c, int hand)
{
    if (g_clDriven[hand] && g_clRefValid[hand])
    {
        // ---- C2: ADOPTION, WHICH IS THIS POLICY INVERTED -----------------
        // HandAnim=0 is the freeze above and is what kills sway. HandAnim=1
        // takes the engine's pose whenever the array no longer holds what we
        // last wrote -- which means the engine restamped, and that restamp is
        // the authored animation (a reload, an idle, the drill) we want to keep.
        // Adopting it as the new reference replays it on a hand that is still
        // following your controller.
        //
        // EXACT COMPARE, NOT AN EPSILON. Our own write is bit-identical to what
        // we stored, so anything else is the engine by construction. An epsilon
        // would swallow small authored motion, which is precisely the breathing
        // this mode exists to preserve.
        //
        // THE PRECONDITION IS MEASURED, not assumed: docs/ENGINE-MAP.md
        // records that the array keeps evaluating in ordinary play with the
        // dirty byte cleared -- bone 27 moved between every pair of dumps.
        if (!g_cfg.handAnim) return true;

        ClusterBone live[kClusterMax] = {};
        for (int i = 0; i < c.count; ++i)
        {
            BoneTransform b = {};
            if (!SafeRead(&g_bones[c.first + i], &b, sizeof(b))) return true;
            if (!ScaleLooksNormal(b.scale)) return true;
            memcpy(live[i].position, b.position, sizeof(live[i].position));
            memcpy(live[i].rotation, b.rotation, sizeof(live[i].rotation));
        }
        if (memcmp(live, g_clLastWritten[hand],
                   sizeof(ClusterBone) * (size_t)c.count) == 0)
            return true;                       // untouched: still our own pose

        // ---- SWAY IS AN ANIMATION TOO, WHICH IS WHY A BARE ADOPT FAILS ----
        // REPORTED, Build V: "HandAnim=1 still has weapon sway." That is this
        // policy working exactly as built and it is not a bug -- the idle bob and
        // the reload are the same engine re-stamp arriving through the same bone
        // array, so "adopt whatever the engine wrote" adopts both. The channels
        // are separable in the SCRIPT (AdditiveHandBobAnim is channel 2, the rest
        // are channel 0); they are not separable here, because what we read is
        // already baked.
        //
        // SO SEPARATE THEM BY SIZE, which the measurements support: idle drift is
        // 1-5 deg (191 B43 samples), while a switch or a reload peaks at 41-135.
        // A threshold in that gap rejects breathing and admits real animation.
        //
        // AND HOLD, or a reload dies in its own middle: once the big opening
        // frame is adopted the following frames are small deltas again, so a bare
        // threshold would snap back to rigid mid-animation. Same peak-hold shape
        // as the M7 motion gate, and for the same reason.
        const ClusterBone& wr = live[c.wrist - c.first];
        const ClusterBone& wp = g_clLastWritten[hand][c.wrist - c.first];
        float dot = 0.0f;
        for (int i = 0; i < 4; ++i) dot += wr.rotation[i] * wp.rotation[i];
        if (dot < 0.0f) dot = -dot;
        if (dot > 1.0f) dot = 1.0f;
        const float deg = 2.0f * acosf(dot) * 57.2957795f;

        static DWORD lastBig[2] = { 0, 0 };
        const DWORD now = GetTickCount();
        if (deg == deg && deg >= (float)g_cfg.handAnimMinDeg) lastBig[hand] = now;

        const bool playing = lastBig[hand] &&
            (now - lastBig[hand]) < (DWORD)g_cfg.handAnimHoldMs;
        if (!playing) return true;             // breathing, not an animation

        memcpy(g_clRef[hand], live, sizeof(ClusterBone) * (size_t)c.count);

        static DWORD lastLog = 0;
        if (now - lastLog >= 2000)
        {
            lastLog = now;
            Log(">>> HANDANIM: cluster %d is animating (%.1f deg at the wrist, "
                "threshold %d) -- adopting the engine's pose.",
                hand, deg, g_cfg.handAnimMinDeg);
        }
        return true;
    }
    if (!g_bones || c.last >= g_boneCount) return false;

    ClusterBone tmp[kClusterMax] = {};
    for (int i = 0; i < c.count; ++i)
    {
        BoneTransform b = {};
        if (!SafeRead(&g_bones[c.first + i], &b, sizeof(b))) return false;
        if (!ScaleLooksNormal(b.scale)) return false;
        memcpy(tmp[i].position, b.position, sizeof(tmp[i].position));
        memcpy(tmp[i].rotation, b.rotation, sizeof(tmp[i].rotation));
    }

    memcpy(g_clRef[hand], tmp, sizeof(g_clRef[hand]));
    g_clRefValid[hand] = true;
    return true;
}

// targetPos is model space. targetQuat may be null, which leaves every bone at
// its authored orientation and slides the cluster bodily -- position mode.
static bool WriteCluster(const ClusterSpec& c, int hand,
    const float targetPos[3], const float targetQuat[4])
{
    const ClusterBone* ref = g_clRef[hand];
    const ClusterBone& wrist = ref[c.wrist - c.first];

    float qDelta[4] = { 0.f, 0.f, 0.f, 1.f };
    if (targetQuat)
    {
        const float invWrist[4] = { -wrist.rotation[0], -wrist.rotation[1],
                                    -wrist.rotation[2],  wrist.rotation[3] };
        QMul(targetQuat, invWrist, qDelta);
    }

    bool any = false;
    for (int i = 0; i < c.count; ++i)
    {
        const int idx = c.first + i;

        float rel[3] = { ref[i].position[0] - wrist.position[0],
                         ref[i].position[1] - wrist.position[1],
                         ref[i].position[2] - wrist.position[2] };
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

        // BONE 43 USED TO TAKE THE POSITION AND STOP THERE, and Build U measured
        // what that cost: with the rest of the cluster frozen, bone 43 was the
        // ONLY bone in 27-44 the engine still animated, and its rotation drifted
        // 1-5 deg at idle with peaks of 41, 77, 126 and 135. That is the whole of
        // the remaining gun sway, and it is why the gun stopped looking attached
        // to a hand that had gone rigid.
        //
        // So the rotation is now a written lane, behind WeaponHandBone43Rot.
        // WHAT IS FATAL HERE IS SCALE, because the attachment path
        // inverse-decomposes the bone and divides by it -- a denormal QUATERNION
        // is that same hazard's rotational shape, which is why the write is
        // refused unless |q| is 1. Four multiplies to keep this fail-closed.
        if (targetQuat && (idx != kWeaponAttachBone || g_cfg.bone43Rot))
        {
            float q[4];
            QMul(qDelta, ref[i].rotation, q);

            bool ok = true;
            if (idx == kWeaponAttachBone)
            {
                const float m = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
                ok = (m == m) && m > 0.998f && m < 1.002f;   // m==m rejects NaN
                if (!ok)
                {
                    static bool warned = false;
                    if (!warned)
                    {
                        warned = true;
                        Log("!!! B43: refusing a non-unit quaternion (|q|^2 = %.6f). "
                            "The attachment path divides through this bone.", m);
                    }
                }
                else
                {
                    static bool announced = false;
                    if (!announced)
                    {
                        announced = true;
                        Log(">>> B43: attach rotation is now WRITTEN "
                            "(WeaponHandBone43Rot=1). Untested path -- watch the gun "
                            "on a telekinesis release and on a weapon switch.");
                    }
                }
            }
            if (ok) SafeWrite(g_bones[idx].rotation, q, sizeof(q));
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
    // That hand's five sleeve bones are the ones we did not. `CollapseArm` pins
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
            const int idx = c.sleeve[i];
            if (idx >= 0 && idx < g_boneCount)
                SafeWrite(g_bones[idx].position, targetPos, sizeof(float) * 3);
        }

    SetDirty(0);          // stop the render pass rebuilding over us this frame
    g_clDriven[hand] = true;

    // C2's anchor. Only paid for when HandAnim is on -- it is one cluster read.
    if (g_cfg.handAnim)
        for (int i = 0; i < c.count; ++i)
        {
            BoneTransform b = {};
            if (!SafeRead(&g_bones[c.first + i], &b, sizeof(b))) break;
            memcpy(g_clLastWritten[hand][i].position, b.position,
                sizeof(g_clLastWritten[hand][i].position));
            memcpy(g_clLastWritten[hand][i].rotation, b.rotation,
                sizeof(g_clLastWritten[hand][i].rotation));
        }
    return true;
}

bool ArmHide_DriveFreeHand(void* handsActor, int hand,
    const float targetPos[3], const float targetQuat[4])
{
    if (!handsActor || !targetPos) return false;
    if (hand != HAND_LEFT && hand != HAND_RIGHT) return false;
    if (!LocateSkeleton(handsActor)) return false;

    const ClusterSpec c = SpecFor(hand);

    // SWITCHED HANDS -- put the old cluster back before adopting the new one.
    // Otherwise the hand we walk away from keeps the last pose we forced on it
    // for as long as the engine's own evaluation takes to win it back.
    //
    // Expressed against THIS ROLE rather than against a global driven flag: the
    // weapon role may legitimately be driving the other cluster at the same time,
    // and releasing it here would be releasing somebody else's hand.
    if (g_roleHand[kRoleFree] >= 0 && g_roleHand[kRoleFree] != hand)
        ArmHide_ReleaseFreeHand();

    if (!CaptureClusterRef(c, hand)) return false;
    g_roleHand[kRoleFree] = hand;

    static int loggedHand = -1;
    if (loggedHand != hand)
    {
        loggedHand = hand;
        Log(">>> FREEHAND: tracking the %s hand, bones %d-%d, %s.",
            hand == HAND_LEFT ? "LEFT" : "RIGHT", c.first, c.last,
            targetQuat ? "position and rotation" : "position only");
    }
    return WriteCluster(c, hand, targetPos, targetQuat);
}

// ---- MODE 3: WHICH MODEL LANE IS WHICH ----------------------------------
// The axis map is a PREDICTION read off the M6-S1 rest pose, and this is what
// makes a wrong prediction cost an ini edit instead of a build. It ignores the
// controller entirely and slides the cluster along one model lane at a time, so
// the tester can simply say which way the hand went.
bool ArmHide_SweepFreeHand(void* handsActor, int hand)
{
    if (!handsActor) return false;
    if (hand != HAND_LEFT && hand != HAND_RIGHT) return false;
    if (!LocateSkeleton(handsActor)) return false;

    const ClusterSpec c = SpecFor(hand);
    if (g_roleHand[kRoleFree] >= 0 && g_roleHand[kRoleFree] != hand)
        ArmHide_ReleaseFreeHand();
    if (!CaptureClusterRef(c, hand)) return false;
    g_roleHand[kRoleFree] = hand;

    static const float kSweepAmp = 25.0f;      // model units, ~20 cm rendered
    static const DWORD kSweepMs = 3000;

    const int lane = (int)((GetTickCount() / kSweepMs) % 3);

    static int lastLane = -1;
    if (lane != lastLane)
    {
        lastLane = lane;
        Log(">>> FREEHAND SWEEP: now pushing model lane %d by +%.0f. "
            "Which way did the hand move?", lane, kSweepAmp);
    }

    const ClusterBone& wrist = g_clRef[hand][c.wrist - c.first];
    float target[3] = { wrist.position[0], wrist.position[1], wrist.position[2] };
    target[lane] += kSweepAmp;

    return WriteCluster(c, hand, target, nullptr);
}

// ONE CLUSTER BACK TO THE ENGINE. Both roles release through here, so the
// lifetime guard below is written once and cannot drift between them.
static void ReleaseCluster(int hand, const char* who)
{
    if (hand != HAND_LEFT && hand != HAND_RIGHT) return;
    if (!g_clDriven[hand]) return;
    g_clDriven[hand] = false;

    // Only ever restore through a skeleton we still hold. A pointer from the
    // previous level looks valid and writes into reused memory -- the same scar
    // ArmHide_Reset and ArmHide_ReleaseInactiveHand both carry. The valid flag is
    // tested PER HAND: a shared one would let a cluster we never captured be
    // "restored" from another hand's reference.
    if (!g_skeleton || !g_bones || !g_clRefValid[hand]) return;

    const ClusterSpec c = SpecFor(hand);
    if (c.last >= g_boneCount) return;

    const ClusterBone* ref = g_clRef[hand];
    for (int i = 0; i < c.count; ++i)
    {
        const int idx = c.first + i;
        SafeWrite(g_bones[idx].position, ref[i].position, sizeof(ref[i].position));

        // GATED ON THE SAME KEY AS THE WRITE, and that is not symmetry for its
        // own sake. While bone 43's rotation was never ours, putting it back
        // would have been wrong -- a reference captured minutes ago is stale.
        // The moment we DO write it, the opposite is true: leaving it would
        // strand the gun at a frozen angle on every release, which is every pause
        // and every scripted scene. Off, this is byte-for-byte the old behaviour.
        if (idx != kWeaponAttachBone || g_cfg.bone43Rot)
            SafeWrite(g_bones[idx].rotation, ref[i].rotation, sizeof(ref[i].rotation));
    }

    SetDirty(1);          // hand the rig back, and keep M7's motion signal honest
    Log(">>> %s: cluster released, engine evaluation requested.", who);
}

void ArmHide_ReleaseFreeHand()
{
    const int hand = g_roleHand[kRoleFree];
    if (hand < 0) return;
    g_roleHand[kRoleFree] = -1;
    ReleaseCluster(hand, "FREEHAND");
}

// ===========================================================================
//  C1: THE WEAPON HAND, RIGID -- AND WHY THIS IS SIX LINES
//
// The gun sways because the game animates the skeleton UNDERNEATH the actor
// transform DriveHands writes. The free hand does not sway, and that was an
// accident of policy rather than a design: CaptureClusterRef freezes its
// reference while driving, and a frozen reference replayed every frame is a
// rigid hand.
//
// SO THE TARGET IS THE CLUSTER'S OWN REFERENCE WRIST. Feed WriteCluster the
// wrist's captured position AND rotation and qDelta collapses to identity, so
// every bone is rewritten exactly where the authored pose put it. That is
// "replay the frozen pose" -- the sway removed and nothing else moved.
//
// DELIBERATELY NOT DERIVED FROM THE CONTROLLER. The actor transform already
// carries this hand to where the controller is; re-deriving a wrist from the
// pose and handsGrip would be a SECOND frame conversion that can only disagree
// with the actor write, and every per-slot grip/cursor offset is tuned against
// the authored pose. Nothing here needs the axis map either.
//
// > ### BONE 43 IS THE ONE BONE THIS DOES NOT FREEZE
// > WriteCluster writes the weapon attach bone's position and never its
// > rotation -- position is verified through telekinesis release, SCALE is known
// > fatal (the attachment path divides by it), and rotation is untested. So
// > under this freeze bone 43 is the ONLY bone in 27-44 the engine still
// > animates, and if the weapon's rendered orientation comes from that
// > quaternion the gun keeps swaying inside a rigid hand.
// >
// > NOBODY HAS EVER LOGGED THAT QUATERNION, so this measures it instead of
// > guessing: read-only, throttled, peak-held. Under ~0.5 deg the question is
// > closed and no write is ever needed. Several degrees and the tester's "look
// > at the gun" verdict from the SAME launch says whether it is visible.
// ===========================================================================
static void Bone43Watch(int hand, const ClusterSpec& c)
{
    if (hand != HAND_RIGHT) return;                 // 43 is in the right cluster
    if (kWeaponAttachBone > c.last || !g_bones) return;

    BoneTransform live = {};
    if (!SafeRead(&g_bones[kWeaponAttachBone], &live, sizeof(live))) return;

    const float* q = g_clRef[hand][kWeaponAttachBone - c.first].rotation;
    float dot = 0.0f;
    for (int i = 0; i < 4; ++i) dot += live.rotation[i] * q[i];
    if (dot < 0.0f) dot = -dot;
    if (dot > 1.0f) dot = 1.0f;
    const float deg = 2.0f * acosf(dot) * 57.2957795f;
    if (deg != deg) return;                         // NaN guard

    static float peak = 0.0f;
    static DWORD lastLog = 0;
    if (deg > peak) peak = deg;

    const DWORD now = GetTickCount();
    if (now - lastLog < 1000) return;
    lastLog = now;
    Log(">>> B43: attach rotation drift  now %.2f deg  peak %.2f deg  "
        "(cluster frozen; this bone is still the engine's)", deg, peak);
    peak = 0.0f;
}

// ---- WHAT YOU ARE HOLDING CHANGED, SO THE POSE WE FROZE IS THE WRONG POSE ----
// MEASURED, Build U, 2026-08-12. The cluster was captured once and replayed
// through SEVEN weapon switches: `WEAPONHAND: cluster released` fired six times,
// each 1 ms after a `PAUSE: PAUSED`, and never on a switch. So the pistol's
// authored pose was replayed onto the machine gun and the shotgun, and pausing
// "fixed" it only because the pause released and the unpause re-captured.
//
// CaptureClusterRef's own comment called this in advance: "The authored pose
// changes on a weapon switch, and a reference from the wrong weapon would put the
// fingers in the wrong shape." The free hand never exposed it because the idle
// off-hand pose barely differs between weapons.
//
// AND THEN WAIT. Re-capturing on the change frame would freeze a pose taken
// mid-equip, which is the same bug wearing a different hat. Release, let the
// engine animate the draw it authored, and capture once it has settled.
// WeaponSwitchSettleMs is the one number in this build not backed by a
// measurement; a hand frozen mid-draw means it is too short.
static const void* g_wpPoseKey = nullptr;
static DWORD       g_wpKeyChanged = 0;
static bool        g_wpSettling = false;
static DWORD       g_wpLastMoved = 0;
static float       g_wpPrevWrist[3] = {};
static bool        g_wpHavePrev = false;

// How still the rig has to be, and for how long, before a pose counts as
// settled. A FIXED TIMER WAS NOT ENOUGH: 600 ms suited the shotgun and not the
// wrench, whose draw runs longer -- reported as *"when switching to the wrench
// one of the times the wrench model location was super off, but it fixed it
// cycling through the weapons."* That is a pose captured mid-draw.
//
// WeaponSwitchSettleMs is now the CEILING rather than the duration, so a weapon
// that settles fast is picked up fast and a slow one is still waited for.
static const float kSettleStillUnits = 0.05f;   // model units per frame
static const DWORD kSettleStillMs = 150;

bool ArmHide_PoseSettling() { return g_wpSettling; }

bool ArmHide_FreezeWeaponHand(void* handsActor, int hand, const void* poseKey)
{
    if (!handsActor) return false;
    if (hand != HAND_LEFT && hand != HAND_RIGHT) return false;
    if (!LocateSkeleton(handsActor)) return false;

    const DWORD now = GetTickCount();
    if (poseKey != g_wpPoseKey)
    {
        g_wpPoseKey = poseKey;
        // A DIFFERENT WEAPON HAS A DIFFERENT GRAB POINT. Drop the latch so it is
        // re-taken from the settled pose of what you are now holding, rather than
        // leaving the previous weapon's fore-end as the target.
        g_anchorValid[HAND_LEFT] = g_anchorValid[HAND_RIGHT] = false;
        ArmHide_ReleaseWeaponHand();      // stamps the settle window itself
        Log(">>> WEAPONHAND: what you are holding changed -- released for %d ms so "
            "the equip can play, then re-freezing on the NEW pose.",
            g_cfg.weaponSwitchSettleMs);
    }
    if (g_wpSettling)
    {
        // WAIT FOR THE RIG TO STOP MOVING, not for a fixed number of
        // milliseconds. Sample the wrist the engine is animating and call it
        // settled once it has been still briefly -- or once the ceiling expires,
        // so a permanently-moving rig cannot wedge this open.
        const ClusterSpec cs2 = SpecFor(hand);
        if (g_bones && cs2.wrist < g_boneCount)
        {
            BoneTransform w = {};
            if (SafeRead(&g_bones[cs2.wrist], &w, sizeof(w)))
            {
                if (g_wpHavePrev)
                {
                    const float dx = w.position[0] - g_wpPrevWrist[0];
                    const float dy = w.position[1] - g_wpPrevWrist[1];
                    const float dz = w.position[2] - g_wpPrevWrist[2];
                    if (sqrtf(dx * dx + dy * dy + dz * dz) > kSettleStillUnits)
                        g_wpLastMoved = now;
                }
                memcpy(g_wpPrevWrist, w.position, sizeof(g_wpPrevWrist));
                g_wpHavePrev = true;
            }
        }

        const bool still = g_wpLastMoved && (now - g_wpLastMoved) >= kSettleStillMs;
        const bool timedOut =
            (now - g_wpKeyChanged) >= (DWORD)g_cfg.weaponSwitchSettleMs;
        if (!still && !timedOut) return false;

        g_wpSettling = false;
        g_wpHavePrev = false;
        Log(">>> WEAPONHAND: pose settled after %u ms (%s) -- freezing from here.",
            (unsigned)(now - g_wpKeyChanged), still ? "rig went still" : "ceiling");
    }

    // THE FREE HAND WINS EVERY TIE. g_freeHand is the mirror of the weapon hand
    // so this cannot happen in steady state, but a one-frame ability-mode flip
    // could ask for it -- and M6-S1 is signed off, so it is the one that keeps
    // its cluster. Refusing is the fail-closed answer.
    if (g_roleHand[kRoleFree] == hand)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Log("!!! WEAPONHAND: the free hand already owns cluster %d. "
                "Refusing -- the tracked hand keeps it.", hand);
        }
        return false;
    }

    const ClusterSpec c = SpecFor(hand);

    // A HIDDEN cluster reads back OUR zeroes, and CaptureClusterRef refuses it on
    // ScaleLooksNormal -- so a hand the inactive-hand pass has collapsed simply
    // never freezes. That is the correct outcome and it costs no extra condition.
    if (!CaptureClusterRef(c, hand)) return false;
    g_freezeOwns[hand] = true;

    static bool loggedHand[2] = { false, false };
    if (!loggedHand[hand])
    {
        loggedHand[hand] = true;
        Log(">>> WEAPONHAND: freezing the %s cluster, bones %d-%d.",
            hand == HAND_LEFT ? "LEFT" : "RIGHT", c.first, c.last);
    }

    const ClusterBone& wrist = g_clRef[hand][c.wrist - c.first];
    const bool ok = WriteCluster(c, hand, wrist.position, wrist.rotation);
    if (ok) Bone43Watch(hand, c);
    return ok;
}

// ---- M6-S2: WHERE THE GAME PUTS THE OFF HAND ---------------------------
// The cluster's wrist in model space, LATCHED from a frame the engine owned.
//
// > ### IT CANNOT BE READ THROUGH CaptureClusterRef, AND THAT WAS A REAL BUG
// > The first version did exactly that, and it is wrong the moment anything
// > drives the cluster: CaptureClusterRef early-outs while driven and hands back
// > OUR pose, so the "authored grab point" silently became whatever we last froze
// > or tracked. REPORTED: *"when you grab the area, the hand attaches to way below
// > the grab point (~6-12 inches below) and doesnt attach to the spot it normally
// > does"* -- the hand was returning to a pose captured at a moment the arm was
// > hanging, and every grab thereafter reproduced it.
//
// So latch it only on frames the ENGINE owns the cluster -- before tracking
// starts, and throughout the post-equip settle window. The last refresh before we
// take the cluster is therefore the settled authored pose for THIS weapon, which
// is precisely the grab point. `ScaleLooksNormal` still refuses a collapsed
// cluster, so a hidden hand reports no anchor rather than a false one.
bool ArmHide_FreeHandAnchor(void* handsActor, int hand, float outModel[3])
{
    if (!handsActor || !outModel) return false;
    if (hand != HAND_LEFT && hand != HAND_RIGHT) return false;
    if (!LocateSkeleton(handsActor)) return false;

    const ClusterSpec c = SpecFor(hand);

    // REFRESH ONLY WHILE THE ENGINE OWNS IT. Once we drive or freeze the cluster
    // the array reports our own transform, and latching that would make the
    // anchor chase itself.
    if (!g_clDriven[hand] && g_bones && c.wrist < g_boneCount)
    {
        BoneTransform b = {};
        if (SafeRead(&g_bones[c.wrist], &b, sizeof(b)) && ScaleLooksNormal(b.scale))
        {
            memcpy(g_anchorPos[hand], b.position, sizeof(g_anchorPos[hand]));
            g_anchorValid[hand] = true;
        }
    }

    if (!g_anchorValid[hand])
    {
        // THE LATCH NEEDS A WINDOW WHERE THE ENGINE OWNS THE CLUSTER, and if the
        // caller never yields one this is silent forever. It was: Build Z tracked
        // the off hand from the first frame, so g_clDriven never went false, the
        // anchor was never taken, and the whole two-hand state machine returned
        // early with ZERO probe lines in a full session.
        static DWORD lastMoan = 0;
        const DWORD now = GetTickCount();
        if (now - lastMoan >= 5000)
        {
            lastMoan = now;
            Log("!!! TWOHAND: no grab point yet for cluster %d -- the engine has "
                "not owned it since the last weapon change. %s", hand,
                g_clDriven[hand] ? "We are driving it; the caller must stand down "
                                   "during the settle window." : "Waiting.");
        }
        return false;
    }
    memcpy(outModel, g_anchorPos[hand], sizeof(float) * 3);
    return true;
}

void ArmHide_ReleaseWeaponHand()
{
    bool releasedAny = false;
    for (int h = 0; h < 2; ++h)
    {
        if (!g_freezeOwns[h]) continue;
        g_freezeOwns[h] = false;
        ReleaseCluster(h, "WEAPONHAND");
        releasedAny = true;
    }
    if (!releasedAny) return;

    // ---- SETTLE AFTER *ANY* RELEASE, NOT ONLY A WEAPON SWITCH -------------
    // REPORTED, Build V: "the weapon position was incorrect while holding the
    // shotgun after the little sister cutscene ended ... it got fixed by cycling
    // the weapons." A scripted scene releases the cluster; on the frame it ends
    // we re-froze immediately, capturing whatever pose the rig happened to hold
    // coming out of the scene. Cycling weapons changed the pose key, which DID
    // take a settle window, which is why cycling fixed it.
    //
    // The settle window was never really about weapon switches. It is about not
    // capturing a pose the engine has not finished writing, and a release is the
    // only moment that can be true. So it belongs here, where every release goes.
    g_wpKeyChanged = GetTickCount();
    g_wpLastMoved = g_wpKeyChanged;
    g_wpHavePrev = false;
    g_wpSettling = true;
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
    ClusterStateReset();
}