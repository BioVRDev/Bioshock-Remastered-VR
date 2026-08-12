// BioshockVR/Render/Hooks.cpp
//
// Present hook, XR bring-up
//
// MEASURED: Present runs on the RENDER thread, CalcView on the GAME thread. So
// Present does NOT own the eye phase -- the camera hook tags each frame and
// Present pops the tag from a FIFO (CameraHook_NextEye).

#include "Render/Hooks.h"
#include "Render/XRSession.h"
#include "Camera/CameraHook.h"
#include "Hud/DrawHook.h"
#include "Input/InputHook.h"
#include "Game/GameState.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>

#include <MinHook.h>
#include "Core/Config.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

bool GameState_Paused();   // GameState.cpp
bool GameState_InGame();   // GameState.cpp
bool GameState_Cutscene();   // GameState.cpp

// THE theater signal. One predicate, read by the camera hook on the game thread
// and by Present on the render thread, so both cannot disagree about what state
// we are in. Context-driven and therefore instant; falls back to the pitch
// heuristic only while the context field is still unlocked.
bool GameState_Theater();


extern void  LogFile(const char* msg);

static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT SyncInterval, UINT Flags);

static PresentFn g_origPresent = nullptr;
static void* g_presentAddr = nullptr;

// ---- FLIP-MODEL CONVERSION (ForceFlipModel) -----------------------------
// MEASURED, same machine same hour on a 60 Hz monitor: windowed 60 Present/s,
// exclusive fullscreen 119. Windowed BitBlt presents are throttled by the
// desktop compositor to one per composition no matter what SyncInterval says,
// which is why DisableVSync never helped. Fullscreen escapes the cap but snaps
// the buffer to a real display mode -- 2750x2850 became 3840x2160 -- so it is
// not usable.
//
// The only route that is BOTH uncapped and free to pick any resolution is
// DXGI_PRESENT_ALLOW_TEARING, and that requires a FLIP swap effect. Neither the
// swap effect nor the buffer count can be changed after creation, so the
// description has to be rewritten on its way through CreateSwapChain.
//
// The probe confirmed all three prerequisites on this machine: OS/driver
// ALLOW_TEARING = 1, MSAA count already 1, and only GetBuffer(0) is ever used.
typedef HRESULT(__stdcall* CreateSwapChainFn)(IDXGIFactory*, IUnknown*,
    DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

static CreateSwapChainFn g_origCreateSwapChain = nullptr;
static void* g_createSwapChainAddr = nullptr;
static bool  g_flipLive = false;   // the rewrite actually took

// DXGI 1.2's entry point. MEASURED: slot 10 armed correctly and NEVER fired, so
// the game is not using the legacy call. IDXGIFactory2 vtable continues from
// IDXGIFactory1: 12 EnumAdapters1, 13 IsCurrent, 14 IsWindowedStereoEnabled,
// 15 CreateSwapChainForHwnd. Read from the header.
typedef HRESULT(__stdcall* CreateSCForHwndFn)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
    IDXGIOutput*, IDXGISwapChain1**);

static CreateSCForHwndFn g_origCreateSCForHwnd = nullptr;
static void* g_createSCForHwndAddr = nullptr;

static HRESULT __stdcall hkCreateSCForHwnd(IDXGIFactory2* self, IUnknown* dev,
    HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFs, IDXGIOutput* out,
    IDXGISwapChain1** ppSC)
{
    if (!pDesc) return g_origCreateSCForHwnd(self, dev, hwnd, pDesc, pFs, out, ppSC);

    // Log BEFORE the key check: with ForceFlipModel=0 this still tells you
    // which entry point the game actually uses, which is the open question.
    Log(">>> FLIP: CreateSwapChainForHwnd  %ux%u  effect %d  buffers %u  flags 0x%08X  msaa %u",
        pDesc->Width, pDesc->Height, (int)pDesc->SwapEffect,
        pDesc->BufferCount, pDesc->Flags, pDesc->SampleDesc.Count);

    if (!g_cfg.forceFlip || pDesc->SampleDesc.Count > 1 ||
        (pDesc->Width <= 128 && pDesc->Height <= 128))
        return g_origCreateSCForHwnd(self, dev, hwnd, pDesc, pFs, out, ppSC);

    DXGI_SWAP_CHAIN_DESC1 d = *pDesc;
    if (d.SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    else if (d.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    if (d.BufferCount < 2) d.BufferCount = 2;
    d.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    HRESULT hr = g_origCreateSCForHwnd(self, dev, hwnd, &d, pFs, out, ppSC);
    if (SUCCEEDED(hr))
    {
        g_flipLive = true;
        Log(">>> FLIP: CONVERTED (ForHwnd) -> effect %d, buffers %u, ALLOW_TEARING set.",
            (int)d.SwapEffect, d.BufferCount);
        return hr;
    }

    Log("!!! FLIP: ForHwnd rewrite REJECTED hr=0x%08X. Falling back to stock.",
        (unsigned)hr);
    return g_origCreateSCForHwnd(self, dev, hwnd, pDesc, pFs, out, ppSC);
}

static HRESULT __stdcall hkCreateSwapChain(IDXGIFactory* self, IUnknown* dev,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    if (pDesc)
        Log(">>> FLIP: CreateSwapChain (legacy)  %ux%u  effect %d  buffers %u",
            pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
            (int)pDesc->SwapEffect, pDesc->BufferCount);

    if (!g_cfg.forceFlip || !pDesc)
        return g_origCreateSwapChain(self, dev, pDesc, ppSC);;

    // Flip model cannot do MSAA on the backbuffer. The game's count is 1, but
    // refuse rather than fail the creation if that ever changes.
    if (pDesc->SampleDesc.Count > 1)
    {
        Log(">>> FLIP: skipping -- MSAA count %u, flip model requires 1.",
            pDesc->SampleDesc.Count);
        return g_origCreateSwapChain(self, dev, pDesc, ppSC);
    }

    // GrabVTable's own 100x100 throwaway is created BEFORE this hook is armed
    // so it cannot reach here, but the guard costs nothing and documents why.
    if (pDesc->BufferDesc.Width <= 128 && pDesc->BufferDesc.Height <= 128)
        return g_origCreateSwapChain(self, dev, pDesc, ppSC);

    Log(">>> FLIP: intercepted CreateSwapChain  %ux%u  effect %d  buffers %u  flags 0x%08X",
        pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
        (int)pDesc->SwapEffect, pDesc->BufferCount, pDesc->Flags);

    DXGI_SWAP_CHAIN_DESC d = *pDesc;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (d.BufferCount < 2) d.BufferCount = 2;
    d.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    HRESULT hr = g_origCreateSwapChain(self, dev, &d, ppSC);
    if (SUCCEEDED(hr))
    {
        g_flipLive = true;
        Log(">>> FLIP: CONVERTED -> FLIP_DISCARD, buffers %u, ALLOW_TEARING set.",
            d.BufferCount);
        return hr;
    }

    // SAFETY NET. A rejected description must never take the game down with it.
    // Fall straight back to exactly what the game asked for.
    Log("!!! FLIP: rewritten desc REJECTED hr=0x%08X. Falling back to stock.",
        (unsigned)hr);
    return g_origCreateSwapChain(self, dev, pDesc, ppSC);
}

static void* g_drawIndexedAddr = nullptr;
static void* g_drawAddr = nullptr;
static void* g_drawIdxInstAddr = nullptr;
static void* g_omSetRTAddr = nullptr;
static void* g_drawInstAddr = nullptr;
static bool  g_drawTried = false;

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static unsigned  g_bbW = 0, g_bbH = 0;
static bool      g_described = false;
static uint64_t  g_frames = 0;
static DWORD     g_lastTick = 0;

static bool g_xrTried = false;
static bool g_xrDead = false;
static bool g_camTried = false;
static bool g_loggedVSyncOverride = false;

// Where each Present's milliseconds go.
static LARGE_INTEGER g_qpf = {};
static LARGE_INTEGER g_lastPresentReturn = {};
static double g_msGame = 0.0;      // outside our hook -- the game's own work
static double g_msXr = 0.0;        // inside XR_Submit*
static double g_msPresent = 0.0;   // inside the game's real Present

static void DescribeOnce(IDXGISwapChain* sc)
{
    g_described = true;

    DXGI_SWAP_CHAIN_DESC d = {};
    if (SUCCEEDED(sc->GetDesc(&d)))
    {
        g_bbW = d.BufferDesc.Width;
        g_bbH = d.BufferDesc.Height;

        Log("--- GAME SWAPCHAIN ---");
        Log("  backbuffer : %u x %u", d.BufferDesc.Width, d.BufferDesc.Height);
        Log("  format     : DXGI %d", (int)d.BufferDesc.Format);
        Log("  buffers    : %u  (flip model needs >= 2)", d.BufferCount);
        Log("  windowed   : %s", d.Windowed ? "YES  (compositor caps the rate)"
            : "NO   (exclusive -- no cap)");
        Log("  samples    : %u (MSAA -- flip model needs 1)", d.SampleDesc.Count);
        Log("  hwnd       : 0x%08X", (unsigned)(uintptr_t)d.OutputWindow);

        // ---- CAN THIS SWAPCHAIN EVER TEAR? ------------------------------
        // ALLOW_TEARING is the only route to an uncapped framerate in a
        // WINDOW, and it needs a FLIP swap effect. Both the flag and the
        // effect are fixed at CREATION -- ResizeBuffers can change flags but
        // never the effect -- so a BitBlt swapchain cannot be upgraded in
        // place. This says whether that route is open before anyone writes a
        // creation hook. Read-only.
        const char* eff = "UNKNOWN";
        switch (d.SwapEffect)
        {
        case DXGI_SWAP_EFFECT_DISCARD:         eff = "DISCARD (BitBlt -- cannot tear)"; break;
        case DXGI_SWAP_EFFECT_SEQUENTIAL:      eff = "SEQUENTIAL (BitBlt -- cannot tear)"; break;
        case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: eff = "FLIP_SEQUENTIAL (can tear)"; break;
        case DXGI_SWAP_EFFECT_FLIP_DISCARD:    eff = "FLIP_DISCARD (can tear)"; break;
        }
        Log("  swapeffect : %d  %s", (int)d.SwapEffect, eff);
        Log("  flags      : 0x%08X", d.Flags);
        Log("  refresh    : %u/%u Hz",
            d.BufferDesc.RefreshRate.Numerator,
            d.BufferDesc.RefreshRate.Denominator);

        IDXGIFactory* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory), (void**)&fac)) && fac)
        {
            IDXGIFactory5* f5 = nullptr;
            if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5)) && f5)
            {
                BOOL allow = FALSE;
                if (SUCCEEDED(f5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                    Log("  tearing    : OS/driver support = %d", (int)allow);
                else
                    Log("  tearing    : CheckFeatureSupport failed");
                f5->Release();
            }
            else Log("  tearing    : no IDXGIFactory5 -- unavailable on this OS");
            fac->Release();
        }
    }
    else
    {
        Log("!!! GetDesc failed on the game swapchain");
    }

    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) && g_dev)
    {
        g_dev->GetImmediateContext(&g_ctx);
        Log("  device     : 0x%08X   context: 0x%08X",
            (unsigned)(uintptr_t)g_dev, (unsigned)(uintptr_t)g_ctx);
    }
    else
    {
        Log("!!! GetDevice failed -- this is NOT a D3D11 swapchain?");
    }
    Log("----------------------");
}

static HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags)
{
    if (!g_described)
    {
        Log(">>> PRESENT HOOK FIRED. First frame.");
        Log("present:  thread = %lu   (RENDER thread)", GetCurrentThreadId());
        Log("present:  SyncInterval = %u  Flags = 0x%X", SyncInterval, Flags);
        DescribeOnce(sc);
        g_lastTick = GetTickCount();
    }

    ++g_frames;

    // The game's own frame time: everything between Present returning and the
    // next Present arriving. Work we do not touch and cannot be blamed for.
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (g_lastPresentReturn.QuadPart)
    {
        LARGE_INTEGER nowIn;
        QueryPerformanceCounter(&nowIn);
        g_msGame += (double)(nowIn.QuadPart - g_lastPresentReturn.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
    }

    if (!g_xrTried && g_dev && g_ctx && g_bbW && g_bbH)
    {
        g_xrTried = true;

        Log(">>> Attempting XR_Init on the game's device (%ux%u)...", g_bbW, g_bbH);
        if (!XR_Init(g_dev, g_ctx, g_bbW, g_bbH))
        {
            g_xrDead = true;
            Log("!!! XR_Init FAILED. Running flat. Game is unaffected.");
        }
        else
        {
            XR_SetGameFov(g_cfg.fovDeg, g_bbW, g_bbH);

            // Derive the foreground projection from the REAL backbuffer, not
            // from ResolutionX/Y -- if the game ignored the requested size, the
            // ini values are a lie and the weapon projection would be wrong.
            // Resolution is the thing you choose; this follows it.
            //
            //   fgFov = 2*atan( tan(GameFovDegrees/2) * (4/3) / (W/H) )
            //
            // The 4/3 is the aspect the game's own foreground projection
            // assumes. At 3072x3264 / 110 this lands on 127.4, which is where
            // the hand-tuned 127 came from.
            if (g_cfg.fgFovAuto && g_bbW && g_bbH)
            {
                const double kPI = 3.14159265358979323846;
                const double aspect = (double)g_bbW / (double)g_bbH;
                const double half = g_cfg.fovDeg * 0.5 * (kPI / 180.0);
                const double v = 2.0 * atan(tan(half) * (4.0 / 3.0) / aspect)
                    * (180.0 / kPI);

                if (v > 5.0 && v < 170.0)
                {
                    Log(">>> FOV AUTO: backbuffer %ux%u (aspect %.4f), game FOV %.1f"
                        "  ->  ForegroundFovValue %.1f  (was %.1f)",
                        g_bbW, g_bbH, aspect, g_cfg.fovDeg, v, g_cfg.fgFovValue);
                    g_cfg.fgFovValue = (float)v;
                }
                else
                {
                    Log("!!! FOV AUTO: computed %.1f is out of range. Keeping %.1f.",
                        v, g_cfg.fgFovValue);
                }
            }

            // ---- GRIP OFFSET vs FOREGROUND FOV ------------------------------
            // The hands actor is placed in WORLD space but drawn by the
            // FOREGROUND projection, so its screen position is
            // (lateral / forward) / tan(fgFov/2), while the compositor reads the
            // backbuffer back at the TRUE world angle. Change the foreground FOV
            // -- which ForegroundFovAuto does on every resolution change -- and
            // the same world offset lands at a different apparent angle. Rescale
            // RIGHT and UP; FORWARD is a depth and does not move. RotOffset and
            // CursorOffset are angles and need no correction at all.
            if (g_cfg.gripTunedFgFov > 5.0f && g_cfg.fgFovValue > 5.0f)
            {
                const double kPI2 = 3.14159265358979323846;
                const double tNow = tan(g_cfg.fgFovValue * 0.5 * (kPI2 / 180.0));
                const double tRef = tan(g_cfg.gripTunedFgFov * 0.5 * (kPI2 / 180.0));
                const double k = (tRef > 1e-6) ? (tNow / tRef) : 1.0;

                if (k > 0.2 && k < 5.0 && (k < 0.999 || k > 1.001))
                {
                    for (int s = 0; s < 9; ++s)
                    {
                        g_cfg.gripSlot[s][1] = (float)(g_cfg.gripSlot[s][1] * k);
                        g_cfg.gripSlot[s][2] = (float)(g_cfg.gripSlot[s][2] * k);
                    }
                    g_cfg.handsGrip[1] = (float)(g_cfg.handsGrip[1] * k);
                    g_cfg.handsGrip[2] = (float)(g_cfg.handsGrip[2] * k);

                    Log(">>> GRIP SCALE: tuned at fgFov %.1f, running at %.1f"
                        "  ->  right/up x %.4f",
                        g_cfg.gripTunedFgFov, g_cfg.fgFovValue, k);
                    for (int s = 0; s < 9; ++s)
                        Log("   GripOffset%d=%.1f,%.1f,%.1f", s,
                            g_cfg.gripSlot[s][0], g_cfg.gripSlot[s][1],
                            g_cfg.gripSlot[s][2]);
                }
                else
                {
                    Log(">>> GRIP SCALE: factor %.4f -- no correction applied.", k);
                }
            }

        }

        // Camera hook on the render thread at the first frame -- NOT at DllMain,
        // where the exe may still be packed.
        if (!g_camTried)
        {
            g_camTried = true;
            if (g_cfg.cameraHook)
                CameraHook_Install();
            else
                Log("camera: DISABLED by BioshockVR.ini (EnableCameraHook=0)");
        }

        // Draw hook, same reasoning as the camera hook: render thread, first
        // frame, never DllMain.
        if (!g_drawTried)
        {
            g_drawTried = true;
            DrawHook_Install(g_drawIndexedAddr, g_drawAddr,
                g_drawIdxInstAddr, g_drawInstAddr, g_omSetRTAddr);
        }
    }

    // Resource identity for the redirect. Done every Present rather than once,
    // because the backbuffer is recreated on a resize or a fullscreen toggle and
    // a cached pointer would then match nothing -- silently, for the rest of the
    // run. GetBuffer is a refcount op; this is not a measurable cost.
    {
        ID3D11Texture2D* bb0 = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb0)) && bb0)
        {
            DrawHook_SetBackbuffer(bb0);
            bb0->Release();
        }
    }

    // Pop the eye tag for the frame we are about to present. ALWAYS, once per
    // Present, so the FIFO stays drained even before XR comes up.
    const int eye = CameraHook_NextEye();

    if (!g_xrDead && XR_IsInit())
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb)
        {
            LARGE_INTEGER x0, x1;
            QueryPerformanceCounter(&x0);

            static bool menuMode = false;

            const bool starved = CameraHook_Starved();
            const bool drawMenu = DrawHook_MenuUp();
            const bool paused = GameState_Paused();

            // Two independent routes to the same quad. The classifier catches
            // prerendered movies, which is what the intro is; the context
            // whitelist is left in for in-engine cutscenes, where the world DOES
            // render and the classifier will not fire.
            const bool theater = g_cfg.cutsceneTheater &&
                (DrawHook_NoWorldRender() || GameState_Theater());

            // Without this the cutscene inherits whatever anchor the previous
            // screen left behind -- typically the main menu's, taken while you
            // were facing somewhere else entirely.
            {
                static bool wasTheater = false;
                if (theater != wasTheater)
                {
                    XR_ResetMenuAnchor();
                    Log(">>> THEATER ROUTE: %s", theater ? "QUAD" : "stereo");
                    wasTheater = theater;
                }
            }

            // Three sources, and only the movie-name ones are trusted going
            // forward: the draw-count signature list is empty and stays empty,
            // because counts false-fire during ordinary play, while a named
            // Flash movie cannot be confused by geometry.
            // A FOLLOW screen takes the same route as an anchored one -- both
            // want the whole composed frame on the menu quad. They differ only
            // in how often that quad's pose is recomputed, which XRSession
            // decides.
            //
            // The OR used to live here. It now lives in DrawHook, because the
            // HUD capture has to answer the same question -- a screen shown as
            // the composed frame must not have its interface lifted out of that
            // frame first, which is what emptied the map. One owner, two
            // consumers, no way for them to drift apart.
            const bool anchorUi = DrawHook_ComposedFrameUp();

            // Theater is a RENDERING-MODE decision, not a menu feature. Gating
            // it behind EnableMenuScreen means turning menus off also disables
            // cutscene handling, which is not a relationship anyone would
            // expect from either switch.
            // A PANEL screen stands the flat route down entirely -- including
            // the `paused` and `starved` terms, which is the whole point. Its
            // interface is being captured onto the HUD quad instead and the
            // world keeps rendering in stereo behind it; letting the mono route
            // fire as well would flatten that world AND draw the interface a
            // second time inside the flat picture.
            const bool ordinaryMenu = g_cfg.menuScreen &&
                !GameState_PanelMovieUp() &&
                (starved || paused || anchorUi ||
                    (drawMenu && !GameState_InGame()));

            // LATCH THE DECISION AT EYE 0 AND HOLD IT THROUGH EYE 1.
            //
            // Present fires TWICE per stereo pair, and this block re-evaluated
            // on both. XR_SubmitPair stashes eye 0 and only submits on eye 1,
            // so a mid-pair flip left xrBeginFrame with no matching xrEndFrame
            // and then ran a whole second frame through the mono path. That
            // strands the eye-0 image and hands the compositor a mono quad it
            // did not expect. The latch below is the fix and must stay.
            //
            // HISTORICAL CORRECTION: this comment used to claim the resulting
            // artifact WAS the flat "second copy of the world" square. It was
            // not. That square survived every mono/pair/theater change and was
            // finally isolated by the HudRedirect=0 A/B to the HUD capture --
            // a textured full-screen quad landing in the capture slot, fixed in
            // DrawHook.cpp with a PSSrv0Res guard. Two different bugs that
            // happened to look alike. See docs/INVARIANTS.md.
            //
            // starved is a 250 ms timer and drawMenu is per-frame; both can
            // change between two Presents 4 ms apart.
            static bool pairMono = false;
            if (eye == 0) pairMono = (theater || ordinaryMenu);

            if (pairMono)
            {
                if (!menuMode)
                {
                    menuMode = true;
                    Log(">>> MENU SCREEN ON (%s%s%s%s%s)",
                        starved ? "camera starved " : "",
                        theater ? "cutscene " : "",
                        paused ? "paused " : "",
                        anchorUi ? "anchor-ui " : "",
                        drawMenu ? "drawmenu" : "");
                }

                // ONE EYE, NOT BOTH. Present fires twice per stereo pair and
                // the backbuffer holds a DIFFERENT eye's image each time, so
                // submitting on both fed the flat panel the left view and then
                // the right view, alternating -- one IPD of horizontal shimmer
                // on a surface the player is trying to read. Reported as "you
                // can see both frames being submitted" in the background.
                //
                // A flat quad wants ONE image, so take every other Present and
                // always the same eye. The panel is world-locked, so the
                // compositor reprojects the frames we skip; and with the view
                // now held still behind an interface there is nothing moving in
                // the image for the halved rate to show.
                if (eye == 0) XR_SubmitMenuMono(bb);
            }
            else
            {
                if (menuMode) { menuMode = false; Log(">>> MENU SCREEN off (camera ticking)"); }
                XR_SubmitPair(bb, eye);
            }
            // pairMono is only recomputed at eye 0, so both halves of every
            // pair always take the same path.

            QueryPerformanceCounter(&x1);
            g_msXr += (double)(x1.QuadPart - x0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;

            bb->Release();
        }
        else if ((g_frames % 600) == 0)
        {
            Log("!!! GetBuffer(0) failed in Present");
        }
    }

    DWORD now = GetTickCount();
    if (now - g_lastTick >= 1000)
    {
        static uint64_t lastFrames = 0;
        static unsigned long long lastSubmitted = 0;
        static double lastGame = 0.0, lastXr = 0.0, lastPresent = 0.0;
        static XrTimeBreakdown lastTb = {};

        unsigned long long xrf = 0, xrs = 0;
        int st = 0;
        XR_Stats(&xrf, &xrs, &st);

        const uint64_t dF = g_frames - lastFrames;

        Log("frames: %llu (~%llu Present/s)   submitted %llu (+%llu/s)   state %d",
            g_frames, dF, xrs, xrs - lastSubmitted, st);

        if (dF)
        {
            Log("  PER PRESENT: game %.2f | XR %.2f | origPresent %.2f  (ms)",
                (g_msGame - lastGame) / (double)dF,
                (g_msXr - lastXr) / (double)dF,
                (g_msPresent - lastPresent) / (double)dF);
        }

        XrTimeBreakdown tb = {};
        XR_Breakdown(&tb);
        const unsigned long long dS = tb.submits - lastTb.submits;
        if (dS)
        {
            Log("  PER SUBMIT:  wait %.2f | begin %.2f | locate %.2f | acquire %.2f | copy %.2f | end %.2f  (ms)",
                (tb.waitFrame - lastTb.waitFrame) / (double)dS,
                (tb.beginFrame - lastTb.beginFrame) / (double)dS,
                (tb.locateViews - lastTb.locateViews) / (double)dS,
                (tb.acquire - lastTb.acquire) / (double)dS,
                (tb.copy - lastTb.copy) / (double)dS,
                (tb.endFrame - lastTb.endFrame) / (double)dS);
        }

        int qmin = -1, qmax = -1;
        unsigned und = 0;
        CameraHook_EyeQueueStats(&qmin, &qmax, &und);
        Log("  EYEQ: depth min=%d max=%d  underruns=%u", qmin, qmax, und);

        lastFrames = g_frames;
        lastSubmitted = xrs;
        lastGame = g_msGame;
        lastXr = g_msXr;
        lastPresent = g_msPresent;
        lastTb = tb;
        g_lastTick = now;
    }

    if (g_cfg.disableVSync)
    {
        if (SyncInterval != 0 && !g_loggedVSyncOverride)
        {
            g_loggedVSyncOverride = true;
            Log(">>> VSYNC OVERRIDE: game asked SyncInterval=%u, presenting with 0.", SyncInterval);
        }
        SyncInterval = 0;
    }

    // Tearing is only legal at SyncInterval 0, and only on a swapchain actually
    // CREATED with the flag -- passing it otherwise fails the Present outright.
    if (g_flipLive && SyncInterval == 0)
    {
        static bool loggedTear = false;
        if (!loggedTear)
        {
            loggedTear = true;
            Log(">>> FLIP: presenting with ALLOW_TEARING. Compositor cap is off.");
        }
        Flags |= DXGI_PRESENT_ALLOW_TEARING;
    }

    DrawHook_EndFrame();
    Input_Tick();
    CameraHook_LateHandsWrite();

    // MEASURED, 60 Hz monitor, gameplay:
    //   every frame  ->  60 Present/s, origPresent 6.5 ms   (compositor tax)
    //   never        ->  85 Present/s, game 11.7 ms         (driver stalls)
    //   every 4th    -> 240 Present/s, origPresent 0.03 ms  (both avoided)
    //
    // Present is not just a display operation -- it is the frame boundary the
    // driver needs to flush and pipeline. Skipping it entirely cost 5x more
    // than the compositor did. So we need SOME presents, just not many.
    //
    // A fixed divisor is wrong for release: the right N is framerate / refresh,
    // and both vary per user. Throttling by TIME auto-tunes to any monitor and
    // any framerate -- it can never exceed the compositor's rate, and it can
    // never starve the driver at low framerates the way a divisor does.
    //
    // MirrorPresentEvery: 0 = time-throttled (recommended), N = every Nth frame.
    bool doMirror;
    if (g_cfg.mirrorEvery > 0)
    {
        static unsigned mirrorTick = 0;
        const bool eyeOk = (!g_cfg.mirrorOneEye || eye == 0);
        if (eyeOk) ++mirrorTick;         // one eye only -- see the note below
        doMirror = eyeOk &&
            ((mirrorTick % (unsigned)g_cfg.mirrorEvery) == 0);
    }
    else
    {
        // 17 ms ~= just under 60 Hz, the lowest refresh worth designing for.
        //
        // THIS IS A CONSTANT AND NOT A SETTING, and the reason is worth having
        // here so it is not made tunable again. It WAS `MirrorIntervalMs`, and
        // the tester lowered it to get a faster desktop image; nothing changed.
        // The call below hands origPresent the GAME'S OWN SyncInterval, so the
        // desktop present is vsynced to the monitor -- asking for frames faster
        // than the refresh cannot produce them, whatever this number says. Only
        // raising it does anything, and "a slower mirror" is not a feature.
        //
        // What people actually want from a faster mirror is the HEADSET image on
        // the monitor, not a quicker copy of the game's flat frame. That is the
        // blit, and it needs a shader rather than a CopyResource: the eye target
        // is portrait (2750x2850) and the backbuffer is landscape, so dimensions
        // and aspect both differ. MirrorPresentEvery is the real control here.
        static const double kMirrorMs = 17.0;
        static LARGE_INTEGER lastMirror = {};
        LARGE_INTEGER nowM;
        QueryPerformanceCounter(&nowM);
        const double sinceMs = lastMirror.QuadPart
            ? (double)(nowM.QuadPart - lastMirror.QuadPart) * 1000.0 / (double)g_qpf.QuadPart
            : 1e9;
        // ONE EYE ON THE DESKTOP, for the same reason the menu quad takes one.
        // The backbuffer alternates between the left and right eye image, so a
        // mirror that presents whichever one happens to be current flickers
        // between two viewpoints -- fine in the headset, where each eye gets its
        // own, but it makes the monitor unwatchable and unrecordable. Reported
        // as "on the monitor it flickers a lot which means people cant record
        // it unless its in the headset".
        //
        // The eye test comes BEFORE the timer so the 17 ms clock only advances
        // on frames we actually present; testing it afterwards would let a
        // skipped frame eat the interval and halve the mirror rate.
        doMirror = (!g_cfg.mirrorOneEye || eye == 0) && (sinceMs >= kMirrorMs);
        if (doMirror) lastMirror = nowM;
    }

    HRESULT hr = S_OK;
    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    if (doMirror) hr = g_origPresent(sc, SyncInterval, Flags);
    QueryPerformanceCounter(&p1);
    g_msPresent += (double)(p1.QuadPart - p0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;

    g_lastPresentReturn = p1;
    return hr;
}

static bool GrabVTable(void** outPresent, void** outDrawIndexed, void** outDraw,
    void** outDrawIdxInst, void** outDrawInst, void** outOMSetRT)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BioVRDummy";
    if (!RegisterClassExW(&wc)) { Log("!!! RegisterClassExW failed (%lu)", GetLastError()); return false; }

    HWND hwnd = CreateWindowExW(0, L"BioVRDummy", L"", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Log("!!! CreateWindowExW failed (%lu)", GetLastError()); return false; }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL    fl = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &sc, &dev, &fl, &ctx);

    if (FAILED(hr) || !sc || !dev || !ctx)
    {
        Log("!!! D3D11CreateDeviceAndSwapChain FAILED hr=0x%08X", (unsigned)hr);
        DestroyWindow(hwnd);
        UnregisterClassW(L"BioVRDummy", wc.hInstance);
        return false;
    }

    void** scVT = *(void***)sc;
    void** ctxVT = *(void***)ctx;

    // The factory that made THIS throwaway swapchain also makes the game's, and
    // the vtable lives in dxgi.dll -- so hooking slot 10 patches CreateSwapChain
    // process-wide, exactly the way the Present hook works.
    // IDXGIFactory vtable: 0-2 IUnknown, 3-6 IDXGIObject, 7 EnumAdapters,
    // 8 MakeWindowAssociation, 9 GetWindowAssociation, 10 CreateSwapChain.
    // Read from the header, not guessed from a pattern.
    {
        IDXGIFactory* fac = nullptr;
        if (SUCCEEDED(sc->GetParent(__uuidof(IDXGIFactory), (void**)&fac)) && fac)
        {
            g_createSwapChainAddr = (*(void***)fac)[10];
            Log("vtable  CreateSwapChain = 0x%08X",
                (unsigned)(uintptr_t)g_createSwapChainAddr);

            IDXGIFactory2* f2 = nullptr;
            if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2)
            {
                g_createSCForHwndAddr = (*(void***)f2)[15];
                Log("vtable  CreateSCForHwnd = 0x%08X",
                    (unsigned)(uintptr_t)g_createSCForHwndAddr);
                f2->Release();
            }
            else Log("!!! no IDXGIFactory2 -- CreateSwapChainForHwnd unavailable.");

            fac->Release();
        }
        else Log("!!! GetParent(IDXGIFactory) failed -- ForceFlipModel unavailable.");
    }

    *outPresent = scVT[8];
    *outDrawIndexed = ctxVT[12];
    *outDraw = ctxVT[13];
    // ID3D11DeviceContext vtable: DrawIndexed 12, Draw 13, Map 14, Unmap 15,
    // PSSetConstantBuffers 16, IASetInputLayout 17, IASetVertexBuffers 18,
    // IASetIndexBuffer 19, DrawIndexedInstanced 20, DrawInstanced 21.
    // The draw calls are NOT contiguous -- 14/15 are Map/Unmap, and detouring
    // those with draw-shaped handlers crashes the moment a menu streams
    // textures. Verified against the header, not guessed from 12/13.
    // OMSetRenderTargets. Same discipline as the draw slots -- counted from the
    // header, not guessed. ID3D11DeviceContext: ...31 GSSetShaderResources,
    // 32 GSSetSamplers, 33 OMSetRenderTargets, 34 OMSetRenderTargetsAndUAVs,
    // 35 OMSetBlendState.
    *outOMSetRT = ctxVT[33];

    Log("vtable  Present     = 0x%08X", (unsigned)(uintptr_t)*outPresent);
    Log("vtable  DrawIndexed = 0x%08X", (unsigned)(uintptr_t)*outDrawIndexed);
    Log("vtable  Draw        = 0x%08X", (unsigned)(uintptr_t)*outDraw);
    Log("vtable  DrawIdxInst = 0x%08X", (unsigned)(uintptr_t)*outDrawIdxInst);
    Log("vtable  DrawInst    = 0x%08X", (unsigned)(uintptr_t)*outDrawInst);
    Log("vtable  OMSetRT     = 0x%08X", (unsigned)(uintptr_t)*outOMSetRT);

    ctx->Release();
    dev->Release();
    sc->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(L"BioVRDummy", wc.hInstance);
    return true;
}

bool Hooks_Install()
{
    void* pPresent = nullptr, * pDrawIndexed = nullptr, * pDraw = nullptr;
    void* pDrawIdxInst = nullptr, * pDrawInst = nullptr, * pOMSetRT = nullptr;
    if (!GrabVTable(&pPresent, &pDrawIndexed, &pDraw, &pDrawIdxInst, &pDrawInst,
        &pOMSetRT))
        return false;

    g_presentAddr = pPresent;
    g_drawIndexedAddr = pDrawIndexed;
    g_drawAddr = pDraw;
    g_drawIdxInstAddr = pDrawIdxInst;
    g_drawInstAddr = pDrawInst;
    g_omSetRTAddr = pOMSetRT;

    MH_STATUS s = MH_CreateHook(pPresent, &hkPresent, (LPVOID*)&g_origPresent);
    if (s != MH_OK) { Log("!!! MH_CreateHook(Present) -> %d", (int)s); return false; }

    s = MH_EnableHook(pPresent);
    if (s != MH_OK) { Log("!!! MH_EnableHook(Present) -> %d", (int)s); return false; }

    // Arm the conversion NOW: the description can only be rewritten as the
    // swapchain is CREATED. MEASURED margin -- the 18:08 log has our init at
    // :10.5 and the game's swapchain at :28.1, so ~17 seconds. Comfortable.
    if (g_cfg.forceFlip && g_createSwapChainAddr)
    {
        MH_STATUS f = MH_CreateHook(g_createSwapChainAddr, &hkCreateSwapChain,
            (LPVOID*)&g_origCreateSwapChain);
        if (f == MH_OK) f = MH_EnableHook(g_createSwapChainAddr);

        if (f == MH_OK) Log(">>> FLIP: CreateSwapChain hook ARMED (ForceFlipModel=1).");
        else Log("!!! FLIP: hook failed -> %d. Running with the stock swapchain.", (int)f);
    }

    // Arm the DXGI 1.2 entry point too. Slot 10 never fired, so this is the
    // likelier path -- but arm both, because whichever one the game uses, the
    // other costs nothing and the log now names it explicitly.
    if (g_createSCForHwndAddr)
    {
        MH_STATUS f2 = MH_CreateHook(g_createSCForHwndAddr, &hkCreateSCForHwnd,
            (LPVOID*)&g_origCreateSCForHwnd);
        if (f2 == MH_OK) f2 = MH_EnableHook(g_createSCForHwndAddr);

        if (f2 == MH_OK) Log(">>> FLIP: CreateSwapChainForHwnd hook ARMED.");
        else Log("!!! FLIP: ForHwnd hook failed -> %d.", (int)f2);
    }
    else if (g_cfg.forceFlip)
    {
        Log("!!! FLIP: no CreateSwapChain address. Running with the stock swapchain.");
    }

    Log(">>> Present hook ARMED. Waiting for the game to draw a frame...");
    return true;
}

void Hooks_Remove()
{
    Input_Remove();
    if (g_presentAddr) MH_DisableHook(g_presentAddr);
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}