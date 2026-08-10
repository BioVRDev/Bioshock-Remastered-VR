// BioshockVR/Render/Hooks.h
#pragma once

// Creates a throwaway D3D11 device+swapchain, reads the shared vtable,
// and MinHooks IDXGISwapChain::Present. Call from the init thread, never
// from DllMain.
bool Hooks_Install();
void Hooks_Remove();