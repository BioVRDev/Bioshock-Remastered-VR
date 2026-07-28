// BioshockVR/DrawHook.h
#pragma once

struct ID3D11Texture2D;

// The swapchain backbuffer, handed over once per Present. We only ever compare
// the POINTER -- this is resource identity, not a reference we keep.
void DrawHook_SetBackbuffer(ID3D11Texture2D* bb);

// The captured interface, on transparent black, or null before the first
// successful capture. Same dimensions as the game's composite target.
ID3D11Texture2D* DrawHook_HudTexture();

// TRUE if at least one interface draw was redirected during the last frame.
// XRSession uses this to decide whether to submit the HUD quad at all.
bool DrawHook_HudCaptured();

// TRUE while the game is NOT rendering an in-engine world -- a prerendered
// movie, a menu, a loading screen. Measured discrimination against gameplay is
// total: 0 of 4820 intro frames identified a scene target, 44890 of 44890
// gameplay frames did. Latched with dwell time, so it is safe to switch
// presentation mode on.
bool DrawHook_NoWorldRender();

// Hooks ID3D11DeviceContext::DrawIndexed and ::Draw so we can IDENTIFY and then
// SUPPRESS individual draw calls -- the HUD, the reticle, the menus, coronas.
//
// PHASE 1 (this file, now): FINGERPRINT ONLY. Every draw is counted by its
// IndexCount/VertexCount. Numpad 3 dumps the table. Nothing is suppressed until
// you put counts in the ini, so installing this cannot change what you see.
//
// PHASE 2 (after we read a dump): suppression by count, then depth placement.
//
// Install from the RENDER THREAD on the first Present, with the vtable slots
// Hooks.cpp already reads (ctxVT[12] == DrawIndexed, ctxVT[13] == Draw).
// S24: the instanced entry points are hooked too. Six sessions of fingerprinting
// eliminated every persistent Draw/DrawIndexed count as the cursor -- something
// on screen every frame that lands in no bucket is being issued through a
// function we were not watching.
// ctxVT[20] == DrawIndexedInstanced,
// ctxVT[21] == DrawInstanced -- 14/15 are Map/Unmap, do NOT hook those.
// The last two may be null; that is not fatal.
bool DrawHook_Install(void* pDrawIndexed, void* pDraw,
    void* pDrawIndexedInstanced, void* pDrawInstanced,
    void* pOMSetRenderTargets);
void DrawHook_Remove();

// Call ONCE per Present, before the real Present. Rolls the per-frame table.
void DrawHook_EndFrame();

// TRUE while a menu/tutorial overlay is on screen, detected by draw signature.
// Works for pause/main/tutorial menus, which do NOT starve the camera hook.
bool DrawHook_MenuUp();

// TRUE while an IN-GAME UI that should sit on the world-locked quad is up --
// the tonic/plasmid slot screen, hacking, vending, the map. These do NOT pause,
// so GameState_Paused() cannot see them, and MenuIndexCounts cannot either:
// Hooks.cpp only lets a draw signature reach the quad when GameState_InGame()
// is FALSE. Separate list, separate consumer -- so a wrong entry here can never
// freeze the camera the way a wrong MenuIndexCounts entry did.
bool DrawHook_AnchorUp();