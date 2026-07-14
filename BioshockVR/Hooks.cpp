// BioshockVR/Hooks.cpp
//
// Phase 2: hook IDXGISwapChain::Present via the dummy-swapchain vtable trick.
// We render nothing. We only prove the hook fires, and we report what the
// game's real backbuffer actually looks like -- which is the input Phase 3 needs.

#include "Hooks.h"
#include "XRSession.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

#include <MinHook.h>

#pragma comment(lib, "d3d11.lib")

extern void LogFile(const char* msg);
static void Log(const char* fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

// Rust's extern "system" on x86 == __stdcall == STDMETHODCALLTYPE.
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT SyncInterval, UINT Flags);

static PresentFn g_origPresent = nullptr;
static void* g_presentAddr = nullptr;

// Cached on first Present. Phase 3 needs these.
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static bool     g_described = false;
static uint64_t g_frames = 0;
static DWORD    g_lastTick = 0;

// Pull the game's real device/context/backbuffer info out of the live swapchain.
// Runs exactly once. This is the payload of Phase 2.
static void DescribeOnce(IDXGISwapChain* sc)
{
    g_described = true;

    DXGI_SWAP_CHAIN_DESC d = {};
    if (SUCCEEDED(sc->GetDesc(&d)))
    {
        Log("--- GAME SWAPCHAIN ---");
        Log("  backbuffer : %u x %u", d.BufferDesc.Width, d.BufferDesc.Height);
        Log("  format     : DXGI %d", (int)d.BufferDesc.Format);
        Log("  buffers    : %u", d.BufferCount);
        Log("  windowed   : %s", d.Windowed ? "YES" : "NO  <-- FIX THIS");
        Log("  samples    : %u (MSAA count)", d.SampleDesc.Count);
        Log("  hwnd       : 0x%08X", (unsigned)(uintptr_t)d.OutputWindow);
        Log("  usage      : 0x%08X", (unsigned)d.BufferUsage);
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
        Log("  feature lvl: 0x%04X", (unsigned)g_dev->GetFeatureLevel());
    }
    else
    {
        Log("!!! GetDevice failed -- this is NOT a D3D11 swapchain?");
    }

    // Can we actually get at the backbuffer texture? Phase 3 lives or dies on this.
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb)
    {
        D3D11_TEXTURE2D_DESC td = {};
        bb->GetDesc(&td);
        Log("  BACKBUFFER TEXTURE OK: %u x %u  fmt %d  usage %d  bind 0x%X  misc 0x%X",
            td.Width, td.Height, (int)td.Format, (int)td.Usage,
            (unsigned)td.BindFlags, (unsigned)td.MiscFlags);
        bb->Release();
    }
    else
    {
        Log("!!! GetBuffer(0) FAILED -- Phase 3 has a problem");
    }
    Log("----------------------");
}

// One-shot: try XR init exactly once, on the game's render thread, once we
// know the device. If it fails we set g_xrDead and never touch OpenXR again --
// retrying every frame would be a disaster.
static bool g_xrTried = false;
static bool g_xrDead = false;

static HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags)
{
    if (!g_described)
    {
        Log(">>> PRESENT HOOK FIRED. First frame.");
        DescribeOnce(sc);
        g_lastTick = GetTickCount();
    }

    ++g_frames;

    // --- bring OpenXR up, once, now that we have the game's device ---
    if (!g_xrTried && g_dev && g_ctx)
    {
        g_xrTried = true;
        Log(">>> Attempting XR_Init on the game's device (1920x1080)...");
        if (!XR_Init(g_dev, g_ctx, 1920, 1080))
        {
            g_xrDead = true;
            Log("!!! XR_Init FAILED. Running flat. Game is unaffected.");
        }
    }

    // --- submit the game's backbuffer to the headset ---
    if (!g_xrDead && XR_IsInit())
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb)
        {
            XR_Frame(bb);      // blocks in xrWaitFrame -- this is expected
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
        unsigned long long xrf = 0, xrs = 0;
        int st = 0;
        XR_Stats(&xrf, &xrs, &st);
        Log("frames: %llu (~%llu fps)   xr: %llu waited / %llu submitted   state %d",
            g_frames, g_frames - lastFrames, xrf, xrs, st);
        lastFrames = g_frames;
        g_lastTick = now;
    }

    return g_origPresent(sc, SyncInterval, Flags);
}

// Stand up a throwaway device + swapchain purely to read the vtable.
// All IDXGISwapChain instances in the process share it, so hooking the
// address we find here hooks the game's swapchain too.
static bool GrabVTable(void** outPresent, void** outDrawIndexed, void** outDraw)
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

    // vtable = *(void***)pInterface.  Indices are from the handoff (6f).
    void** scVT = *(void***)sc;
    void** ctxVT = *(void***)ctx;

    *outPresent = scVT[8];
    *outDrawIndexed = ctxVT[12];
    *outDraw = ctxVT[13];

    Log("vtable  Present     = 0x%08X", (unsigned)(uintptr_t)*outPresent);
    Log("vtable  DrawIndexed = 0x%08X   (not hooked yet -- Phase 9)", (unsigned)(uintptr_t)*outDrawIndexed);
    Log("vtable  Draw        = 0x%08X   (not hooked yet -- Phase 9)", (unsigned)(uintptr_t)*outDraw);

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
    if (!GrabVTable(&pPresent, &pDrawIndexed, &pDraw)) return false;

    g_presentAddr = pPresent;

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