// BioshockVR/CameraHook.h
#pragma once

// Locates APlayerController::eventPlayerCalcView via the FName chain (no
// hardcoded offsets) and hooks it. Call from the RENDER THREAD on the first
// Present -- never from DllMain, and never before the exe has unpacked.
bool CameraHook_Install();
void CameraHook_Remove();