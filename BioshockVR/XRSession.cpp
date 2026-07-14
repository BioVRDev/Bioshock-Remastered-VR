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
static bool g_running = false;
static bool g_init = false;

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static unsigned g_w = 0, g_h = 0;

// ONE PERSISTENT TEXTURE PER EYE. The backbuffer only ever holds ONE eye's
// image, so the other eye's most recent image has to live somewhere. Here.
static ID3D11Texture2D* g_stage[2] = { nullptr, nullptr };
static bool             g_stageValid[2] = { false, false };

static XrViewConfigurationView g_viewCfg[2] = {};

static unsigned long long g_xrFrames = 0;
static unsigned long long g_xrSubmitted = 0;

static XrFovf g_gameFov = {};
static bool   g_haveGameFov = false;
static bool   g_loggedHmdFov = false;

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

static bool EnsureStage(int i, ID3D11Texture2D* like)
{
    if (g_stage[i]) return true;
    if (!g_dev || !like) return false;

    D3D11_TEXTURE2D_DESC d = {};
    like->GetDesc(&d);
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;

    HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &g_stage[i]);
    if (FAILED(hr) || !g_stage[i])
    {
        g_stage[i] = nullptr;
        Log(">>> XR: !!! staging[%d] CreateTexture2D FAILED hr=0x%08X", i, (unsigned)hr);
        return false;
    }
    Log(">>> XR: staging[%d] %ux%u DXGI %d created", i, d.Width, d.Height, (int)d.Format);
    return true;
}

// ONE full XR frame cycle. Every OpenXR call individually timed.
static void SubmitPair(ID3D11Texture2D* leftImg, ID3D11Texture2D* rightImg)
{
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
                pv[e].pose = views[e].pose;
                pv[e].fov = g_haveGameFov ? g_gameFov : views[e].fov;
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

// --- XRMode=1. PHASE 5, KNOWN GOOD. Mono, full cycle every Present.
void XR_SubmitMono(ID3D11Texture2D* image)
{
    if (!g_init) return;
    PumpEvents();
    if (!g_running || !image) return;

    SubmitPair(image, image);
}

// --- XRMode=2. §4's cadence: submit every 2nd Present. THE REGRESSION.
void XR_SubmitEye(ID3D11Texture2D* image, int eye)
{
    if (!g_init) return;
    PumpEvents();
    if (!g_running || !image) return;

    if (eye == 0)
    {
        if (!EnsureStage(0, image)) return;
        LARGE_INTEGER a, b;
        QPC(a);
        g_ctx->CopyResource(g_stage[0], image);
        QPC(b);
        g_tb.copy += MS(a, b);
        g_stageValid[0] = true;
        return;
    }

    SubmitPair(g_stageValid[0] ? g_stage[0] : image, image);
    g_stageValid[0] = false;
}

// --- XRMode=3. THE FIX. Full cycle EVERY Present; submit rate stays 118/sec.
void XR_SubmitAER(ID3D11Texture2D* image, int eye)
{
    if (!g_init) return;
    PumpEvents();
    if (!g_running || !image) return;
    if (eye < 0 || eye > 1) return;

    if (!EnsureStage(eye, image)) return;

    LARGE_INTEGER a, b;
    QPC(a);
    g_ctx->CopyResource(g_stage[eye], image);   // this eye is now FRESH
    QPC(b);
    g_tb.copy += MS(a, b);
    g_stageValid[eye] = true;

    // The other eye re-shows its image from the previous Present. That one-frame
    // stagger IS the AER artifact, and it is the entire trade.
    // Until both eyes have rendered once, show the fresh image to both.
    ID3D11Texture2D* L = g_stageValid[0] ? g_stage[0] : image;
    ID3D11Texture2D* R = g_stageValid[1] ? g_stage[1] : image;

    SubmitPair(L, R);
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
    for (int i = 0; i < 2; ++i)
    {
        if (g_stage[i]) { g_stage[i]->Release(); g_stage[i] = nullptr; }
        g_stageValid[i] = false;
    }

    for (int eye = 0; eye < 2; ++eye)
        if (g_sc[eye]) { xrDestroySwapchain(g_sc[eye]); g_sc[eye] = XR_NULL_HANDLE; }
    if (g_space) { xrDestroySpace(g_space);     g_space = XR_NULL_HANDLE; }
    if (g_session) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
    if (g_inst) { xrDestroyInstance(g_inst);   g_inst = XR_NULL_HANDLE; }
    g_init = g_running = false;
}