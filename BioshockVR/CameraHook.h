// BioshockVR/CameraHook.h
#pragma once

// Locates APlayerController::eventPlayerCalcView via the FName chain (no
// hardcoded offsets) and hooks it. Call from the RENDER THREAD on the first
// Present -- never from DllMain, and never before the exe has unpacked.
bool CameraHook_Install();
void CameraHook_Remove();

// ---- THE EYE TAG FIFO (Phase 6) ----------------------------------------
// MEASURED: CalcView runs on the GAME thread, Present runs on the RENDER
// thread. They are different threads with a pipeline between them, so Present
// CANNOT simply flip a flag and expect the next CalcView to see it.
//
// Instead the tag travels WITH the frame. The camera hook (producer) tags every
// site0 view it computes with the eye it applied, and pushes it. Present
// (consumer) pops in order. Whatever the pipeline depth is, the tag pops out
// alongside the frame it belongs to, and a one-frame slip can never accumulate.
//
// Call ONCE per Present. Returns 0 (LEFT) or 1 (RIGHT).
int  CameraHook_NextEye();

// Queue depth observed since the last call, then resets. If min == max the two
// threads are in lockstep at a fixed pipeline depth. Underruns = Presents that
// had no camera tag waiting (menus, movies) -- expected before a level loads.
void CameraHook_EyeQueueStats(int* minDepth, int* maxDepth, unsigned* underruns);

// ---- THE LATCHED-POSE CHANNEL (flicker fix, §2) -------------------------
// game->render, mirror of the render->game head seqlock. Publishes the pose
// the camera was ACTUALLY rendered from: the quat latched at eye-0 time, and
// the APPLIED head-center position (origin + CLAMPED offset, metres, XR
// LOCAL). Returns false when the camera is not head-driven (tracking off,
// write off, hook not yet armed) -- caller falls back to the fresh pose.
bool CameraHook_GetLatchedPose(float quat[4], float pos[3]);