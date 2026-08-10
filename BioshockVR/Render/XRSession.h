// BioshockVR/Render/XRSession.h
#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

bool XR_Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, unsigned w, unsigned h);
bool XR_IsInit();
void XR_SetGameFov(float horizFovDeg, unsigned w, unsigned h);

// THE submit path. Called once per Present with that frame's eye tag, popped
// from the camera hook's FIFO (the camera and Present live on DIFFERENT threads
// -- the tag travels with the frame, it is not a shared flag).
//
//   eye 0 : stash this backbuffer. No XR cycle.
//   eye 1 : run ONE full XR cycle, submitting (stashed left, this right).
//
// So: ~236 Present/s in, ~118 XR submits/s out, 118 unique frames per eye,
// one Present (~4.2ms) of inter-eye disparity. That disparity IS the stereo.
void XR_SubmitPair(ID3D11Texture2D* image, int eye);

// Menu/loading path: ONE mono frame per Present, shown on a head-locked quad
// ("virtual screen") instead of the projection layer. Called when the camera
// hook is starved (CameraHook_Starved), i.e. the game isn't rendering a world.
void XR_SubmitMenuMono(ID3D11Texture2D* image);

// Drop the world-locked screen's anchor so the next menu/theater frame takes a
// fresh one. Called from Present, which is the thread that owns the anchor.
void XR_ResetMenuAnchor();

// Latest HMD head orientation (OpenXR LOCAL-space quaternion x,y,z,w), published
// from the render thread for the game-thread camera write. Seqlock-safe.
void XR_GetHeadQuat(float out[4]);

// Latest HMD head-center position (metres, OpenXR LOCAL space). Same seqlock
// as the quaternion above.
void XR_GetHeadPos(float out[3]);

void XR_Stats(unsigned long long* frames, unsigned long long* submitted, int* state);

// Milliseconds inside each individual OpenXR call, accumulated since start.
// THIS IS NOT DEBUG CRUFT. It is what solved Phase 7. It stays.
struct XrTimeBreakdown
{
    unsigned long long submits;
    double waitFrame;
    double beginFrame;
    double locateViews;
    double acquire;     // xrAcquireSwapchainImage + xrWaitSwapchainImage
    double copy;        // CopyResource
    double endFrame;
};
void XR_Breakdown(XrTimeBreakdown* out);

void XR_Shutdown();