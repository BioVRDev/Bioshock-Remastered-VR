// BioshockVR/Game/EngineExec.h
#pragma once

// RUNNING A CONSOLE COMMAND THROUGH THE ENGINE.
//
// UGameEngine::Exec is the same entry point the tilde console uses, so anything
// you could type there we can issue from code -- including `set`, which writes
// the CLASS DEFAULT as well as live instances. That is why the reticle kill uses
// this instead of suppressing a draw signature: it survives pawn respawn, level
// change and save reload, none of which a draw-count suppression does.
//
// THE HONEST CAVEAT: unlike the camera hook, this does NOT find its target by
// pattern. It uses three absolute addresses, which are only valid for one build
// of the executable. They are therefore ini-overridable (EnginePtrRva,
// EngineVtableRva, EngineExecRva, EngineExecThis), and the engine's vtable is
// VERIFIED against the expected value before the call is made. On any other
// build that check fails, this logs and does nothing, and the mod carries on --
// which is the only acceptable failure mode for an address we did not derive.

// Returns true only if the engine reports it HANDLED the command.
bool EngineExec_Run(const char* command);

bool EngineExec_GetLastOutput(wchar_t* out, size_t chars);
