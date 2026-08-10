// ============================================================================
//  shim.h -- BioshockVR OpenXR-over-OpenVR shim, shared internals.
//
//  This DLL replaces openxr_loader.dll next to BioshockHD.exe. It implements
//  the exact OpenXR API surface BioshockVR.dll imports, backed by OpenVR
//  (SteamVR), which -- unlike SteamVR's OpenXR runtime -- fully supports
//  32-bit applications. That makes the mod work on native SteamVR headsets
//  (Vive, Vive Pro, Index) with no changes to the mod itself.
//
//  Design notes:
//   * OpenXR and OpenVR share coordinate conventions (right-handed, +y up,
//     -z forward), so poses carry over with no axis surgery.
//   * OpenXR's LOCAL space is emulated by latching the first valid HMD pose
//     (yaw + position) as the origin of everything we report.
//   * OpenXR projection layers carry an arbitrary per-view FOV + pose;
//     OpenVR's Submit() does not. So xrEndFrame re-composites: each layer is
//     drawn as a textured quad into a per-eye render target using the real
//     HMD frustum (GetProjectionRaw), then the two eye targets are Submitted.
//     A projection layer becomes a quad at 50 m -- at that distance a planar
//     homography is indistinguishable from ideal rotational reprojection, and
//     the mod's latched-pose flicker fix falls out of the geometry naturally.
//   * Input: the mod's action set is mirrored into a SteamVR input manifest
//     generated at attach time, with authored bindings for Index (knuckles),
//     Vive wands and Touch. Users can rebind in SteamVR's controller UI.
// ============================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// openvr.h: C++ header, namespace vr, carries the CORRECT pack pragmas for
// every struct (the C API header openvr_capi.h does not on Windows). We use
// vr:: structs for all data crossing the ABI and the FnTable structs from
// openvr_capi.h for the calls themselves (C calling convention, MinGW-safe).
#include <openvr.h>

// openvr_capi.h does `typedef char bool` when __WIN32 is defined, which is
// illegal C++. The macro is used nowhere else in the header; drop it so the
// header takes its <stdbool.h> branch (a no-op in C++, same 1-byte bool).
#ifdef __WIN32
#undef __WIN32
#endif
extern "C" {
#include <openvr_capi.h>
}

// ---------------------------------------------------------------- logging
void ShimLog(const char* fmt, ...);
#define SLOG(...) ShimLog(__VA_ARGS__)

// ---------------------------------------------------------------- math
struct M34 { float m[3][4]; };            // row-major 3x4 rigid transform

M34  M34_Identity();
M34  M34_Mul(const M34& a, const M34& b);
M34  M34_InvRigid(const M34& a);
void M34_XformPoint(const M34& a, const float in[3], float out[3]);
M34  M34_FromQuatPos(const float q[4], const float p[3]);   // q = x,y,z,w
void M34_ToQuatPos(const M34& a, float q[4], float p[3]);
M34  M34_FromVr(const vr::HmdMatrix34_t& v);

// ---------------------------------------------------------------- handles
struct SwapchainRec
{
    uint32_t w = 0, h = 0;
    int64_t  fmt = 0;
    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    bool acquired = false;
};

struct ActionSetRec
{
    char     name[80] = {};
    bool     attached = false;
    uint64_t vrHandle = 0;      // VRActionSetHandle_t
};

struct ActionRec
{
    ActionSetRec* set = nullptr;
    char          name[80] = {};
    XrActionType  type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    uint64_t      vrHandle = 0; // VRActionHandle_t
};

enum SpaceKind { SPACE_REF_LOCAL = 0, SPACE_REF_VIEW = 1, SPACE_ACTION = 2 };

struct SpaceRec
{
    int        kind = SPACE_REF_LOCAL;
    ActionRec* action = nullptr;
};

// ---------------------------------------------------------------- OpenVR
struct VrApi
{
    HMODULE dll = nullptr;
    VR_IVRSystem_FnTable*     sys = nullptr;
    VR_IVRCompositor_FnTable* comp = nullptr;
    VR_IVROverlay_FnTable*    ovl = nullptr;   // optional (dashboard check)
    VR_IVRInput_FnTable*      input = nullptr;
    bool ok = false;
};

extern VrApi g_vr;

// ---------------------------------------------------------------- state
struct ShimState
{
    // instance / session lifecycle
    bool instanceAlive = false;
    bool sessionAlive  = false;
    bool sessionBegun  = false;

    // graphics
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    // HMD geometry (cached at session create)
    uint32_t rtW = 0, rtH = 0;                 // recommended per-eye size
    M34      eyeToHead[2];
    float    rawL[2], rawR[2], rawU[2], rawD[2];  // frustum tangents, up-positive
    XrFovf   eyeFov[2];
    float    displayHz = 90.0f;

    // tracking, all in "origin space" (first valid HMD pose = identity)
    bool haveOrigin = false;
    M34  originInv;
    bool hmdValid = false;
    M34  hmd;                                  // origin-space HMD pose (render pose)

    // frame timing
    LARGE_INTEGER qpf = {}, qpc0 = {};
    XrTime lastPredictedTime = 0;
    uint64_t frameIndex = 0;
};

extern ShimState g_st;

// ---------------------------------------------------------------- events
void Events_Push(XrSessionState s);
bool Events_Pop(XrSessionState* out);

// ---------------------------------------------------------------- renderer
struct ProjDrawView
{
    ID3D11ShaderResourceView* srv;
    float pose[7];             // qx,qy,qz,qw, px,py,pz  (origin space)
    float tanL, tanR, tanU, tanD;
};

struct QuadDraw
{
    ID3D11ShaderResourceView* srv;
    float pose[7];             // in its space (VIEW-relative or origin space)
    float sx, sy;              // metres
    int   viewSpace;           // 1 = head-locked (VIEW), 0 = world (LOCAL)
    int   blend;               // 0 opaque, 1 src-alpha (unpremult), 2 premult
};

bool Render_Init();            // lazy, needs g_st.dev
void Render_Shutdown();
// Renders + submits both eyes. proj may be null (menu path submits quads only).
void Render_CompositeAndSubmit(const ProjDrawView* proj /*2 or null*/,
                               const QuadDraw* quads, int quadCount);

// ---------------------------------------------------------------- input
bool InputShim_Attach(ActionSetRec* set, ActionRec** actions, int actionCount);
