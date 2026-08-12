// BioshockVR/Hud/DrawHook.h
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
// The fingerprint table is still here and still useful, but it is now a
// DIAGNOSTIC rather than the mechanism: Numpad * clears it, Numpad 3 dumps it,
// Numpad - steps the isolate walker. Suppression, HUD capture and alpha repair
// are all live, driven by the ini lists and by structural render-target
// classification rather than by counts alone.
//
// Count-based classification turned out to be the wrong long-term architecture
// -- it is fragile across scenes and cannot see a draw's role -- but the DUMP
// remains the fastest way to name an unknown draw. It is what identified the
// duplicate-world square: normal play captured 5d tex=no, the square 6d
// tex=yes, and that one difference was the whole fix.
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

// TRUE while a screen is being shown as the game's OWN COMPOSED FRAME on the
// menu quad -- both the anchored route and the head-following one do that, and
// so does the (empty) draw-count list.
//
// ONE OWNER for that question, deliberately. Hooks.cpp routes the quad by it and
// DrawHook suppresses the HUD capture by it, so the two can never disagree about
// which screens are on this route -- and disagreeing is exactly what left the
// map's contents in the capture instead of in the picture.
bool DrawHook_ComposedFrameUp();

bool DrawHook_CutsceneBarsActive();

// Forward declaration rather than #include <d3d11.h>: this header is included
// in files that do not otherwise pull in D3D, and a pointer to an incomplete
// type is all a declaration needs.
struct ID3D11DeviceContext;

ID3D11Texture2D* DrawHook_HudTextureForSubmit(ID3D11DeviceContext* ctx);