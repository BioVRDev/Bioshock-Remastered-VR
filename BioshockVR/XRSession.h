#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// Bring up an OpenXR session on an existing D3D11 device.
bool XR_Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, unsigned w, unsigned h);
bool XR_IsInit();

// Call once per game frame. Pumps events and submits 'image' to both eyes.
void XR_Frame(ID3D11Texture2D* image);

// For the heartbeat log. Any pointer may be null.
void XR_Stats(unsigned long long* frames, unsigned long long* submitted, int* state);

void XR_Shutdown();