#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include "XRSession.h"
#include "InputHook.h"

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
extern float g_cfgMenuSize;
extern float g_cfgMenuDist;
extern bool  g_cfgCrosshair;
extern float g_cfgHudWidthDeg;
extern float g_cfgHudDist;
extern float g_cfgHudPitchDeg;
extern float g_cfgHudYawDeg;
extern float g_cfgXhSize;    // DOT diameter, metres, at g_cfgXhDist
extern float g_cfgXhDist;
extern float g_cfgMenuHeight;
extern float g_cfgHeightOffset;   // CameraHeightOffset, cm (dllmain.cpp)
extern float g_cfgPlasmidAimPitch;      // dllmain.cpp
bool HandsProbe_AbilityMode();          // HandsProbe.cpp
extern int   g_cfgAimSource;   // dllmain.cpp -- 1 == motion aim (right controller)
bool CameraHook_GetAimOffset(float* dYawDeg, float* dPitchDeg);
bool CameraHook_GetLatchedPose(float quat[4], float pos[3]);   // CameraHook.cpp
void CameraHook_OffsetQuat(const float in[4], const float pyr[3], float out[4]);
extern float g_cfgCursorRot[3];

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

// Menus are WORLD-locked, not head-locked: a screen pinned to your face is a
// motion-sickness generator. We anchor it once, where you were looking when the
// menu came up, and leave it in the room until the menu closes.
static bool    g_menuAnchorSet = false;
static XrPosef g_menuAnchor = {};

// S24: where the head WAS when the anchor was taken. At startup the first valid
// pose can be the headset sitting on a desk -- one run anchored at y = -1.01 m,
// a metre below the player, so the menu hung far below and the player felt
// suspended above it. Nothing ever re-anchored because g_menuAnchorSet only
// clears in SubmitPair, which never runs while a pre-game menu is up.
// So: if the head is now far from where it was at anchor time, re-anchor.
static float g_menuAnchorHead[3] = { 0.f, 0.f, 0.f };
static const float kMenuReanchorM = 0.45f;   // metres of head travel

// Drop the anchor so the next menu/theater frame takes a fresh one. Called from
// Present, the same thread that owns g_menuAnchorSet -- no synchronisation
// needed, and it must live below the declaration above.
void XR_ResetMenuAnchor() { g_menuAnchorSet = false; }

static XrSwapchain g_sc[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static std::vector<XrSwapchainImageD3D11KHR> g_scImages[2];

// ---- crosshair (S18) ------------------------------------------------------
// The game's reticle is a big translucent star drawn at BACKBUFFER CENTRE, so
// it is baked into the projection layer and inherits every one of that layer's
// problems. Ours is a separate head-locked QUAD layer: composited by the
// runtime at display time, so it is pixel-stable and latency-free no matter
// what the game's frame is doing. Because head-aim is on, "where you look" IS
// "where you shoot", so a view-space dot is honest by construction.
static const int   kXhPx = 64;      // texture is 64x64
static const float kXhDotFrac = 28.0f / 64.0f;   // white dot / quad edge
static XrSwapchain g_xhSc = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageD3D11KHR> g_xhImages;
static ID3D11Texture2D* g_xhSrc = nullptr;
static bool             g_xhReady = false;

// ---- HUD quad --------------------------------------------------------------
// The interface, captured off the eye texture by DrawHook, composited by the
// runtime as its own quad. Created LAZILY on first use rather than in XR_Init:
// the capture surface only exists once the classifier has locked, and its size
// comes from the game's composite target, which we should read rather than
// assume matches the values XR_Init was handed.
static XrSwapchain g_hudSc = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageD3D11KHR> g_hudImages;
static unsigned    g_hudScW = 0, g_hudScH = 0;
static int64_t     g_scFormat = 0;          // filled in XR_Init from `chosen`

// Live-tunable placement. Angular width is the scale; the rest is where it sits.
static float g_hudWidthDeg = 70.0f;
static float g_hudDist = 2.0f;      // metres
static float g_hudYawDeg = 0.0f;      // + right
static float g_hudPitchDeg = 0.0f;      // + up
static int   g_hudEditParam = 0;        // 0 width, 1 dist, 2 pitch, 3 yaw

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

ID3D11Texture2D* DrawHook_HudTexture();   // DrawHook.cpp
bool             DrawHook_HudCaptured();  // DrawHook.cpp

// Returns false until the capture surface exists and a matching swapchain has
// been made for it. Re-creates on a resolution change.
static bool EnsureHudSwapchain()
{
    ID3D11Texture2D* src = DrawHook_HudTexture();
    if (!src || !g_scFormat) return false;

    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    if (!d.Width || !d.Height) return false;

    if (g_hudSc != XR_NULL_HANDLE && d.Width == g_hudScW && d.Height == g_hudScH)
        return true;

    if (g_hudSc != XR_NULL_HANDLE)
    {
        xrDestroySwapchain(g_hudSc);
        g_hudSc = XR_NULL_HANDLE;
        g_hudImages.clear();
    }

    XrSwapchainCreateInfo xc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    xc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    xc.format = g_scFormat;
    xc.sampleCount = 1;
    xc.width = d.Width;
    xc.height = d.Height;
    xc.faceCount = 1;
    xc.arraySize = 1;
    xc.mipCount = 1;

    if (XR_FAILED(xrCreateSwapchain(g_session, &xc, &g_hudSc)))
    {
        Log(">>> XR: !!! hud xrCreateSwapchain failed (%ux%u)", d.Width, d.Height);
        g_hudSc = XR_NULL_HANDLE;
        return false;
    }

    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_hudSc, 0, &n, nullptr);
    g_hudImages.resize(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xrEnumerateSwapchainImages(g_hudSc, n, &n,
        (XrSwapchainImageBaseHeader*)g_hudImages.data());

    g_hudScW = d.Width; g_hudScH = d.Height;
    Log(">>> XR: HUD quad swapchain %ux%u, %u images", d.Width, d.Height, n);
    return true;
}

static bool KeyFired(int vk, bool& prev)
{
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = down && !prev;
    prev = down;
    return fired;
}

// S56: the FOV keys are superseded by ForegroundFovValue, and numpad 0/2/4/5/6/7/8
// now tune HandsGripOffset in HandsProbe.cpp. Reused for HUD quad placement,
// which is the same kind of problem: a number nobody can guess from a desk.
static void PollFovKeys()
{

    // What does this keyboard ACTUALLY send? Legends lie, Fn layers remap, and
    // NumLock silently swaps the whole numpad between two VK sets. Restricted to
    // the nav cluster, numpad and F-row, so ordinary WASD play logs nothing.
    {
        static bool prev[256] = {};
        for (int vk = 0x21; vk <= 0x87; ++vk)
        {
            if (vk == 0x30) vk = 0x60;              // skip the alphanumerics
            const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (d && !prev[vk]) Log("KEY: vk 0x%02X (%d)", vk, vk);
            prev[vk] = d;
        }
    }

    static bool pEnd = false, pIns = false, pDel = false;

    if (KeyFired(VK_DELETE, pEnd))
    {
        g_hudEditParam = (g_hudEditParam + 1) % 4;
        static const char* names[4] = { "WIDTH (deg)", "DISTANCE (m)",
                                        "PITCH (deg)", "YAW (deg)" };
        Log(">>> HUD QUAD: now editing %s", names[g_hudEditParam]);
    }

    int dir = 0;
    if (KeyFired(VK_F11, pIns)) dir = -1;
    if (KeyFired(VK_F12, pDel)) dir = +1;
    if (!dir) return;

    switch (g_hudEditParam)
    {
    case 0: g_hudWidthDeg += dir * 2.0f;
        if (g_hudWidthDeg < 10.f)  g_hudWidthDeg = 10.f;
        if (g_hudWidthDeg > 140.f) g_hudWidthDeg = 140.f;
        break;
    case 1: g_hudDist += dir * 0.1f;
        if (g_hudDist < 0.4f) g_hudDist = 0.4f;
        if (g_hudDist > 8.0f) g_hudDist = 8.0f;
        break;
    case 2: g_hudPitchDeg += dir * 1.0f;  break;
    case 3: g_hudYawDeg += dir * 1.0f;  break;
    }

    // Printed in a form you can paste straight into the ini once these become
    // config keys, so a good setting found in the headset cannot be lost.
    Log(">>> HUD QUAD: HudWidthDeg=%.1f HudDist=%.2f HudPitchDeg=%.1f HudYawDeg=%.1f",
        g_hudWidthDeg, g_hudDist, g_hudPitchDeg, g_hudYawDeg);
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

    g_scFormat = chosen;

    // Seeded from the ini; DEL / F11 / F12 still adjust these live, and every
    // change logs a line you can paste straight back into the ini.
    g_hudWidthDeg = g_cfgHudWidthDeg;
    g_hudDist = g_cfgHudDist;
    g_hudPitchDeg = g_cfgHudPitchDeg;
    g_hudYawDeg = g_cfgHudYawDeg;

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

    // ---- crosshair swapchain + dot texture --------------------------------
    if (g_cfgCrosshair)
    {
        XrSwapchainCreateInfo xc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        xc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        xc.format = chosen;
        xc.sampleCount = 1;
        xc.width = kXhPx;
        xc.height = kXhPx;
        xc.faceCount = 1;
        xc.arraySize = 1;
        xc.mipCount = 1;

        if (XR_SUCCEEDED(xrCreateSwapchain(g_session, &xc, &g_xhSc)))
        {
            uint32_t n = 0;
            xrEnumerateSwapchainImages(g_xhSc, 0, &n, nullptr);
            g_xhImages.resize(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            xrEnumerateSwapchainImages(g_xhSc, n, &n,
                (XrSwapchainImageBaseHeader*)g_xhImages.data());

            // Filled white dot, thin dark ring for contrast against a bright
            // wall, hard zero alpha outside. Alpha is UNPREMULTIPLIED.
            static unsigned char px[kXhPx * kXhPx * 4];
            const float c = (kXhPx - 1) * 0.5f;
            const float rIn = kXhPx * kXhDotFrac * 0.5f;   // white radius
            const float rOut = rIn + 3.0f;                 // ring radius
            for (int yy = 0; yy < kXhPx; ++yy)
                for (int xx = 0; xx < kXhPx; ++xx)
                {
                    const float dx = xx - c, dy = yy - c;
                    const float r = sqrtf(dx * dx + dy * dy);
                    unsigned char* o = &px[(yy * kXhPx + xx) * 4];
                    float lum = 255.f, a = 0.f;
                    if (r <= rIn) { lum = 255.f; a = 255.f; }
                    else if (r <= rIn + 1.f) { lum = 255.f; a = 255.f * (rIn + 1.f - r); }
                    else if (r <= rOut) { lum = 0.f;   a = 190.f; }
                    else if (r <= rOut + 1.f) { lum = 0.f;   a = 190.f * (rOut + 1.f - r); }
                    o[0] = (unsigned char)lum; o[1] = (unsigned char)lum;
                    o[2] = (unsigned char)lum; o[3] = (unsigned char)a;
                }

            D3D11_TEXTURE2D_DESC td = {};
            td.Width = kXhPx; td.Height = kXhPx;
            td.MipLevels = 1; td.ArraySize = 1;
            td.Format = (DXGI_FORMAT)chosen;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA sd = {};
            sd.pSysMem = px;
            sd.SysMemPitch = kXhPx * 4;

            if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &g_xhSrc)) && g_xhSrc)
            {
                g_xhReady = true;
                const float edge = g_cfgXhSize / kXhDotFrac;
                Log(">>> XR: CROSSHAIR ready. dot %.1f mm at %.2f m = %.2f deg",
                    g_cfgXhSize * 1000.f, g_cfgXhDist,
                    2.f * atanf(g_cfgXhSize * 0.5f / g_cfgXhDist) * 57.2958f);
                (void)edge;
            }
            else Log(">>> XR: !!! crosshair CreateTexture2D failed");
        }
        else Log(">>> XR: !!! crosshair xrCreateSwapchain failed");
    }
    else Log(">>> XR: crosshair DISABLED by ini (EnableCrosshair=0)");

    // Touch controllers -> virtual pad. MUST happen before the session is begun:
    // xrAttachSessionActionSets is one-shot for the life of the session.
    Input_XrCreate(g_inst, g_session);

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

// Publish head orientation + centre for the game-thread camera write, and for
// the menu anchor. MUST be called from EVERY path that runs an XR frame -- the
// menu path included. It was originally only called from SubmitPair, which meant
// g_headQ/g_headPos were whatever gameplay last left there. At startup gameplay
// has never run, so they were still identity/origin, and the 2K-logo quad got
// anchored to LOCAL forward at LOCAL origin instead of in front of the player.
// Viewed from anywhere else that is an oblique rectangle -- i.e. a TRAPEZOID.
static void PublishHead(const XrView views[2])
{
    _InterlockedIncrement(&g_headSeq);          // odd == writing
    MemoryBarrier();
    g_headQ[0] = views[0].pose.orientation.x;
    g_headQ[1] = views[0].pose.orientation.y;
    g_headQ[2] = views[0].pose.orientation.z;
    g_headQ[3] = views[0].pose.orientation.w;
    // Head CENTER = midpoint of the two eye positions (each view pose is an EYE,
    // offset by half the IPD -- the midpoint cancels that).
    g_headPos[0] = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
    g_headPos[1] = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
    g_headPos[2] = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
    MemoryBarrier();
    _InterlockedIncrement(&g_headSeq);          // even == done
}

// Rotate vector v by unit quaternion q (x,y,z,w).
static void XhQuatRotate(const float q[4], const float v[3], float out[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.f * (y * v[2] - z * v[1]);
    const float ty = 2.f * (z * v[0] - x * v[2]);
    const float tz = 2.f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
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

    Input_XrSync(fs.predictedDisplayTime, g_space);

    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[2] = {};
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    const XrCompositionLayerBaseHeader* layers[3] = { nullptr, nullptr, nullptr };
    uint32_t layerCount = 0;
    XrCompositionLayerQuad xh = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad hq = { XR_TYPE_COMPOSITION_LAYER_QUAD };

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
            PublishHead(views);

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

            // ---- crosshair quad: head-locked, aim offset computed HERE on the
            //      render thread from fresh poses so head motion can't drag it --
            if (g_cfgCrosshair && g_xhReady)
            {
                uint32_t xi = 0;
                XrSwapchainImageAcquireInfo xai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XrSwapchainImageWaitInfo    xwi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                xwi.timeout = XR_INFINITE_DURATION;

                if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_xhSc, &xai, &xi)) &&
                    XR_SUCCEEDED(xrWaitSwapchainImage(g_xhSc, &xwi)))
                {
                    g_ctx->CopyResource(g_xhImages[xi].texture, g_xhSrc);
                    XrSwapchainImageReleaseInfo xri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    xrReleaseSwapchainImage(g_xhSc, &xri);

                    const float edge = g_cfgXhSize / kXhDotFrac;
                    xh.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    xh.space = g_viewSpace;                 // head-locked
                    xh.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    xh.subImage.swapchain = g_xhSc;
                    xh.subImage.imageRect.offset = { 0, 0 };
                    xh.subImage.imageRect.extent = { kXhPx, kXhPx };
                    xh.subImage.imageArrayIndex = 0;
                    xh.pose.orientation = { 0.f, 0.f, 0.f, 1.f };
                    xh.size = { edge, edge };

                    const float xd = g_cfgXhDist;

                    // Direction the gun points, expressed in HEAD-LOCAL space:
                    //   aim_world = controller aim quat * forward(-Z)
                    //   aim_local = conj(head quat) * aim_world
                    // Both quaternions are read on THIS thread, THIS frame, so the
                    // head term matches the pose the runtime reprojects against --
                    // no stale game-thread head, so the dot stops sliding.
                    static float lastX = 0.f, lastY = 0.f, lastZ = -1.f;
                    float dl[3] = { lastX, lastY, lastZ };

                    // FRESH controller minus FRESH head, both read on THIS thread
                    // THIS frame. CameraHook_GetAimOffset was wrong here: it is a
                    // head-relative offset computed on the GAME thread against a
                    // head latched at eye-0 time, while this quad is reprojected
                    // against the runtime's predicted head at display time. That
                    // stale head term IS the dot following your head. Fresh minus
                    // fresh has no such term.
                    //
                    // Accepted cost: this ignores the clamp and the smoothing, so
                    // the dot can trail the shot by the smoothing time and diverge
                    // past AimClampDeg. Bounded and small at 0.5 / 80 deg.
                    HandPose hp = {};
                    const int xhHand = HandsProbe_AbilityMode() ? HAND_LEFT : HAND_RIGHT;
                    const bool haveAim = (g_cfgAimSource == 1) &&
                        Input_GetHandPose(xhHand, &hp) && hp.aimValid;
                    if (haveAim)
                    {
                        float qc[4];
                        CameraHook_OffsetQuat(hp.aimQuat, g_cfgCursorRot, qc);

                        const float fwd[3] = { 0.f, 0.f, -1.f };
                        float aw[3];
                        XhQuatRotate(qc, fwd, aw);                      // aim in world

                        const XrQuaternionf& Q = views[0].pose.orientation;
                        const float hc[4] = { -Q.x, -Q.y, -Q.z, Q.w };  // conjugate
                        XhQuatRotate(hc, aw, dl);                       // aim head-local

                        // Same palm correction the aim write applies, so the dot and
                        // the shot pitch together instead of splitting apart.
                        if (HandsProbe_AbilityMode())
                        {
                            const float rp = g_cfgPlasmidAimPitch * 0.01745329f;
                            const float cs = cosf(rp), sn = sinf(rp);
                            const float y = dl[1], z = dl[2];
                            dl[1] = y * cs - z * sn;
                            dl[2] = y * sn + z * cs;
                        }

                        float n = sqrtf(dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2]);
                        if (n > 1e-4f) { dl[0] /= n; dl[1] /= n; dl[2] /= n; }
                        lastX = dl[0]; lastY = dl[1]; lastZ = dl[2];
                    }
                    else if (g_cfgAimSource != 1)
                    {
                        dl[0] = 0.f; dl[1] = 0.f; dl[2] = -1.f;      // motion aim off
                    }
                    // else: motion aim on but a one-frame tracking blip -- hold last.

                    // CameraHeightOffset raises the camera above the pawn's eye,
                    // but shots still leave FROM the pawn's eye -- so the dot sat
                    // that far above every impact at every range. Drop the quad by
                    // the same amount and they coincide.
                    xh.pose.position = { xd * dl[0],
                                         xd * dl[1] - g_cfgHeightOffset * 0.01f,
                                         xd * dl[2] };
                    layers[1] = (const XrCompositionLayerBaseHeader*)&xh;
                    layerCount = 2;
                }
            }

            // ---- HUD quad ------------------------------------------------
            // Indexed by layerCount, not by a hardcoded slot: the crosshair may
            // or may not have claimed slot 1, and a null hole in the middle of
            // the array is an invalid submission.
            if (DrawHook_HudCaptured() && EnsureHudSwapchain())
            {
                uint32_t hi = 0;
                XrSwapchainImageAcquireInfo hai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XrSwapchainImageWaitInfo    hwi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                hwi.timeout = XR_INFINITE_DURATION;

                if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_hudSc, &hai, &hi)) &&
                    XR_SUCCEEDED(xrWaitSwapchainImage(g_hudSc, &hwi)))
                {
                    g_ctx->CopyResource(g_hudImages[hi].texture, DrawHook_HudTexture());
                    XrSwapchainImageReleaseInfo hri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    xrReleaseSwapchainImage(g_hudSc, &hri);

                    hq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    hq.space = g_viewSpace;              // head-locked for now
                    hq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    hq.subImage.swapchain = g_hudSc;
                    hq.subImage.imageRect.offset = { 0, 0 };
                    hq.subImage.imageRect.extent = { (int32_t)g_hudScW, (int32_t)g_hudScH };
                    hq.subImage.imageArrayIndex = 0;

                    // Angular width IS the scale control. Height follows from the
                    // capture's aspect -- deriving it any other way stretches the
                    // interface, and a stretched HUD reads as a broken one.
                    const float rad = g_hudWidthDeg * 0.5f * 0.01745329f;
                    const float wM = 2.f * g_hudDist * tanf(rad);
                    const float hM = wM * (float)g_hudScH / (float)g_hudScW;
                    hq.size = { wM, hM };

                    const float yaw = g_hudYawDeg * 0.01745329f;
                    const float pit = g_hudPitchDeg * 0.01745329f;
                    const float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
                    const float cp = cosf(pit * 0.5f), sp = sinf(pit * 0.5f);
                    hq.pose.orientation = { sp * cy, sy * cp, -sp * sy, cp * cy };
                    hq.pose.position = { g_hudDist * sinf(yaw),
                                         g_hudDist * sinf(pit),
                                        -g_hudDist * cosf(yaw) * cosf(pit) };

                    layers[layerCount] = (const XrCompositionLayerBaseHeader*)&hq;
                    ++layerCount;
                }
            }
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
    g_menuAnchorSet = false;   // next menu re-anchors to wherever you're facing
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

    Input_XrSync(fs.predictedDisplayTime, g_space);

    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
    uint32_t layerCount = 0;

    if (fs.shouldRender)
    {
        // Locate the head HERE. The menu path used to skip this entirely and
        // then anchor from g_headQ/g_headPos, which only gameplay ever wrote.
        // Consequence: any menu shown before the first gameplay frame (the 2K
        // logo, the startup movies, the main menu) anchored at identity /
        // LOCAL origin rather than in front of the player.
        {
            XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = g_space;

            XrViewState vs = { XR_TYPE_VIEW_STATE };
            XrView      mv[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
            uint32_t    got = 0;

            QPC(a);
            XrResult lr = xrLocateViews(g_session, &vli, &vs, 2, &got, mv);
            QPC(b);
            g_tb.locateViews += MS(a, b);

            if (XR_SUCCEEDED(lr) && got == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
            {
                PublishHead(mv);
            }
            else if (!g_menuAnchorSet)
            {
                // No valid pose yet -- do NOT anchor to a stale/identity one.
                // Skip this frame's anchor; we will get another Present in ~4ms.
                XrFrameEndInfo skip = { XR_TYPE_FRAME_END_INFO };
                skip.displayTime = fs.predictedDisplayTime;
                skip.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                skip.layerCount = 0;
                skip.layers = nullptr;
                xrEndFrame(g_session, &skip);
                ++g_tb.submits;
                return;
            }
        }

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

            // Drop a stale anchor if the head has since moved a long way --
            // headset picked up off a desk, player stood up, room-scale step.
            if (g_menuAnchorSet)
            {
                float now[3];
                XR_GetHeadPos(now);
                const float dx = now[0] - g_menuAnchorHead[0];
                const float dy = now[1] - g_menuAnchorHead[1];
                const float dz = now[2] - g_menuAnchorHead[2];
                if (dx * dx + dy * dy + dz * dz > kMenuReanchorM * kMenuReanchorM)
                {
                    g_menuAnchorSet = false;
                    Log(">>> MENU re-anchoring: head moved %.2f m from the anchor",
                        sqrtf(dx * dx + dy * dy + dz * dz));
                }
            }

            if (!g_menuAnchorSet)
            {
                float hq[4], hp[3];
                XR_GetHeadQuat(hq);
                XR_GetHeadPos(hp);

                // Head forward = q * (0,0,-1), then flattened to horizontal so
                // the screen is never tilted even if you looked up or down.
                float fx = -2.f * (hq[0] * hq[2] + hq[1] * hq[3]);
                float fz = -(1.f - 2.f * (hq[0] * hq[0] + hq[1] * hq[1]));
                float len = sqrtf(fx * fx + fz * fz);
                if (len < 1e-4f) { fx = 0.f; fz = -1.f; len = 1.f; }
                fx /= len; fz /= len;

                // The quad's visible face is its local +Z (identity
                // orientation faces a viewer at -Z -- which is why yaw~0
                // anchors always looked straight). We need local +Z
                // rotated onto -forward = (-fx, 0, -fz), and
                // R_y(yaw)*(0,0,1) = (sin yaw, 0, cos yaw), so:
                //     yaw = atan2(-fx, -fz)
                // The old atan2f(fx, -fz) built the MIRROR of the head
                // yaw: quad askew by 2*yaw, opposite sign looking left vs
                // right -- the S17 slight-rotation artifact.
                const float yaw = atan2f(-fx, -fz);
                g_menuAnchor.orientation = { 0.f, sinf(yaw * 0.5f), 0.f, cosf(yaw * 0.5f) };
                hp[1] += g_cfgMenuHeight;      // manual height nudge (S25)
                g_menuAnchor.position = { hp[0] + fx * g_cfgMenuDist,
                                          hp[1],
                                          hp[2] + fz * g_cfgMenuDist };
                g_menuAnchorSet = true;
                g_menuAnchorHead[0] = hp[0];
                g_menuAnchorHead[1] = hp[1];
                g_menuAnchorHead[2] = hp[2];
                Log(">>> MENU anchored at yaw %.1f deg, head (%.2f %.2f %.2f) m",
                    yaw * 57.2958f, hp[0], hp[1], hp[2]);
            }

            quad.space = g_space;                        // world-locked
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH; // MONO -- depth can't break
            quad.subImage.swapchain = g_sc[0];
            quad.subImage.imageRect.offset = { 0, 0 };
            quad.subImage.imageRect.extent = { (int32_t)g_w, (int32_t)g_h };
            quad.subImage.imageArrayIndex = 0;
            quad.pose = g_menuAnchor;

            // ASPECT. The quad was square while the image is 3072x3264 (0.94),
            // so every menu was stretched ~6% horizontally. quad.size is METRES
            // in the world; the runtime maps the whole imageRect onto it and
            // does NOT preserve aspect for you. MenuScreenSize = the LONG edge.
            {
                float qw = g_cfgMenuSize, qh = g_cfgMenuSize;
                if (g_w && g_h)
                {
                    if (g_w >= g_h) qh = g_cfgMenuSize * (float)g_h / (float)g_w;
                    else            qw = g_cfgMenuSize * (float)g_w / (float)g_h;
                }
                quad.size = { qw, qh };

                static bool loggedSz = false;
                if (!loggedSz)
                {
                    loggedSz = true;
                    Log(">>> MENU quad %.3f x %.3f m for a %ux%u image (aspect %.4f)",
                        qw, qh, g_w, g_h, (float)g_w / (float)g_h);
                }
            }
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

    if (g_xhSrc) { g_xhSrc->Release(); g_xhSrc = nullptr; }
    g_xhReady = false;
    if (g_xhSc) { xrDestroySwapchain(g_xhSc); g_xhSc = XR_NULL_HANDLE; }

    if (g_hudSc != XR_NULL_HANDLE) { xrDestroySwapchain(g_hudSc); g_hudSc = XR_NULL_HANDLE; }
    g_hudImages.clear();

    for (int eye = 0; eye < 2; ++eye)
        if (g_sc[eye]) { xrDestroySwapchain(g_sc[eye]); g_sc[eye] = XR_NULL_HANDLE; }
    if (g_space) { xrDestroySpace(g_space);     g_space = XR_NULL_HANDLE; }
    if (g_viewSpace) { xrDestroySpace(g_viewSpace); g_viewSpace = XR_NULL_HANDLE; }
    if (g_session) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
    if (g_inst) { xrDestroyInstance(g_inst);   g_inst = XR_NULL_HANDLE; }
    g_init = g_running = false;
}