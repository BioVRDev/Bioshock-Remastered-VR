// BioshockVR/Game/GameState.h
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
//
// ============================================================================
//  ⚠ MEASURED: THE SCAN HAS NEVER LOCKED. READ THIS BEFORE BUILDING ON IT.
// ============================================================================
// The design above is sound and the search works -- it brackets the right
// window (pawn+0x728..+0xA7C, anchored on FirstPersonHands.PlayerHands) and
// matches a wall of known localized constants. It then never finds a context
// name. There is not one ">>> CONTEXT" line in any session on record.
//
// So GameState_MenuUp(), GameState_RadialOpen(), GameState_ScriptedSequence()
// and GameState_Cutscene() are effectively INERT. They compile, they are
// correct, and they always return false. Everything downstream of a cutscene
// signal -- the HUD gate, the cutscene anchor, comfort settings -- is built and
// waiting for a value that never arrives.
//
// The likely reason: LastPlayerInputContext is declared `var private travel
// string` and NOTHING in the decompiled script writes it, so it is presumably
// native and may be inert on this build. See research/uscript/.
//
// GameState_Paused() is the exception and is genuinely reliable -- it reads
// Level.Pauser, the game's own pause test, not a scanned field.
//
// Do not add a heuristic here to paper over this. docs/INVARIANTS.md lists six
// that were tried and falsified. docs/modules/gamestate.md holds the two live
// leads.
// ============================================================================

// A full-screen UI is up, read from Level.Pauser (the game's own pause test).
// THE ONE RELIABLE SIGNAL IN THIS FILE.
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

// A scripted hand-animation sequence is running -- the game is deliberately
// animating the player's hands as part of a scripted moment.
//
// MEASURED (M7-S1): hands+0x594 bit 2, exact on both edges, and it fired ONCE
// in six minutes of mixed play. This is the first real scripted-event signal
// this project has ever had.
//
// IT DOES NOT COVER the Little Sister rescue or the EVE injection -- both are
// Hands *states*, a different mechanism, and both were measured leaving this
// bit clear. Do not treat it as "in a cutscene" generally.
bool GameState_ScriptedAnim();

// The player is riding a bathysphere. MEASURED-BY-ORACLE: pawn+0x464 bit 1 is
// Pawn.bCannotFall, which ActionEnableBathysphereModeForPlayer sets for the
// whole ride while clearing bit 2 in the same call.
//
// It exists so the gameplay rotation freeze can leave the ride alone -- a
// bathysphere is not a scripted animation, so without this it would be frozen
// along with ordinary play and the camera would stop following the sphere.
bool GameState_Bathysphere();

// The game is interpolating the player into position and heading --
// StartForcePlayerMove, which runs BEFORE a scripted animation begins and for
// the whole of a bathysphere boarding. MEASURED at controller+0x9E0.
//
// Anything that writes Controller.Rotation must stand down while this is true,
// or it fights the interpolation: that was the reported entry stall, where the
// controller dragged the view for 1-6 seconds and the move timed out short of
// its target.
//
// NOT an animation. Arms and hands must NOT follow this -- they use the motion
// signal, which is a different question.
bool GameState_ForcedMove();

// TRUE for a scripted animation the player is still meant to walk through -- the
// Big Daddy killing a splicer, where the meters stay up and you keep control.
//
// DECIDED ONCE, inside the first second of the animation, and frozen until it
// ends. It has to be: the per-frame version of this question could flip in the
// middle of a sequence, which resumed the aim write while the game was still
// moving the player and threw the balcony landing 3.7 m off. See the banner in
// GameState.cpp.
//
// FALSE while undecided, so callers fail closed. Consumers still gate on
// g_cfg.controllableScripted -- this reports the fact, not the policy.
bool GameState_ScriptedInControl();

void GameState_Reset();
bool GameState_Cutscene();
bool GameState_InGame();
void* GameState_Pawn();
