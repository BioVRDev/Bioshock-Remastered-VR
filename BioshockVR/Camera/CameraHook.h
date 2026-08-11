// BioshockVR/Camera/CameraHook.h
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

// TRUE when no camera view has been produced for >250ms (menu/loading/movie).
bool CameraHook_Starved();

// World units the camera moved between the two eye renders. Large means the
// stashed left image no longer fuses with the live right one.
double CameraHook_InterEyeMove();

// ---- THE LATCHED-POSE CHANNEL (flicker fix, §2) -------------------------
// game->render, mirror of the render->game head seqlock. Publishes the pose
// the camera was ACTUALLY rendered from: the quat latched at eye-0 time, and
// the APPLIED head-center position (origin + CLAMPED offset, metres, XR
// LOCAL). Returns false when the camera is not head-driven (tracking off,
// write off, hook not yet armed) -- caller falls back to the fresh pose.
bool CameraHook_GetLatchedPose(float quat[4], float pos[3]);

// ---- MOTION AIM (S41) ---------------------------------------------------
// Angular offset of the (clamped, smoothed) controller aim from the head, in
// degrees. The crosshair quad lives in VIEW space, so this offset is all
// XRSession needs to move it off head-centre and onto where the gun points.
// Returns false when motion aim is off or the controller is untracked --
// caller should draw the crosshair straight ahead.
bool CameraHook_GetAimOffset(float* dYawDeg, float* dPitchDeg);

// Re-applies the hands rotator from the RENDER thread, after the game tick has
// had its say. Call once per Present.
void CameraHook_LateHandsWrite();

bool CameraHook_GetPitchError(float* outDeg);

bool CameraHook_GetHeadYawOffset(float* outDeg);

// True only while the mod is actually writing Controller.Rotation -- i.e. while
// the aim field carries our composed heading rather than the game's.
//
// FALSE during a scripted release (M7-S3 suppresses the write on purpose), with
// head aim off, while the UI is up, and when the camera hook is starved.
bool CameraHook_OwnsAimField();

// How far to rotate the MOVEMENT STICK, in degrees, to make walking go where the
// current MovementMode says it should. Computed at the aim write site from the
// values actually written, so it cancels the composition exactly at any pitch --
// deriving it from the head and controller yaws instead left a residual coupling
// that grew with pitch and inverted with direction. See the banner in
// CameraHook.cpp. False until the first aim write.
bool CameraHook_GetWalkRotation(float* outDeg);

// Direction the shot ACTUALLY goes, in XR head-local axes. False until the
// head-aim write has run at least once.
bool CameraHook_GetShotDir(float out[3]);