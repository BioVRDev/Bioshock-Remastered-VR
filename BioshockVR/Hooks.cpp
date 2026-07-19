// BioshockVR/Hooks.cpp
//
// Present hook, XR bring-up
//
// MEASURED: Present runs on the RENDER thread, CalcView on the GAME thread. So
// Present does NOT own the eye phase -- the camera hook tags each frame and
// Present pops the tag from a FIFO (CameraHook_NextEye).

#include "Hooks.h"
#include "XRSession.h"
#include "CameraHook.h"
#include "DrawHook.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

#include <MinHook.h>

#pragma comment(lib, "d3d11.lib")

extern void  LogFile(const char* msg);
extern float g_cfgFovDeg;
extern bool  g_cfgCameraHook;
extern bool  g_cfgDisableVSync;
extern bool  g_cfgMenuScreen;

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

static void* g_drawIndexedAddr = nullptr;
static void* g_drawAddr = nullptr;
static void* g_drawIdxInstAddr = nullptr;
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
        Log("  buffers    : %u", d.BufferCount);
        Log("  windowed   : %s", d.Windowed ? "YES" : "NO  <-- FIX THIS");
        Log("  samples    : %u (MSAA count)", d.SampleDesc.Count);
        Log("  hwnd       : 0x%08X", (unsigned)(uintptr_t)d.OutputWindow);
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
            XR_SetGameFov(g_cfgFovDeg, g_bbW, g_bbH);
        }

        // Camera hook on the render thread at the first frame -- NOT at DllMain,
        // where the exe may still be packed.
        if (!g_camTried)
        {
            g_camTried = true;
            if (g_cfgCameraHook)
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
                g_drawIdxInstAddr, g_drawInstAddr);
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
            if (g_cfgMenuScreen && (CameraHook_Starved() || DrawHook_MenuUp()))
            {
                if (!menuMode)
                {
                    menuMode = true;
                    Log(">>> MENU SCREEN ON (%s)",
                        CameraHook_Starved() ? "camera starved" : "draw signature");
                }

                XR_SubmitMenuMono(bb);
            }
            else
            {
                if (menuMode) { menuMode = false; Log(">>> MENU SCREEN off (camera ticking)"); }
                XR_SubmitPair(bb, eye);
            }

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

    if (g_cfgDisableVSync)
    {
        if (SyncInterval != 0 && !g_loggedVSyncOverride)
        {
            g_loggedVSyncOverride = true;
            Log(">>> VSYNC OVERRIDE: game asked SyncInterval=%u, presenting with 0.", SyncInterval);
        }
        SyncInterval = 0;
    }

    DrawHook_EndFrame();

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr = g_origPresent(sc, SyncInterval, Flags);
    QueryPerformanceCounter(&p1);
    g_msPresent += (double)(p1.QuadPart - p0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;

    g_lastPresentReturn = p1;
    return hr;
}

static bool GrabVTable(void** outPresent, void** outDrawIndexed, void** outDraw,
    void** outDrawIdxInst, void** outDrawInst)
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

    *outPresent = scVT[8];
    *outDrawIndexed = ctxVT[12];
    *outDraw = ctxVT[13];
    // ID3D11DeviceContext vtable: DrawIndexed 12, Draw 13, Map 14, Unmap 15,
    // PSSetConstantBuffers 16, IASetInputLayout 17, IASetVertexBuffers 18,
    // IASetIndexBuffer 19, DrawIndexedInstanced 20, DrawInstanced 21.
    // The draw calls are NOT contiguous -- 14/15 are Map/Unmap, and detouring
    // those with draw-shaped handlers crashes the moment a menu streams
    // textures. Verified against the header, not guessed from 12/13.
    *outDrawIdxInst = ctxVT[20];
    *outDrawInst = ctxVT[21];

    Log("vtable  Present     = 0x%08X", (unsigned)(uintptr_t)*outPresent);
    Log("vtable  DrawIndexed = 0x%08X", (unsigned)(uintptr_t)*outDrawIndexed);
    Log("vtable  Draw        = 0x%08X", (unsigned)(uintptr_t)*outDraw);
    Log("vtable  DrawIdxInst = 0x%08X", (unsigned)(uintptr_t)*outDrawIdxInst);
    Log("vtable  DrawInst    = 0x%08X", (unsigned)(uintptr_t)*outDrawInst);

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
    void* pDrawIdxInst = nullptr, * pDrawInst = nullptr;
    if (!GrabVTable(&pPresent, &pDrawIndexed, &pDraw, &pDrawIdxInst, &pDrawInst))
        return false;

    g_presentAddr = pPresent;
    g_drawIndexedAddr = pDrawIndexed;
    g_drawAddr = pDraw;
    g_drawIdxInstAddr = pDrawIdxInst;
    g_drawInstAddr = pDrawInst;

    MH_STATUS s = MH_CreateHook(pPresent, &hkPresent, (LPVOID*)&g_origPresent);
    if (s != MH_OK) { Log("!!! MH_CreateHook(Present) -> %d", (int)s); return false; }

    s = MH_EnableHook(pPresent);
    if (s != MH_OK) { Log("!!! MH_EnableHook(Present) -> %d", (int)s); return false; }

    Log(">>> Present hook ARMED. Waiting for the game to draw a frame...");
    return true;
}

void Hooks_Remove()
{
    if (g_presentAddr) MH_DisableHook(g_presentAddr);
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}