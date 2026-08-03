// ============================================================================
//  shim_main.cpp -- the OpenXR entry points, instance/session/frame loop.
// ============================================================================
#include "shim.h"
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

VrApi     g_vr;
ShimState g_st;

// ---------------------------------------------------------------- logging
static char g_moduleDir[MAX_PATH] = {};
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logCs;
static bool g_logCsInit = false;

extern "C" IMAGE_DOS_HEADER __ImageBase;

static void EnsureModuleDir()
{
    if (g_moduleDir[0]) return;
    char path[MAX_PATH] = {};
    GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
    strcpy(g_moduleDir, path);
    char* slash = strrchr(g_moduleDir, '\\');
    if (slash) *slash = 0;
}

void ShimLog(const char* fmt, ...)
{
    if (!g_logCsInit) { InitializeCriticalSection(&g_logCs); g_logCsInit = true; }
    EnterCriticalSection(&g_logCs);
    if (!g_log)
    {
        EnsureModuleDir();

        char logDir[MAX_PATH] = {};

        _snprintf_s(
            logDir,
            MAX_PATH,
            _TRUNCATE,
            "%s\\logs",
            g_moduleDir);

        CreateDirectoryA(
            logDir,
            nullptr);

        char path[MAX_PATH] = {};

        _snprintf_s(
            path,
            MAX_PATH,
            _TRUNCATE,
            "%s\\openxr_shim.log",
            logDir);

        g_log = fopen(
            path,
            "w");
    }

    if (g_log)
    {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(g_log, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a; va_start(a, fmt);
        vfprintf(g_log, fmt, a);
        va_end(a);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------- math
M34 M34_Identity()
{
    M34 r = {};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.f;
    return r;
}

M34 M34_Mul(const M34& a, const M34& b)
{
    M34 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
        {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) r.m[i][j] += a.m[i][3];
        }
    return r;
}

M34 M34_InvRigid(const M34& a)
{
    M34 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[j][i];                       // transpose rotation
    for (int i = 0; i < 3; ++i)
        r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}

void M34_XformPoint(const M34& a, const float in[3], float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = a.m[i][0] * in[0] + a.m[i][1] * in[1] + a.m[i][2] * in[2] + a.m[i][3];
}

M34 M34_FromQuatPos(const float q[4], const float p[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    M34 r;
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);     r.m[0][3] = p[0];
    r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);     r.m[1][3] = p[1];
    r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y); r.m[2][3] = p[2];
    return r;
}

void M34_ToQuatPos(const M34& a, float q[4], float p[3])
{
    const float tr = a.m[0][0] + a.m[1][1] + a.m[2][2];
    if (tr > 0.f)
    {
        float s = sqrtf(tr + 1.f) * 2.f;
        q[3] = 0.25f * s;
        q[0] = (a.m[2][1] - a.m[1][2]) / s;
        q[1] = (a.m[0][2] - a.m[2][0]) / s;
        q[2] = (a.m[1][0] - a.m[0][1]) / s;
    }
    else if (a.m[0][0] > a.m[1][1] && a.m[0][0] > a.m[2][2])
    {
        float s = sqrtf(1.f + a.m[0][0] - a.m[1][1] - a.m[2][2]) * 2.f;
        q[3] = (a.m[2][1] - a.m[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (a.m[0][1] + a.m[1][0]) / s;
        q[2] = (a.m[0][2] + a.m[2][0]) / s;
    }
    else if (a.m[1][1] > a.m[2][2])
    {
        float s = sqrtf(1.f + a.m[1][1] - a.m[0][0] - a.m[2][2]) * 2.f;
        q[3] = (a.m[0][2] - a.m[2][0]) / s;
        q[0] = (a.m[0][1] + a.m[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (a.m[1][2] + a.m[2][1]) / s;
    }
    else
    {
        float s = sqrtf(1.f + a.m[2][2] - a.m[0][0] - a.m[1][1]) * 2.f;
        q[3] = (a.m[1][0] - a.m[0][1]) / s;
        q[0] = (a.m[0][2] + a.m[2][0]) / s;
        q[1] = (a.m[1][2] + a.m[2][1]) / s;
        q[2] = 0.25f * s;
    }
    p[0] = a.m[0][3]; p[1] = a.m[1][3]; p[2] = a.m[2][3];
}

M34 M34_FromVr(const vr::HmdMatrix34_t& v)
{
    M34 r;
    memcpy(r.m, v.m, sizeof(r.m));
    return r;
}

// ---------------------------------------------------------------- events
static XrSessionState g_evQueue[32];
static int g_evHead = 0, g_evTail = 0;

void Events_Push(XrSessionState s)
{
    const int next = (g_evTail + 1) % 32;
    if (next == g_evHead) return;              // full: drop (should never happen)
    g_evQueue[g_evTail] = s;
    g_evTail = next;
}

bool Events_Pop(XrSessionState* out)
{
    if (g_evHead == g_evTail) return false;
    *out = g_evQueue[g_evHead];
    g_evHead = (g_evHead + 1) % 32;
    return true;
}

// ---------------------------------------------------------------- OpenVR init
// NOTE: the openvr_api.dll ENTRY POINTS are __cdecl (VR_CALLTYPE); only the
// FnTable methods are __stdcall. Getting this wrong imbalances the x86 stack
// on the first call -- it crashed the game on startup before this was fixed.
typedef intptr_t (__cdecl *PFN_VR_InitInternal2)(EVRInitError*, EVRApplicationType, const char*);
typedef void     (__cdecl *PFN_VR_ShutdownInternal)();
typedef intptr_t (__cdecl *PFN_VR_GetGenericInterface)(const char*, EVRInitError*);
typedef bool     (__cdecl *PFN_VR_IsInterfaceVersionValid)(const char*);
typedef const char* (__cdecl *PFN_VR_GetVRInitErrorAsEnglishDescription)(EVRInitError);
typedef bool     (__cdecl *PFN_VR_IsHmdPresent)();

static PFN_VR_ShutdownInternal g_vrShutdown = nullptr;

static bool VrConnect()
{
    if (g_vr.ok) return true;

    EnsureModuleDir();
    char dllPath[MAX_PATH];
    _snprintf(dllPath, MAX_PATH, "%s\\openvr_api.dll", g_moduleDir);
    g_vr.dll = LoadLibraryA(dllPath);
    if (!g_vr.dll) g_vr.dll = LoadLibraryA("openvr_api.dll");
    if (!g_vr.dll)
    {
        SLOG("!!! openvr_api.dll not found next to openxr_loader.dll -- reinstall the shim package");
        return false;
    }

    auto init  = (PFN_VR_InitInternal2)GetProcAddress(g_vr.dll, "VR_InitInternal2");
    auto getIf = (PFN_VR_GetGenericInterface)GetProcAddress(g_vr.dll, "VR_GetGenericInterface");
    auto errStr= (PFN_VR_GetVRInitErrorAsEnglishDescription)GetProcAddress(g_vr.dll, "VR_GetVRInitErrorAsEnglishDescription");
    g_vrShutdown = (PFN_VR_ShutdownInternal)GetProcAddress(g_vr.dll, "VR_ShutdownInternal");
    if (!init || !getIf)
    {
        SLOG("!!! openvr_api.dll is missing entry points (wrong dll?)");
        return false;
    }

    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Scene, "");
    if (err != EVRInitError_VRInitError_None)
    {
        SLOG("!!! VR_InitInternal2 failed: %d (%s)", (int)err,
             errStr ? errStr(err) : "?");
        SLOG("!!! Is SteamVR installed and the headset connected?");
        return false;
    }

    char buf[128];
    _snprintf(buf, 128, "FnTable:%s", vr::IVRSystem_Version);
    g_vr.sys = (VR_IVRSystem_FnTable*)getIf(buf, &err);
    _snprintf(buf, 128, "FnTable:%s", vr::IVRCompositor_Version);
    g_vr.comp = (VR_IVRCompositor_FnTable*)getIf(buf, &err);
    _snprintf(buf, 128, "FnTable:%s", vr::IVRInput_Version);
    g_vr.input = (VR_IVRInput_FnTable*)getIf(buf, &err);
    _snprintf(buf, 128, "FnTable:%s", vr::IVROverlay_Version);
    g_vr.ovl = (VR_IVROverlay_FnTable*)getIf(buf, &err);   // optional

    if (!g_vr.sys || !g_vr.comp || !g_vr.input)
    {
        SLOG("!!! VR_GetGenericInterface failed (sys=%p comp=%p input=%p) v(%s/%s/%s)",
             g_vr.sys, g_vr.comp, g_vr.input,
             vr::IVRSystem_Version, vr::IVRCompositor_Version, vr::IVRInput_Version);
        return false;
    }

    g_vr.comp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseStanding);
    g_vr.ok = true;
    SLOG("OpenVR connected. Interfaces: %s / %s / %s",
         vr::IVRSystem_Version, vr::IVRCompositor_Version, vr::IVRInput_Version);
    return true;
}

// ---------------------------------------------------------------- helpers
static const XrInstance  kInstance = (XrInstance)0x5601;
static const XrSession   kSession  = (XrSession)0x5602;
static const XrSystemId  kSystemId = 1;

static std::vector<std::string>* g_paths = nullptr;   // XrPath = index+1

static std::vector<ActionSetRec*> g_actionSets;
static std::vector<ActionRec*>    g_actions;

static XrTime NowXrTime()
{
    if (!g_st.qpf.QuadPart) { QueryPerformanceFrequency(&g_st.qpf); QueryPerformanceCounter(&g_st.qpc0); }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    const double ns = (double)(t.QuadPart - g_st.qpc0.QuadPart) * 1e9 / (double)g_st.qpf.QuadPart;
    return (XrTime)ns + 1;
}

static void CacheHmdGeometry()
{
    g_vr.sys->GetRecommendedRenderTargetSize(&g_st.rtW, &g_st.rtH);
    for (int e = 0; e < 2; ++e)
    {
        HmdMatrix34_t ehC = g_vr.sys->GetEyeToHeadTransform((EVREye)e);
        memcpy(g_st.eyeToHead[e].m, ehC.m, sizeof(g_st.eyeToHead[e].m));

        float l, r, t, b;
        g_vr.sys->GetProjectionRaw((EVREye)e, &l, &r, &t, &b);
        // OpenVR raw projection uses y-down tangents: top is negative.
        // Convert to up-positive: U = -t, D = -b. Guard against the opposite
        // convention just in case.
        float U = -t, D = -b;
        if (U < D) { const float tmp = U; U = D; D = tmp; }
        g_st.rawL[e] = l; g_st.rawR[e] = r; g_st.rawU[e] = U; g_st.rawD[e] = D;

        g_st.eyeFov[e].angleLeft  = atanf(l);
        g_st.eyeFov[e].angleRight = atanf(r);
        g_st.eyeFov[e].angleUp    = atanf(U);
        g_st.eyeFov[e].angleDown  = atanf(D);
        SLOG("eye %d raw tangents L%.3f R%.3f U%.3f D%.3f  eyeToHead x=%.4f",
             e, l, r, U, D, g_st.eyeToHead[e].m[0][3]);
    }
    ETrackedPropertyError perr = ETrackedPropertyError_TrackedProp_Success;
    const float hz = g_vr.sys->GetFloatTrackedDeviceProperty(0,
        ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &perr);
    if (perr == ETrackedPropertyError_TrackedProp_Success && hz > 30.f) g_st.displayHz = hz;
    SLOG("recommended per-eye %ux%u, display %.1f Hz", g_st.rtW, g_st.rtH, g_st.displayHz);
}

static void PumpVrEvents()
{
    if (!g_vr.ok) return;
    vr::VREvent_t ev;
    while (g_vr.sys->PollNextEvent((VREvent_t*)&ev, sizeof(ev)))
    {
        if (ev.eventType == vr::VREvent_Quit)
        {
            SLOG("SteamVR requested quit");
            g_vr.sys->AcknowledgeQuit_Exiting();
            Events_Push(XR_SESSION_STATE_STOPPING);
        }
    }
}

// ============================================================================
//  exported OpenXR functions
// ============================================================================
#define SHIM_EXPORT extern "C" __declspec(dllexport)

// implemented in shim_input.cpp
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet, const XrActionCreateInfo*, XrAction*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession, const XrActionsSyncInfo*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession, const XrActionStateGetInfo*, XrActionStateFloat*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*);
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation*);

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(
    const XrInstanceCreateInfo* createInfo, XrInstance* instance)
{
    SLOG("=== BioshockVR OpenXR->OpenVR shim (build %s %s) ===", __DATE__, __TIME__);
    if (!createInfo || !instance) return XR_ERROR_VALIDATION_FAILURE;
    SLOG("xrCreateInstance app='%s' extensions=%u",
         createInfo->applicationInfo.applicationName, createInfo->enabledExtensionCount);
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i)
        SLOG("  ext: %s", createInfo->enabledExtensionNames[i]);

    if (!VrConnect()) return XR_ERROR_INITIALIZATION_FAILED;

    if (!g_paths) g_paths = new std::vector<std::string>();
    g_st.instanceAlive = true;
    *instance = kInstance;
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance)
{
    SLOG("xrDestroyInstance");
    Render_Shutdown();
    if (g_vr.ok && g_vrShutdown) g_vrShutdown();
    g_vr.ok = false;
    g_st.instanceAlive = false;
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(
    XrInstance, XrInstanceProperties* p)
{
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    strncpy(p->runtimeName, "OpenVR/SteamVR via BioshockVR shim", XR_MAX_RUNTIME_NAME_SIZE - 1);
    p->runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = 0;
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(
    XrInstance, const XrSystemGetInfo* info, XrSystemId* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *out = kSystemId;
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetD3D11GraphicsRequirementsKHR(
    XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* req)
{
    if (!req) return XR_ERROR_VALIDATION_FAILURE;
    uint64_t luid = 0;
    g_vr.sys->GetOutputDevice(&luid, ETextureType_TextureType_DirectX, nullptr);
    memcpy(&req->adapterLuid, &luid, sizeof(LUID));
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    SLOG("xrGetD3D11GraphicsRequirementsKHR luid=%08X:%08X",
         (unsigned)(luid >> 32), (unsigned)(luid & 0xFFFFFFFF));
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(
    XrInstance instance, const char* name, PFN_xrVoidFunction* fn);   // defined at bottom

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(
    XrInstance, XrSystemId, XrViewConfigurationType type,
    uint32_t capacity, uint32_t* count, XrViewConfigurationView* views)
{
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (count) *count = 2;
    if (capacity == 0 || !views) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;

    if (!g_st.rtW) CacheHmdGeometry();
    for (int i = 0; i < 2; ++i)
    {
        views[i].recommendedImageRectWidth  = g_st.rtW;
        views[i].recommendedImageRectHeight = g_st.rtH;
        views[i].maxImageRectWidth  = 8192;
        views[i].maxImageRectHeight = 8192;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(
    XrInstance, const XrSessionCreateInfo* info, XrSession* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;

    const XrGraphicsBindingD3D11KHR* bind = nullptr;
    for (const XrBaseInStructure* s = (const XrBaseInStructure*)info->next; s;
         s = (const XrBaseInStructure*)s->next)
        if (s->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
            bind = (const XrGraphicsBindingD3D11KHR*)s;

    if (!bind || !bind->device)
    {
        SLOG("!!! xrCreateSession: no D3D11 graphics binding");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    g_st.dev = bind->device;
    g_st.dev->GetImmediateContext(&g_st.ctx);
    if (!g_st.rtW) CacheHmdGeometry();

    g_st.sessionAlive = true;
    *out = kSession;

    Events_Push(XR_SESSION_STATE_IDLE);
    Events_Push(XR_SESSION_STATE_READY);
    SLOG("xrCreateSession ok (device=%p)", g_st.dev);
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession)
{
    g_st.sessionAlive = g_st.sessionBegun = false;
    if (g_st.ctx) { g_st.ctx->Release(); g_st.ctx = nullptr; }
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(
    XrSession, const XrSessionBeginInfo*)
{
    g_st.sessionBegun = true;
    Events_Push(XR_SESSION_STATE_SYNCHRONIZED);
    Events_Push(XR_SESSION_STATE_VISIBLE);
    Events_Push(XR_SESSION_STATE_FOCUSED);
    SLOG("xrBeginSession");
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession)
{
    g_st.sessionBegun = false;
    Events_Push(XR_SESSION_STATE_EXITING);
    SLOG("xrEndSession");
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(
    XrInstance, XrEventDataBuffer* ev)
{
    if (!ev) return XR_ERROR_VALIDATION_FAILURE;
    PumpVrEvents();

    XrSessionState s;
    if (!Events_Pop(&s)) return XR_EVENT_UNAVAILABLE;

    XrEventDataSessionStateChanged* ssc = (XrEventDataSessionStateChanged*)ev;
    memset(ev, 0, sizeof(*ev));
    ssc->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    ssc->session = kSession;
    ssc->state = s;
    ssc->time = NowXrTime();
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- spaces
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(
    XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    SpaceRec* s = new SpaceRec();
    if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
        s->kind = SPACE_REF_LOCAL;
    else if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW)
        s->kind = SPACE_REF_VIEW;
    else { delete s; return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED; }
    *out = (XrSpace)s;
    SLOG("xrCreateReferenceSpace kind=%d", s->kind);
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace sp)
{
    delete (SpaceRec*)sp;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- swapchains
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(
    XrSession, uint32_t capacity, uint32_t* count, int64_t* formats)
{
    static const int64_t fmts[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,   // 29 -- the mod's first choice
        DXGI_FORMAT_R8G8B8A8_UNORM,        // 28
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,   // 91
        DXGI_FORMAT_B8G8R8A8_UNORM,        // 87
    };
    if (count) *count = 4;
    if (capacity == 0 || !formats) return XR_SUCCESS;
    if (capacity < 4) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(formats, fmts, sizeof(fmts));
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(
    XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out)
{
    if (!info || !out || !g_st.dev) return XR_ERROR_VALIDATION_FAILURE;

    // Backing texture is TYPELESS so the mod can CopyResource from UNORM game
    // surfaces into it regardless of the *_SRGB view format it asked for. We
    // sample it as UNORM (raw gamma values) and Submit with ColorSpace_Gamma,
    // so the numbers pass through untouched end to end.
    DXGI_FORMAT typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    DXGI_FORMAT srvFmt   = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (info->format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS; srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS; srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default:
        SLOG("!!! xrCreateSwapchain: unsupported format %d", (int)info->format);
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = info->width;
    td.Height = info->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = typeless;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    SwapchainRec* sc = new SwapchainRec();
    sc->w = info->width; sc->h = info->height; sc->fmt = info->format;

    HRESULT hr = g_st.dev->CreateTexture2D(&td, nullptr, &sc->tex);
    if (FAILED(hr))
    {
        SLOG("!!! xrCreateSwapchain CreateTexture2D failed 0x%08X (%ux%u)",
             (unsigned)hr, info->width, info->height);
        delete sc;
        return XR_ERROR_RUNTIME_FAILURE;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srvFmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = g_st.dev->CreateShaderResourceView(sc->tex, &sd, &sc->srv);
    if (FAILED(hr))
    {
        SLOG("!!! xrCreateSwapchain CreateSRV failed 0x%08X", (unsigned)hr);
        sc->tex->Release();
        delete sc;
        return XR_ERROR_RUNTIME_FAILURE;
    }

    *out = (XrSwapchain)sc;
    SLOG("xrCreateSwapchain %ux%u fmt %d -> ok", info->width, info->height, (int)info->format);
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain h)
{
    SwapchainRec* sc = (SwapchainRec*)h;
    if (sc)
    {
        if (sc->srv) sc->srv->Release();
        if (sc->tex) sc->tex->Release();
        delete sc;
    }
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(
    XrSwapchain h, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
    SwapchainRec* sc = (SwapchainRec*)h;
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (count) *count = 1;
    if (capacity == 0 || !images) return XR_SUCCESS;
    XrSwapchainImageD3D11KHR* d = (XrSwapchainImageD3D11KHR*)images;
    d[0].texture = sc->tex;
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(
    XrSwapchain h, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{
    if (index) *index = 0;
    SwapchainRec* sc = (SwapchainRec*)h;
    if (sc) sc->acquired = true;
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(
    XrSwapchain, const XrSwapchainImageWaitInfo*)
{
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(
    XrSwapchain h, const XrSwapchainImageReleaseInfo*)
{
    SwapchainRec* sc = (SwapchainRec*)h;
    if (sc) sc->acquired = false;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- frame loop
static vr::TrackedDevicePose_t g_renderPoses[vr::k_unMaxTrackedDeviceCount];

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(
    XrSession, const XrFrameWaitInfo*, XrFrameState* fs)
{
    if (!fs) return XR_ERROR_VALIDATION_FAILURE;
    if (!g_vr.ok) return XR_ERROR_SESSION_LOST;

    // The blocking pacing point: returns at "running start", ~2-3 ms before
    // vsync, with poses predicted to the upcoming photon time.
    const EVRCompositorError we = g_vr.comp->WaitGetPoses(
        (TrackedDevicePose_t*)g_renderPoses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    {
        static EVRCompositorError lastWe = EVRCompositorError_VRCompositorError_None;
        if (we != lastWe)
        {
            lastWe = we;
            if (we != EVRCompositorError_VRCompositorError_None)
                SLOG("WaitGetPoses -> compositor error %d", (int)we);
        }
    }

    const vr::TrackedDevicePose_t& hp = g_renderPoses[vr::k_unTrackedDeviceIndex_Hmd];
    if (hp.bPoseIsValid)
    {
        M34 abs = M34_FromVr(hp.mDeviceToAbsoluteTracking);
        if (!g_st.haveOrigin)
        {
            // Latch LOCAL-space origin: first valid HMD pose, yaw + position.
            // HMD forward = -z column of the rotation. Origin = Ry(yaw) whose
            // forward (-s, 0, -c) matches the flattened HMD forward.
            float fx = -abs.m[0][2], fz = -abs.m[2][2];
            const float len = sqrtf(fx * fx + fz * fz);
            float yaw = 0.f;
            if (len > 1e-3f) yaw = atan2f(-fx / len, -fz / len);
            M34 o = M34_Identity();
            const float c = cosf(yaw), s = sinf(yaw);
            o.m[0][0] = c;  o.m[0][2] = s;
            o.m[2][0] = -s; o.m[2][2] = c;
            o.m[0][3] = abs.m[0][3];
            o.m[1][3] = abs.m[1][3];
            o.m[2][3] = abs.m[2][3];
            g_st.originInv = M34_InvRigid(o);
            g_st.haveOrigin = true;
            SLOG("LOCAL origin latched at (%.2f %.2f %.2f) yaw %.1f deg",
                 abs.m[0][3], abs.m[1][3], abs.m[2][3], yaw * 57.2958f);
        }
        g_st.hmd = M34_Mul(g_st.originInv, abs);
        g_st.hmdValid = true;
    }
    else
        g_st.hmdValid = false;

    const XrTime period = (XrTime)(1e9 / (double)g_st.displayHz);
    fs->predictedDisplayTime = NowXrTime() + period;
    fs->predictedDisplayPeriod = period;
    fs->shouldRender = XR_TRUE;
    g_st.lastPredictedTime = fs->predictedDisplayTime;
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(
    XrSession, const XrFrameBeginInfo*)
{
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(
    XrSession, const XrViewLocateInfo* info, XrViewState* state,
    uint32_t capacity, uint32_t* count, XrView* views)
{
    if (!info || !state || !count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 2;
    if (capacity == 0 || !views) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;

    if (!g_st.hmdValid || !g_st.haveOrigin)
    {
        state->viewStateFlags = 0;
        for (int e = 0; e < 2; ++e)
        {
            views[e].pose.orientation = { 0, 0, 0, 1 };
            views[e].pose.position = { 0, 0, 0 };
            views[e].fov = g_st.eyeFov[e];
        }
        return XR_SUCCESS;
    }

    for (int e = 0; e < 2; ++e)
    {
        const M34 eyePose = M34_Mul(g_st.hmd, g_st.eyeToHead[e]);
        float q[4], p[3];
        M34_ToQuatPos(eyePose, q, p);
        views[e].pose.orientation = { q[0], q[1], q[2], q[3] };
        views[e].pose.position = { p[0], p[1], p[2] };
        views[e].fov = g_st.eyeFov[e];
    }
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                            XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                            XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(
    XrSession, const XrFrameEndInfo* fei)
{
    if (!fei) return XR_ERROR_VALIDATION_FAILURE;
    if (!g_vr.ok) return XR_ERROR_SESSION_LOST;

    ProjDrawView proj[2];
    bool haveProj = false;
    QuadDraw quads[8];
    int quadCount = 0;

    for (uint32_t i = 0; i < fei->layerCount; ++i)
    {
        const XrCompositionLayerBaseHeader* L = fei->layers[i];
        if (!L) continue;

        if (L->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
        {
            const XrCompositionLayerProjection* pl = (const XrCompositionLayerProjection*)L;
            if (pl->viewCount != 2) continue;
            for (int e = 0; e < 2; ++e)
            {
                const XrCompositionLayerProjectionView& v = pl->views[e];
                SwapchainRec* sc = (SwapchainRec*)v.subImage.swapchain;
                if (!sc) { haveProj = false; break; }
                proj[e].srv = sc->srv;
                proj[e].pose[0] = v.pose.orientation.x;
                proj[e].pose[1] = v.pose.orientation.y;
                proj[e].pose[2] = v.pose.orientation.z;
                proj[e].pose[3] = v.pose.orientation.w;
                proj[e].pose[4] = v.pose.position.x;
                proj[e].pose[5] = v.pose.position.y;
                proj[e].pose[6] = v.pose.position.z;
                proj[e].tanL = tanf(v.fov.angleLeft);
                proj[e].tanR = tanf(v.fov.angleRight);
                proj[e].tanU = tanf(v.fov.angleUp);
                proj[e].tanD = tanf(v.fov.angleDown);
                haveProj = true;
            }
        }
        else if (L->type == XR_TYPE_COMPOSITION_LAYER_QUAD && quadCount < 8)
        {
            const XrCompositionLayerQuad* ql = (const XrCompositionLayerQuad*)L;
            SwapchainRec* sc = (SwapchainRec*)ql->subImage.swapchain;
            if (!sc) continue;
            SpaceRec* sp = (SpaceRec*)ql->space;
            QuadDraw& q = quads[quadCount++];
            q.srv = sc->srv;
            q.pose[0] = ql->pose.orientation.x;
            q.pose[1] = ql->pose.orientation.y;
            q.pose[2] = ql->pose.orientation.z;
            q.pose[3] = ql->pose.orientation.w;
            q.pose[4] = ql->pose.position.x;
            q.pose[5] = ql->pose.position.y;
            q.pose[6] = ql->pose.position.z;
            q.sx = ql->size.width;
            q.sy = ql->size.height;
            q.viewSpace = (sp && sp->kind == SPACE_REF_VIEW) ? 1 : 0;
            // The menu quad ships NO blend flags (opaque); crosshair and HUD
            // ship SOURCE_ALPHA | UNPREMULTIPLIED. Honor them exactly.
            if (ql->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)
                q.blend = (ql->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 1 : 2;
            else
                q.blend = 0;
        }
    }

    Render_CompositeAndSubmit(haveProj ? proj : nullptr, quads, quadCount);
    ++g_st.frameIndex;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- paths
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(
    XrInstance, const char* str, XrPath* out)
{
    if (!str || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (!g_paths) g_paths = new std::vector<std::string>();
    for (size_t i = 0; i < g_paths->size(); ++i)
        if ((*g_paths)[i] == str) { *out = (XrPath)(i + 1); return XR_SUCCESS; }
    g_paths->push_back(str);
    *out = (XrPath)g_paths->size();
    return XR_SUCCESS;
}

const char* PathToString(XrPath p)
{
    if (!g_paths || p == XR_NULL_PATH || p > g_paths->size()) return "";
    return (*g_paths)[(size_t)p - 1].c_str();
}

// ---------------------------------------------------------------- GIPA
struct ProcEntry { const char* name; PFN_xrVoidFunction fn; };

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(
    XrInstance, const char* name, PFN_xrVoidFunction* fn)
{
    static const ProcEntry table[] = {
        { "xrCreateInstance",            (PFN_xrVoidFunction)xrCreateInstance },
        { "xrDestroyInstance",           (PFN_xrVoidFunction)xrDestroyInstance },
        { "xrGetInstanceProperties",     (PFN_xrVoidFunction)xrGetInstanceProperties },
        { "xrGetSystem",                 (PFN_xrVoidFunction)xrGetSystem },
        { "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction)xrGetD3D11GraphicsRequirementsKHR },
        { "xrEnumerateViewConfigurationViews", (PFN_xrVoidFunction)xrEnumerateViewConfigurationViews },
        { "xrCreateSession",             (PFN_xrVoidFunction)xrCreateSession },
        { "xrDestroySession",            (PFN_xrVoidFunction)xrDestroySession },
        { "xrBeginSession",              (PFN_xrVoidFunction)xrBeginSession },
        { "xrEndSession",                (PFN_xrVoidFunction)xrEndSession },
        { "xrPollEvent",                 (PFN_xrVoidFunction)xrPollEvent },
        { "xrCreateReferenceSpace",      (PFN_xrVoidFunction)xrCreateReferenceSpace },
        { "xrDestroySpace",              (PFN_xrVoidFunction)xrDestroySpace },
        { "xrEnumerateSwapchainFormats", (PFN_xrVoidFunction)xrEnumerateSwapchainFormats },
        { "xrCreateSwapchain",           (PFN_xrVoidFunction)xrCreateSwapchain },
        { "xrDestroySwapchain",          (PFN_xrVoidFunction)xrDestroySwapchain },
        { "xrEnumerateSwapchainImages",  (PFN_xrVoidFunction)xrEnumerateSwapchainImages },
        { "xrAcquireSwapchainImage",     (PFN_xrVoidFunction)xrAcquireSwapchainImage },
        { "xrWaitSwapchainImage",        (PFN_xrVoidFunction)xrWaitSwapchainImage },
        { "xrReleaseSwapchainImage",     (PFN_xrVoidFunction)xrReleaseSwapchainImage },
        { "xrWaitFrame",                 (PFN_xrVoidFunction)xrWaitFrame },
        { "xrBeginFrame",                (PFN_xrVoidFunction)xrBeginFrame },
        { "xrLocateViews",               (PFN_xrVoidFunction)xrLocateViews },
        { "xrEndFrame",                  (PFN_xrVoidFunction)xrEndFrame },
        { "xrStringToPath",              (PFN_xrVoidFunction)xrStringToPath },
        { "xrCreateActionSet",           (PFN_xrVoidFunction)xrCreateActionSet },
        { "xrCreateAction",              (PFN_xrVoidFunction)xrCreateAction },
        { "xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)xrSuggestInteractionProfileBindings },
        { "xrAttachSessionActionSets",   (PFN_xrVoidFunction)xrAttachSessionActionSets },
        { "xrCreateActionSpace",         (PFN_xrVoidFunction)xrCreateActionSpace },
        { "xrSyncActions",               (PFN_xrVoidFunction)xrSyncActions },
        { "xrGetActionStateBoolean",     (PFN_xrVoidFunction)xrGetActionStateBoolean },
        { "xrGetActionStateFloat",       (PFN_xrVoidFunction)xrGetActionStateFloat },
        { "xrGetActionStateVector2f",    (PFN_xrVoidFunction)xrGetActionStateVector2f },
        { "xrLocateSpace",               (PFN_xrVoidFunction)xrLocateSpace },
    };
    if (!name || !fn) return XR_ERROR_VALIDATION_FAILURE;
    for (const ProcEntry& e : table)
        if (strcmp(e.name, name) == 0) { *fn = e.fn; return XR_SUCCESS; }
    *fn = nullptr;
    SLOG("xrGetInstanceProcAddr: '%s' not implemented", name);
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
