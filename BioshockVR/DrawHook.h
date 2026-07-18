// BioshockVR/DrawHook.h
#pragma once

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
bool DrawHook_Install(void* pDrawIndexed, void* pDraw);
void DrawHook_Remove();

// Call ONCE per Present, before the real Present. Rolls the per-frame table.
void DrawHook_EndFrame();

// TRUE while a menu/tutorial overlay is on screen, detected by draw signature.
// Works for pause/main/tutorial menus, which do NOT starve the camera hook.
bool DrawHook_MenuUp();
