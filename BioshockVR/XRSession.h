#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

bool XR_Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, unsigned w, unsigned h);
bool XR_IsInit();
void XR_SetGameFov(float horizFovDeg, unsigned w, unsigned h);

// --- THE THREE CADENCES (selected by XRMode in the ini) ---

// XRMode=1. PHASE 5, KNOWN GOOD. Full XR cycle every Present, same image to both
// eyes. Mono. 118 submits/sec, headset at 120. The floor we can always fall back to.
void XR_SubmitMono(ID3D11Texture2D* image);

// XRMode=2. What §4 prescribed: submit every SECOND Present. This HALVES the
// submit rate to 59/sec and drops the headset to 60. Kept only to reproduce the
// regression on demand.
void XR_SubmitEye(ID3D11Texture2D* image, int eye);

// XRMode=3. THE FIX. Full XR cycle EVERY Present, so the submit rate stays at
// 118/sec exactly like Phase 5 -- but each eye carries its OWN image. The camera
// alternates eyes per Present; the stale eye re-shows its image from one Present
// ago. That one-frame stagger IS AER. Stereo did NOT require starving the
// compositor; §4 conflated "alternate the camera" with "submit half as often".
void XR_SubmitAER(ID3D11Texture2D* image, int eye);

// XRMode=4. AER with a STABLE eye pairing. XRMode=3 alternates which eye is
// stale, so the inter-eye time offset flips sign 118x/sec -- during a turn that
// reads as the world shimmering in and out of depth. Here the pair is always
// (L from Present N, R from Present N+1) and is held for both frames of the
// cycle, so R is ALWAYS 8.4ms newer than L. Constant sign, no shimmer.
void XR_SubmitAERStable(ID3D11Texture2D* image, int eye);

void XR_Stats(unsigned long long* frames, unsigned long long* submitted, int* state);

// Milliseconds inside each individual OpenXR call, accumulated since start.
// xrWaitFrame already measured 0.00 -- it is NOT the limiter. Something costs
// ~4ms/Present and pins the game at 118. This names it.
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