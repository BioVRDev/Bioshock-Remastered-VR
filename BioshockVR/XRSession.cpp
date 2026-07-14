#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include "XRSession.h"

#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#pragma comment(lib, "d3d11.lib")

extern void LogFile(const char* msg);

static void Log(const char* fmt, ...)
{
    char b[512];
    va_list a; va_start(a, fmt);
    vsprintf_s(b, fmt, a);
    va_end(a);
    LogFile(b);
}

static XrInstance  g_inst = XR_NULL_HANDLE;
static XrSystemId  g_sysId = XR_NULL_SYSTEM_ID;
static XrSession   g_session = XR_NULL_HANDLE;
static XrSpace     g_space = XR_NULL_HANDLE;

static XrSwapchain g_sc[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static std::vector<XrSwapchainImageD3D11KHR> g_scImages[2];

static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running = false;   // xrBeginSession has been called
static bool g_init = false;

static ID3D11DeviceContext* g_ctx = nullptr;
static unsigned g_w = 0, g_h = 0;

static XrViewConfigurationView g_viewCfg[2] = {};

// Frame accounting, so the log tells us whether frames are actually reaching
// the compositor -- not just whether Present is firing.
static unsigned long long g_xrFrames = 0;
static unsigned long long g_xrSubmitted = 0;

// The FOV the game actually rendered at. Built by XR_SetGameFov.
static XrFovf g_gameFov = {};
static bool   g_haveGameFov = false;
static bool   g_loggedHmdFov = false;

#define XRCHK(expr, what) \
    do { XrResult _r = (expr); if (XR_FAILED(_r)) { Log(">>> XR: %s failed (%d)", what, (int)_r); return false; } } while (0)

void XR_SetGameFov(float horizFovDeg, unsigned w, unsigned h)
{
    const double PI = 3.14159265358979323846;

    // Horizontal half-angle, then derive vertical from the real backbuffer aspect.
    double halfH = (horizFovDeg * 0.5) * PI / 180.0;
    double halfV = atan(tan(halfH) * ((double)h / (double)w));

    // Symmetric frustum -- the game renders a normal centred perspective view.
    // The HEADSET's frustum is asymmetric and canted; that's exactly the lie
    // we're removing. Both eyes get the same symmetric FOV, so the same image
    // lands in the same angular place in both eyes and can actually FUSE.
    g_gameFov.angleLeft = (float)(-halfH);
    g_gameFov.angleRight = (float)(halfH);
    g_gameFov.angleUp = (float)(halfV);
    g_gameFov.angleDown = (float)(-halfV);
    g_haveGameFov = true;

    Log(">>> XR: GAME FOV = %.1f h / %.1f v deg  (from %ux%u)",
        horizFovDeg, halfV * 2.0 * 180.0 / PI, w, h);
}

bool XR_IsInit() { return g_init; }

static const char* FmtName(int64_t f)
{
    switch ((int)f)
    {
    case 28: return "R8G8B8A8_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 87: return "B8G8R8A8_UNORM";
    case 91: return "B8G8R8A8_UNORM_SRGB";
    case 24: return "R10G10B10A2_UNORM";
    case 10: return "R16G16B16A16_FLOAT";
    default: return "?";
    }
}

bool XR_Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, unsigned w, unsigned h)
{
    if (g_init) return true;
    if (!dev || !ctx) return false;

    g_ctx = ctx;
    g_w = w; g_h = h;

    // --- instance ---
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    strcpy_s(ici.applicationInfo.applicationName, "BioshockVR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "BioshockVR");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrResult r = xrCreateInstance(&ici, &g_inst);
    if (XR_FAILED(r))
    {
        Log(">>> XR: xrCreateInstance FAILED (%d) - is Virtual Desktop Streamer running", (int)r);
        Log(">>> XR: and the Quest connected BEFORE the game launched?");
        return false;
    }

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(g_inst, &ip)))
        Log(">>> XR: runtime = %s", ip.runtimeName);

    // --- system ---
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHK(xrGetSystem(g_inst, &sgi, &g_sysId), "xrGetSystem");

    // --- graphics requirements (MUST call before xrCreateSession) ---
    PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetReq = nullptr;
    XRCHK(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfnGetReq), "xrGetInstanceProcAddr");

    XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    XRCHK(pfnGetReq(g_inst, g_sysId, &req), "xrGetD3D11GraphicsRequirements");
    Log(">>> XR: runtime wants adapter LUID %08X:%08X",
        (unsigned)req.adapterLuid.HighPart, (unsigned)req.adapterLuid.LowPart);

    // --- view config ---
    uint32_t viewCount = 0;
    XRCHK(xrEnumerateViewConfigurationViews(g_inst, g_sysId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
        "xrEnumerateViewConfigurationViews(count)");
    if (viewCount != 2) { Log(">>> XR: expected 2 views, got %u", viewCount); return false; }

    g_viewCfg[0].type = g_viewCfg[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    XRCHK(xrEnumerateViewConfigurationViews(g_inst, g_sysId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, g_viewCfg),
        "xrEnumerateViewConfigurationViews");
    Log(">>> XR: recommended per-eye %ux%u",
        g_viewCfg[0].recommendedImageRectWidth, g_viewCfg[0].recommendedImageRectHeight);

    // --- session ---
    XrGraphicsBindingD3D11KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    bind.device = dev;

    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &bind;
    sci.systemId = g_sysId;
    XRCHK(xrCreateSession(g_inst, &sci, &g_session), "xrCreateSession");
    Log(">>> XR: session created");

    // --- reference space ---
    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XRCHK(xrCreateReferenceSpace(g_session, &rsci, &g_space), "xrCreateReferenceSpace");

    // --- swapchain format ---
    // The GAME'S BACKBUFFER IS R8G8B8A8_UNORM (DXGI 28), measured in Phase 2.
    // CopyResource needs src and dst in the same typeless family, so we must
    // pick an R8G8B8A8 format -- NOT B8G8R8A8. Both 28 and 29 are legal
    // destinations (same family, raw bit copy).
    //
    // We prefer _SRGB (29): the game writes gamma-encoded pixels, and declaring
    // the swapchain sRGB tells the compositor to linearize them on sample.
    // If the headset image looks WASHED OUT / MILKY, swap the two blocks below.
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(g_session, fmtCount, &fmtCount, fmts.data());

    Log(">>> XR: runtime offers %u swapchain formats:", fmtCount);
    for (int64_t f : fmts)
        Log("       DXGI %d  %s", (int)f, FmtName(f));

    int64_t chosen = 0;
    for (int64_t f : fmts)                                   // 1st choice
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { chosen = f; break; }
    if (!chosen)
        for (int64_t f : fmts)                               // 2nd choice
            if (f == DXGI_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
    if (!chosen)
    {
        Log(">>> XR: !!! runtime offers NO R8G8B8A8 format. CopyResource from the");
        Log(">>> XR: !!! game's DXGI 28 backbuffer would be illegal. Stopping.");
        return false;
    }
    Log(">>> XR: CHOSE swapchain format DXGI %d  %s", (int)chosen, FmtName(chosen));

    // --- swapchains, one per eye, at the GAME's resolution (runtime will scale) ---
    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo sc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = chosen;
        sc.sampleCount = 1;
        sc.width = g_w;
        sc.height = g_h;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        XRCHK(xrCreateSwapchain(g_session, &sc, &g_sc[eye]), "xrCreateSwapchain");

        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(g_sc[eye], 0, &imgCount, nullptr);
        g_scImages[eye].resize(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        XRCHK(xrEnumerateSwapchainImages(g_sc[eye], imgCount, &imgCount,
            (XrSwapchainImageBaseHeader*)g_scImages[eye].data()),
            "xrEnumerateSwapchainImages");
        Log(">>> XR: eye %d swapchain %ux%u  %u images", eye, g_w, g_h, imgCount);
    }

    g_init = true;
    Log(">>> XR: INIT COMPLETE");
    return true;
}

static void PumpEvents()
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS)
    {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            auto* ssc = (XrEventDataSessionStateChanged*)&ev;
            g_state = ssc->state;
            Log(">>> XR: session state -> %d", (int)g_state);

            if (g_state == XR_SESSION_STATE_READY && !g_running)
            {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &bi)))
                {
                    g_running = true;
                    Log(">>> XR: session BEGUN - frames flowing");
                }
            }
            else if (g_state == XR_SESSION_STATE_STOPPING && g_running)
            {
                xrEndSession(g_session);
                g_running = false;
                Log(">>> XR: session ended");
            }
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

void XR_Frame(ID3D11Texture2D* image)
{
    if (!g_init) return;

    PumpEvents();
    if (!g_running || !image) return;

    ++g_xrFrames;

    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(g_session, &fwi, &fs))) return;

    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    if (XR_FAILED(xrBeginFrame(g_session, &fbi))) return;

    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[2] = {};
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
    uint32_t layerCount = 0;

    if (fs.shouldRender)
    {
        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_space;

        XrViewState vs = { XR_TYPE_VIEW_STATE };
        uint32_t got = 0;

        if (XR_SUCCEEDED(xrLocateViews(g_session, &vli, &vs, 2, &got, views)) && got == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        {
            for (int eye = 0; eye < 2; ++eye)
            {
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                if (XR_FAILED(xrAcquireSwapchainImage(g_sc[eye], &ai, &idx))) continue;

                XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_FAILED(xrWaitSwapchainImage(g_sc[eye], &wi))) continue;

                // Same image to both eyes. Mono, on purpose. AER is Phase 6.
                g_ctx->CopyResource(g_scImages[eye][idx].texture, image);

                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(g_sc[eye], &ri);

                pv[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                pv[eye].pose = views[eye].pose;
                
                // THE PHASE 8 FIX. Report the GAME's fov, not the headset's.
                pv[eye].fov = g_haveGameFov ? g_gameFov : views[eye].fov;

                if (!g_loggedHmdFov && eye == 1)
                {
                    g_loggedHmdFov = true;
                    const double R2D = 180.0 / 3.14159265358979323846;
                    for (int e = 0; e < 2; ++e)
                        Log(">>> XR: HMD eye %d fov  L%.1f R%.1f U%.1f D%.1f deg",
                            e, views[e].fov.angleLeft * R2D, views[e].fov.angleRight * R2D,
                            views[e].fov.angleUp * R2D, views[e].fov.angleDown * R2D);
                }

                pv[eye].subImage.swapchain = g_sc[eye];
                pv[eye].subImage.imageRect.offset = { 0, 0 };
                pv[eye].subImage.imageRect.extent = { (int32_t)g_w, (int32_t)g_h };
                pv[eye].subImage.imageArrayIndex = 0;
            }

            layer.space = g_space;
            layer.viewCount = 2;
            layer.views = pv;
            layers[0] = (const XrCompositionLayerBaseHeader*)&layer;
            layerCount = 1;
            ++g_xrSubmitted;
        }
    }

    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layers;
    xrEndFrame(g_session, &fei);
}

void XR_Stats(unsigned long long* frames, unsigned long long* submitted, int* state)
{
    if (frames)    *frames = g_xrFrames;
    if (submitted) *submitted = g_xrSubmitted;
    if (state)     *state = (int)g_state;
}

void XR_Shutdown()
{
    for (int eye = 0; eye < 2; ++eye)
        if (g_sc[eye]) { xrDestroySwapchain(g_sc[eye]); g_sc[eye] = XR_NULL_HANDLE; }
    if (g_space) { xrDestroySpace(g_space);     g_space = XR_NULL_HANDLE; }
    if (g_session) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
    if (g_inst) { xrDestroyInstance(g_inst);   g_inst = XR_NULL_HANDLE; }
    g_init = g_running = false;
}