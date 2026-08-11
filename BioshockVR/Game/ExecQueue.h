// BioshockVR/Game/ExecQueue.h
#pragma once

// ONE OWNER FOR "TELL THE ENGINE TO DO SOMETHING".
//
// EngineExec_Run() is the mechanism; this is the discipline around it. Three
// planned features need ini-driven one-shot commands (the wrench experiments,
// left-handed mode, holsters), and holsters additionally needs to issue a
// command from a DETECTION MADE ON THE XR THREAD.
//
// THE THREAD RULE, WHICH IS THE WHOLE REASON THIS FILE EXISTS:
//
//   EngineExec_Run() walks an engine pointer, checks a vtable and calls through
//   it. That is a GAME THREAD operation. Calling it from the XR thread reads
//   engine state while the game thread is free to be rewriting it -- the kind of
//   bug that works for weeks and then corrupts something during a level load.
//
//   ExecQueue_Post() is therefore the ONLY entry point off the game thread. It
//   copies the string into a small ring and returns; nothing is executed until
//   ExecQueue_Tick() drains it from GameState_Observe(). Stating this once, in
//   one file, is the point -- it was previously an unwritten rule that happened
//   to hold because only one caller existed.

// Drains at most one command per call. GAME THREAD ONLY -- called from
// GameState_Observe(), beside Reticle_Tick().
//
// ONE PER FRAME, NOT THE WHOLE RING. `set` on a class default is not free, and a
// burst issued at level load would land in the same frame the engine is still
// building its object table. Spreading them costs a few frames nobody can feel.
void ExecQueue_Tick();

// Queues a command. Safe from ANY thread. Returns false if the ring is full or
// the string does not fit, and NEVER blocks -- a dropped haptic-triggered
// holster command is a missed draw, while a stalled XR thread is a dropped
// frame in the headset.
bool ExecQueue_Post(const char* command);

// Fires the ini's ExecCommand1..8 once, in order, the first time the engine is
// reachable. Safe to call every frame; it self-latches.
void ExecQueue_RunStartupCommands();
