#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include "XRSession.h"

#include <windows.h>
#include <intrin.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#pragma comment(lib, "d3d11.lib")

extern void LogFile(const char* msg);

extern float g_cfgEyeSep;    // half-IPD, game units == cm (dllmain.cpp)
extern bool  g_cfgSwapEyes;
bool CameraHook_GetLatchedPose(float quat[4], float pos[3]);   // CameraHook.cpp

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
static XrSpace     g_viewSpace = XR_NULL_HANDLE;   // head-locked, for the menu quad

static XrSwapchain g_sc[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static std::vector<XrSwapchainImageD3D11KHR> g_scImages[2];

static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running = false;
static bool g_init = false;

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static unsigned g_w = 0, g_h = 0;

// The left eye's image, stashed on the eye-0 Present and held until the eye-1
// Present arrives. The ONLY staging texture in the mod.
static ID3D11Texture2D* g_stageL = nullptr;
static bool             g_stageLValid = false;

static XrViewConfigurationView g_viewCfg[2] = {};

static unsigned long long g_xrFrames = 0;
static unsigned long long g_xrSubmitted = 0;

static XrFovf g_gameFov = {};
static bool   g_haveGameFov = false;
static bool   g_loggedHmdFov = false;

// --- LIVE FOV TUNING RIG (temporary, numpad) -------------------------------
// mode 0 = report the GAME's symmetric FOV (current behavior)
// mode 1 = report the RUNTIME's true canted per-eye FOV (the experiment)
// Scales warp the reported frustum via tangent space (correct at wide FOV).
static int   g_fovMode = 0;
static float g_fovScaleH = 1.0f;
static float g_fovScaleV = 1.0f;

static XrFovf ScaleFov(const XrFovf& f, float sh, float sv)
{
    XrFovf o;
    o.angleLeft = atanf(tanf(f.angleLeft) * sh);
    o.angleRight = atanf(tanf(f.angleRight) * sh);
    o.angleUp = atanf(tanf(f.angleUp) * sv);
    o.angleDown = atanf(tanf(f.angleDown) * sv);
    return o;
}

static bool KeyFired(int vk, bool& prev)
{
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = down && !prev;
    prev = down;
    return fired;
}

static void PollFovKeys()
{
    static bool k0 = false, k4 = false, k6 = false, k2 = false, k8 = false, k5 = false;
    bool chg = false;
    if (KeyFired(VK_NUMPAD0, k0)) { g_fovMode = (g_fovMode + 1) & 1;              chg = true; }
    if (KeyFired(VK_NUMPAD4, k4)) { g_fovScaleH -= 0.02f;                          chg = true; }
    if (KeyFired(VK_NUMPAD6, k6)) { g_fovScaleH += 0.02f;                          chg = true; }
    if (KeyFired(VK_NUMPAD2, k2)) { g_fovScaleV -= 0.02f;                          chg = true; }
    if (KeyFired(VK_NUMPAD8, k8)) { g_fovScaleV += 0.02f;                          chg = true; }
    if (KeyFired(VK_NUMPAD5, k5)) { g_fovScaleH = 1.0f; g_fovScaleV = 1.0f;        chg = true; }

    if (g_fovScaleH < 0.5f) g_fovScaleH = 0.5f;  if (g_fovScaleH > 2.0f) g_fovScaleH = 2.0f;
    if (g_fovScaleV < 0.5f) g_fovScaleV = 0.5f;  if (g_fovScaleV > 2.0f) g_fovScaleV = 2.0f;

    if (chg)
        Log(">>> FOVTUNE: mode=%d (%s)  scaleH=%.2f  scaleV=%.2f",
            g_fovMode, g_fovMode == 0 ? "GAME symmetric" : "RUNTIME true per-eye",
            g_fovScaleH, g_fovScaleV);
}
// ---------------------------------------------------------------------------

// Head orientation published render-thread -> game-thread for the camera write
// (Phase 11). Seqlock: odd seq == mid-write. The LAYER pose is no longer fresh
// per-submit -- it comes back LATCHED via CameraHook_GetLatchedPose (§2), so
// the compositor reprojects from the pose the image was actually rendered from.
static volatile long g_headSeq = 0;
static float         g_headQ[4] = { 0.f, 0.f, 0.f, 1.f };   // x,y,z,w, OpenXR LOCAL space
static float         g_headPos[3] = { 0.f, 0.f, 0.f };      // head CENTER (eye midpoint), metres, XR LOCAL

// --- timing ---
static LARGE_INTEGER   g_qpf = {};
static XrTimeBreakdown g_tb = {};

static inline void QPC(LARGE_INTEGER& t) { QueryPerformanceCounter(&t); }
static inline double MS(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
{
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

#define XRCHK(expr, what) \
    do { XrResult _r = (expr); if (XR_FAILED(_r)) { Log(">>> XR: %s failed (%d)", what, (int)_r); return false; } } while (0)

void XR_SetGameFov(float horizFovDeg, unsigned w, unsigned h)
{
    const double PI = 3.14159265358979323846;

    double halfH = (horizFovDeg * 0.5) * PI / 180.0;
    double halfV = atan(tan(halfH) * ((double)h / (double)w));

    // Symmetric frustum. The game renders one centred perspective view; the
    // HEADSET's frustum is canted and asymmetric, and that lie is what we remove.
    // Eye SEPARATION is a position offset on the game camera (§6e), never an FOV
    // change -- do not "fix" this by feeding the HMD's fov back in.
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

    g_dev = dev;
    g_ctx = ctx;
    g_w = w; g_h = h;

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

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHK(xrGetSystem(g_inst, &sgi, &g_sysId), "xrGetSystem");

    PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetReq = nullptr;
    XRCHK(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfnGetReq), "xrGetInstanceProcAddr");

    XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    XRCHK(pfnGetReq(g_inst, g_sysId, &req), "xrGetD3D11GraphicsRequirements");
    Log(">>> XR: runtime wants adapter LUID %08X:%08X",
        (unsigned)req.adapterLuid.HighPart, (unsigned)req.adapterLuid.LowPart);

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

    XrGraphicsBindingD3D11KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    bind.device = dev;

    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &bind;
    sci.systemId = g_sysId;
    XRCHK(xrCreateSession(g_inst, &sci, &g_session), "xrCreateSession");
    Log(">>> XR: session created");

    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XRCHK(xrCreateReferenceSpace(g_session, &rsci, &g_space), "xrCreateReferenceSpace");

    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRCHK(xrCreateReferenceSpace(g_session, &rsci, &g_viewSpace), "xrCreateReferenceSpace(VIEW)");

    // The game's backbuffer is R8G8B8A8_UNORM (DXGI 28). CopyResource needs the
    // same typeless family, so we MUST pick an R8G8B8A8 format -- never BGRA.
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(g_session, fmtCount, &fmtCount, fmts.data());

    Log(">>> XR: runtime offers %u swapchain formats:", fmtCount);
    for (int64_t f : fmts)
        Log("       DXGI %d  %s", (int)f, FmtName(f));

    int64_t chosen = 0;
    for (int64_t f : fmts)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { chosen = f; break; }
    if (!chosen)
        for (int64_t f : fmts)
            if (f == DXGI_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
    if (!chosen)
    {
        Log(">>> XR: !!! runtime offers NO R8G8B8A8 format. Stopping.");
        return false;
    }
    Log(">>> XR: CHOSE swapchain format DXGI %d  %s", (int)chosen, FmtName(chosen));

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

static bool EnsureStageL(ID3D11Texture2D* like)
{
    if (g_stageL) return true;
    if (!g_dev || !like) return false;

    D3D11_TEXTURE2D_DESC d = {};
    like->GetDesc(&d);
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;

    HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &g_stageL);
    if (FAILED(hr) || !g_stageL)
    {
        g_stageL = nullptr;
        Log(">>> XR: !!! stageL CreateTexture2D FAILED hr=0x%08X", (unsigned)hr);
        return false;
    }
    Log(">>> XR: stageL %ux%u DXGI %d created", d.Width, d.Height, (int)d.Format);
    return true;
}

// ONE full XR frame cycle. Every OpenXR call individually timed.
static void SubmitPair(ID3D11Texture2D* leftImg, ID3D11Texture2D* rightImg)
{
    PollFovKeys();
    ++g_xrFrames;
    LARGE_INTEGER a, b;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);

    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState    fs = { XR_TYPE_FRAME_STATE };
    QPC(a); XrResult wr = xrWaitFrame(g_session, &fwi, &fs); QPC(b);
    g_tb.waitFrame += MS(a, b);
    if (XR_FAILED(wr)) return;

    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    QPC(a); XrResult br = xrBeginFrame(g_session, &fbi); QPC(b);
    g_tb.beginFrame += MS(a, b);
    if (XR_FAILED(br)) return;

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

        QPC(a);
        XrResult lr = xrLocateViews(g_session, &vli, &vs, 2, &got, views);
        QPC(b);
        g_tb.locateViews += MS(a, b);

        if (XR_SUCCEEDED(lr) && got == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        {
            // Publish head orientation for the game-thread camera write. view[0]
            // and view[1] share head orientation in our symmetric rig (the cant
            // is in the FOV, not the pose), so eye 0's is the head's.
            _InterlockedIncrement(&g_headSeq);          // odd == writing
            MemoryBarrier();
            g_headQ[0] = views[0].pose.orientation.x;
            g_headQ[1] = views[0].pose.orientation.y;
            g_headQ[2] = views[0].pose.orientation.z;
            g_headQ[3] = views[0].pose.orientation.w;
            // Head CENTER = midpoint of the two eye positions (each view pose is
            // an EYE, offset by half the IPD -- the midpoint cancels that).
            g_headPos[0] = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            g_headPos[1] = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            g_headPos[2] = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            MemoryBarrier();
            _InterlockedIncrement(&g_headSeq);          // even == done

            // --- LAYER POSE (flicker fix, §2): stamp the pose the image was
            // RENDERED from -- the eye-0 latched pose -- not the fresh one.
            // Fresh-vs-latched mismatch = ~3 deg of baked-in yaw at 200 deg/s
            // = the flicker. Falls back to fresh whenever the camera isn't
            // head-driven (the pre-Phase-11 known-good path).
            float lq[4], lp[3];
            const bool useLatched = CameraHook_GetLatchedPose(lq, lp);
            float rxr = 0.f, ryr = 0.f, rzr = 0.f;   // latched RIGHT vec, XR space
            if (useLatched)
            {
                const float qx = lq[0], qy = lq[1], qz = lq[2], qw = lq[3];
                rxr = 1.f - 2.f * (qy * qy + qz * qz);   // quat * (1,0,0)
                ryr = 2.f * (qx * qy + qz * qw);
                rzr = 2.f * (qx * qz - qy * qw);

                static bool logged = false;
                if (!logged) { logged = true; Log(">>> XR: LAYER POSE = LATCHED (flicker fix live)"); }
            }

            ID3D11Texture2D* src[2] = { leftImg, rightImg };

            for (int e = 0; e < 2; ++e)
            {
                if (!src[e]) continue;

                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XrSwapchainImageWaitInfo    wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;

                QPC(a);
                XrResult ar = xrAcquireSwapchainImage(g_sc[e], &ai, &idx);
                XrResult sr = XR_FAILED(ar) ? ar : xrWaitSwapchainImage(g_sc[e], &wi);
                QPC(b);
                g_tb.acquire += MS(a, b);
                if (XR_FAILED(ar) || XR_FAILED(sr)) continue;

                QPC(a);
                g_ctx->CopyResource(g_scImages[e][idx].texture, src[e]);
                QPC(b);
                g_tb.copy += MS(a, b);

                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(g_sc[e], &ri);

                pv[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                pv[e].pose = views[e].pose;              // fallback: fresh pose
                if (useLatched)
                {
                    pv[e].pose.orientation.x = lq[0];
                    pv[e].pose.orientation.y = lq[1];
                    pv[e].pose.orientation.z = lq[2];
                    pv[e].pose.orientation.w = lq[3];

                    // Each eye = latched head center +- half-IPD along the
                    // latched right vector -- where that eye was actually
                    // rendered from. Same sign convention as the camera write.
                    float s = (e == 0 ? -1.f : 1.f) * (g_cfgEyeSep * 0.01f);  // cm -> m
                    if (g_cfgSwapEyes) s = -s;
                    pv[e].pose.position.x = lp[0] + rxr * s;
                    pv[e].pose.position.y = lp[1] + ryr * s;
                    pv[e].pose.position.z = lp[2] + rzr * s;
                }
                {
                    const XrFovf base = (g_fovMode == 1) ? views[e].fov
                        : (g_haveGameFov ? g_gameFov : views[e].fov);
                    pv[e].fov = ScaleFov(base, g_fovScaleH, g_fovScaleV);
                }
                pv[e].subImage.swapchain = g_sc[e];
                pv[e].subImage.imageRect.offset = { 0, 0 };
                pv[e].subImage.imageRect.extent = { (int32_t)g_w, (int32_t)g_h };
                pv[e].subImage.imageArrayIndex = 0;
            }

            if (!g_loggedHmdFov)
            {
                g_loggedHmdFov = true;
                const double R2D = 180.0 / 3.14159265358979323846;
                for (int e = 0; e < 2; ++e)
                    Log(">>> XR: HMD eye %d fov  L%.1f R%.1f U%.1f D%.1f deg",
                        e, views[e].fov.angleLeft * R2D, views[e].fov.angleRight * R2D,
                        views[e].fov.angleUp * R2D, views[e].fov.angleDown * R2D);
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
    QPC(a); xrEndFrame(g_session, &fei); QPC(b);
    g_tb.endFrame += MS(a, b);

    ++g_tb.submits;
}

// eye 0 stashes. eye 1 submits the pair. One XR cycle per two Presents.
void XR_SubmitPair(ID3D11Texture2D* image, int eye)
{
    if (!g_init) return;
    PumpEvents();
    if (!g_running || !image) return;

    if (eye == 0)
    {
        if (!EnsureStageL(image)) return;

        LARGE_INTEGER a, b;
        QPC(a);
        g_ctx->CopyResource(g_stageL, image);
        QPC(b);
        g_tb.copy += MS(a, b);

        g_stageLValid = true;
        return;
    }

    // eye 1. If we somehow never saw a left (first frame, or the camera hook
    // isn't ticking), fall back to mono for this cycle rather than dropping it.
    SubmitPair(g_stageLValid ? g_stageL : image, image);
    g_stageLValid = false;
}

void XR_SubmitMenuMono(ID3D11Texture2D* image)
{
    if (!g_init) return;
    PumpEvents();
    if (!g_running || !image) return;

    g_stageLValid = false;   // any stashed left eye is stale once a menu is up

    ++g_xrFrames;
    LARGE_INTEGER a, b;

    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState    fs = { XR_TYPE_FRAME_STATE };
    QPC(a); XrResult wr = xrWaitFrame(g_session, &fwi, &fs); QPC(b);
    g_tb.waitFrame += MS(a, b);
    if (XR_FAILED(wr)) return;

    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    QPC(a); XrResult br = xrBeginFrame(g_session, &fbi); QPC(b);
    g_tb.beginFrame += MS(a, b);
    if (XR_FAILED(br)) return;

    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
    uint32_t layerCount = 0;

    if (fs.shouldRender)
    {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo    wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = XR_INFINITE_DURATION;

        QPC(a);
        XrResult ar = xrAcquireSwapchainImage(g_sc[0], &ai, &idx);
        XrResult sr = XR_FAILED(ar) ? ar : xrWaitSwapchainImage(g_sc[0], &wi);
        QPC(b);
        g_tb.acquire += MS(a, b);

        if (XR_SUCCEEDED(ar) && XR_SUCCEEDED(sr))
        {
            QPC(a);
            g_ctx->CopyResource(g_scImages[0][idx].texture, image);
            QPC(b);
            g_tb.copy += MS(a, b);

            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(g_sc[0], &ri);

            quad.space = g_viewSpace;                    // head-locked: always visible
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH; // MONO -- depth can't break
            quad.subImage.swapchain = g_sc[0];
            quad.subImage.imageRect.offset = { 0, 0 };
            quad.subImage.imageRect.extent = { (int32_t)g_w, (int32_t)g_h };
            quad.subImage.imageArrayIndex = 0;
            quad.pose.orientation = { 0.f, 0.f, 0.f, 1.f };
            quad.pose.position = { 0.f, 0.f, -1.75f };   // metres in front of the eyes
            quad.size = { 1.5f, 1.5f };                  // metres: ~46 deg wide. Comfortable.
            layers[0] = (const XrCompositionLayerBaseHeader*)&quad;
            layerCount = 1;
            ++g_xrSubmitted;
        }
    }

    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layers;
    QPC(a); xrEndFrame(g_session, &fei); QPC(b);
    g_tb.endFrame += MS(a, b);
    ++g_tb.submits;
}

void XR_GetHeadQuat(float out[4])
{
    for (;;)
    {
        const long s0 = g_headSeq;
        if (s0 & 1) continue;               // writer mid-update
        MemoryBarrier();
        out[0] = g_headQ[0]; out[1] = g_headQ[1];
        out[2] = g_headQ[2]; out[3] = g_headQ[3];
        MemoryBarrier();
        if (s0 == g_headSeq) return;        // consistent snapshot
    }
}

void XR_GetHeadPos(float out[3])
{
    for (;;)
    {
        const long s0 = g_headSeq;
        if (s0 & 1) continue;
        MemoryBarrier();
        out[0] = g_headPos[0]; out[1] = g_headPos[1]; out[2] = g_headPos[2];
        MemoryBarrier();
        if (s0 == g_headSeq) return;
    }
}

void XR_Stats(unsigned long long* frames, unsigned long long* submitted, int* state)
{
    if (frames)    *frames = g_xrFrames;
    if (submitted) *submitted = g_xrSubmitted;
    if (state)     *state = (int)g_state;
}

void XR_Breakdown(XrTimeBreakdown* out)
{
    if (out) *out = g_tb;
}

void XR_Shutdown()
{
    if (g_stageL) { g_stageL->Release(); g_stageL = nullptr; }
    g_stageLValid = false;

    for (int eye = 0; eye < 2; ++eye)
        if (g_sc[eye]) { xrDestroySwapchain(g_sc[eye]); g_sc[eye] = XR_NULL_HANDLE; }
    if (g_space) { xrDestroySpace(g_space);     g_space = XR_NULL_HANDLE; }
    if (g_viewSpace) { xrDestroySpace(g_viewSpace); g_viewSpace = XR_NULL_HANDLE; }
    if (g_session) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
    if (g_inst) { xrDestroyInstance(g_inst);   g_inst = XR_NULL_HANDLE; }
    g_init = g_running = false;
}