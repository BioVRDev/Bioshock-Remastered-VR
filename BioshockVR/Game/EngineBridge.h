// BioshockVR/Game/EngineBridge.h
#pragma once

// TIER 1 OF docs/ARCHITECTURE.md -- the native call bridge.
//
// M3-S1 IS LOCATE ONLY. Nothing here calls into the engine. The scan is
// read-only, one-shot, and gates no behaviour; its entire product is a log
// line. M3-S2 is the session that dares to call, and it must re-verify the
// prologue bytes this session recorded before it does.
//
// THREADING: EngineBridge_Tick() runs on the GAME thread, from
// GameState_Observe, beside MyHudTick/CineTick. The accessors below are plain
// reads of a pointer that is written once and never cleared -- the engine's
// native table is static for the life of the process, so unlike the pawn and
// controller pointers this has no lifetime boundary to reset at.

// One-shot locate, with backoff. Cheap after it settles.
void EngineBridge_Tick();

// The located native, or nullptr if the scan has not run or did not lock.
// M3-S1 ships no caller for this -- it exists so S2 has somewhere to start.
void* EngineBridge_GetPropertyTextByName();
void* EngineBridge_GetPropertyText();
