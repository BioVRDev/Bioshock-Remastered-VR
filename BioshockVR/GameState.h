// BioshockVR/GameState.h
#pragma once

// THE GAME'S OWN UI STATE, read instead of inferred.
//
// Everything in DrawHook's menu detection exists because we were fingerprinting
// draw calls to guess a state the game already knows. From User.ini and
// ShockPlayerController we know that state is a named INPUT CONTEXT --
// "Default", "PauseUIActive", "RadialActive", "HackingUIActive" and ~25 others
// -- and that ShockPlayerController carries it in an FString field.
//
// HOW WE FIND IT, since this codebase has no UE2 reflection walk (the camera
// hook is a CODE PATTERN SCAN, it finds functions by locating code that
// mentions them -- it cannot resolve a property offset):
//
//   An FString here is a TArray<TCHAR>: { wchar_t* Data; int Count; int Max; }.
//   That shape is very hard to hit by accident -- readable pointer, sane count,
//   Max >= Count, null-terminated, every character printable. We scan the
//   controller object ONCE, list every field matching that shape, and lock onto
//   whichever one holds a name from the known context list.
//
// This is the same discipline that found +0x1E4: verified against a real value,
// never assumed. If nothing matches, we log every string field we did find and
// fall back to the old draw-signature path with nothing lost.

// A full-screen UI is up, read from Level.Pauser (the game's own pause test).
bool GameState_Paused();

// Call from hkCalcView with `pThis`. Cheap once locked (three reads).
void GameState_Observe(void* playerController);

// TRUE once an input-context field has been positively identified.
bool GameState_Valid();

// The raw context name, or "" before lock. Never null.
const char* GameState_Context();

// A full-screen UI is up -- pause, inventory, hacking, map, vending, and so on.
// This is the replacement for DrawHook_MenuUp()'s enumerated MenuIndexCounts,
// and it CANNOT false-positive on world geometry, which is what put an anchored
// square in front of you mid-fight.
bool GameState_MenuUp();

// The weapon/plasmid radial is open (LB/RB held). The world is still rendering,
// so this is NOT a menu -- but the sticks belong to the radial, which is what
// InputHook needs to know to pass right-stick Y through.
bool GameState_RadialOpen();

// Scripted-camera / input-suppressed sequence: NullInput and friends. The world
// renders; the player does not steer. This is the forced-camera signal the
// FORCEDCAM statistical probe was going to approximate.
bool GameState_ScriptedSequence();

void GameState_Reset();
bool GameState_Cutscene();
bool GameState_InGame();
void* GameState_Pawn();
