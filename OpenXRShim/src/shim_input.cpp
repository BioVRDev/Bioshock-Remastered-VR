// ============================================================================
//  shim_input.cpp -- OpenXR actions -> SteamVR input (IVRInput).
//
//  The mod creates one action set ("gamepad") with named actions and suggests
//  bindings for oculus/touch_controller. SteamVR's input system wants a JSON
//  action manifest plus per-controller binding files instead, so at attach
//  time we generate them next to the DLL and hand them to SetActionManifestPath.
//  Bindings are authored for Index controllers (knuckles) first, with wand and
//  Touch fallbacks; the user can rebind anything in SteamVR's controller UI.
//
//  Index mapping choices worth knowing about:
//    * the mod's "right thumbrest touch" (its d-pad modifier) -> right
//      trackpad TOUCH, which is where an Index thumb naturally rests
//    * the mod's "menu" (Touch's left menu button) -> left trackpad CLICK
//      (a firm press on the Index trackpad)
//    * aim pose -> SteamVR "tip" component, grip pose -> "handgrip"
// ============================================================================
#include "shim.h"
#include <cstring>
#include <cstdio>
#include <direct.h>
#include <vector>

#define SHIM_EXPORT extern "C" __declspec(dllexport)

extern const char* PathToString(XrPath p);

static ActionSetRec* g_theSet = nullptr;
static std::vector<ActionRec*> g_setActions;
static bool g_inputReady = false;

// ---------------------------------------------------------------- creation
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(
    XrInstance, const XrActionSetCreateInfo* info, XrActionSet* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionSetRec* s = new ActionSetRec();
    strncpy(s->name, info->actionSetName, sizeof(s->name) - 1);
    g_theSet = s;
    *out = (XrActionSet)s;
    SLOG("xrCreateActionSet '%s'", s->name);
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(
    XrActionSet set, const XrActionCreateInfo* info, XrAction* out)
{
    if (!set || !info || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = new ActionRec();
    a->set = (ActionSetRec*)set;
    strncpy(a->name, info->actionName, sizeof(a->name) - 1);
    a->type = info->actionType;
    g_setActions.push_back(a);
    *out = (XrAction)a;
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance, const XrInteractionProfileSuggestedBinding* sb)
{
    if (!sb) return XR_ERROR_VALIDATION_FAILURE;
    SLOG("xrSuggestInteractionProfileBindings profile='%s' count=%u (noted; shim "
         "authors its own SteamVR bindings)", PathToString(sb->interactionProfile),
         sb->countSuggestedBindings);
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(
    XrSession, const XrActionSpaceCreateInfo* info, XrSpace* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    SpaceRec* s = new SpaceRec();
    s->kind = SPACE_ACTION;
    s->action = (ActionRec*)info->action;
    *out = (XrSpace)s;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- manifest
static const char* kBindingsKnuckles = R"JSON({
  "bindings": {
    "/actions/gamepad": {
      "sources": [
        { "path": "/user/hand/left/input/thumbstick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gamepad/in/move" },
                      "click":    { "output": "/actions/gamepad/in/thumb_l" } } },
        { "path": "/user/hand/right/input/thumbstick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gamepad/in/turn" },
                      "click":    { "output": "/actions/gamepad/in/thumb_r" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/trigger_l" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/trigger_r" } } },
        { "path": "/user/hand/left/input/grip", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/grip_l" } } },
        { "path": "/user/hand/right/input/grip", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/grip_r" } } },
        { "path": "/user/hand/right/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_a" } } },
        { "path": "/user/hand/right/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_b" } } },
        { "path": "/user/hand/left/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_x" } } },
        { "path": "/user/hand/left/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_y" } } },
        { "path": "/user/hand/left/input/trackpad", "mode": "trackpad",
          "inputs": { "click": { "output": "/actions/gamepad/in/menu" },
                      "touch": { "output": "/actions/gamepad/in/rest_l" } } },
        { "path": "/user/hand/right/input/trackpad", "mode": "trackpad",
          "inputs": { "touch": { "output": "/actions/gamepad/in/rest_r" } } }
      ],
      "poses": [
        { "output": "/actions/gamepad/in/aim_l",   "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/gamepad/in/aim_r",   "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/gamepad/in/gpose_l", "path": "/user/hand/left/pose/handgrip" },
        { "output": "/actions/gamepad/in/gpose_r", "path": "/user/hand/right/pose/handgrip" }
      ],
      "haptics": [
        { "output": "/actions/gamepad/out/haptic_l", "path": "/user/hand/left/output/haptic" },
        { "output": "/actions/gamepad/out/haptic_r", "path": "/user/hand/right/output/haptic" }
      ]
    }
  },
  "controller_type": "knuckles",
  "description": "BioshockVR shim default bindings for Index controllers",
  "name": "BioshockVR (shim) Index bindings"
}
)JSON";

static const char* kBindingsVive = R"JSON({
  "bindings": {
    "/actions/gamepad": {
      "sources": [
        { "path": "/user/hand/left/input/trackpad", "mode": "trackpad",
          "inputs": { "position": { "output": "/actions/gamepad/in/move" },
                      "click":    { "output": "/actions/gamepad/in/thumb_l" } } },
        { "path": "/user/hand/right/input/trackpad", "mode": "trackpad",
          "inputs": { "position": { "output": "/actions/gamepad/in/turn" },
                      "click":    { "output": "/actions/gamepad/in/thumb_r" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/trigger_l" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/trigger_r" } } },
        { "path": "/user/hand/left/input/application_menu", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/menu" } } },
        { "path": "/user/hand/right/input/application_menu", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_a" } } }
      ],
      "poses": [
        { "output": "/actions/gamepad/in/aim_l",   "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/gamepad/in/aim_r",   "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/gamepad/in/gpose_l", "path": "/user/hand/left/pose/handgrip" },
        { "output": "/actions/gamepad/in/gpose_r", "path": "/user/hand/right/pose/handgrip" }
      ],
      "haptics": [
        { "output": "/actions/gamepad/out/haptic_l", "path": "/user/hand/left/output/haptic" },
        { "output": "/actions/gamepad/out/haptic_r", "path": "/user/hand/right/output/haptic" }
      ]
    }
  },
  "controller_type": "vive_controller",
  "description": "BioshockVR shim minimal bindings for Vive wands",
  "name": "BioshockVR (shim) Vive wand bindings"
}
)JSON";

static const char* kBindingsTouch = R"JSON({
  "bindings": {
    "/actions/gamepad": {
      "sources": [
        { "path": "/user/hand/left/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gamepad/in/move" },
                      "click":    { "output": "/actions/gamepad/in/thumb_l" } } },
        { "path": "/user/hand/right/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gamepad/in/turn" },
                      "click":    { "output": "/actions/gamepad/in/thumb_r" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/trigger_l" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/trigger_r" } } },
        { "path": "/user/hand/left/input/grip", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/grip_l" } } },
        { "path": "/user/hand/right/input/grip", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/grip_r" } } },
        { "path": "/user/hand/right/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_a" } } },
        { "path": "/user/hand/right/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_b" } } },
        { "path": "/user/hand/left/input/x", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_x" } } },
        { "path": "/user/hand/left/input/y", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/btn_y" } } },
        { "path": "/user/hand/left/input/application_menu", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/menu" } } },
        { "path": "/user/hand/right/input/thumbrest", "mode": "button",
          "inputs": { "touch": { "output": "/actions/gamepad/in/rest_r" } } },
        { "path": "/user/hand/left/input/thumbrest", "mode": "button",
          "inputs": { "touch": { "output": "/actions/gamepad/in/rest_l" } } }
      ],
      "poses": [
        { "output": "/actions/gamepad/in/aim_l",   "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/gamepad/in/aim_r",   "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/gamepad/in/gpose_l", "path": "/user/hand/left/pose/handgrip" },
        { "output": "/actions/gamepad/in/gpose_r", "path": "/user/hand/right/pose/handgrip" }
      ],
      "haptics": [
        { "output": "/actions/gamepad/out/haptic_l", "path": "/user/hand/left/output/haptic" },
        { "output": "/actions/gamepad/out/haptic_r", "path": "/user/hand/right/output/haptic" }
      ]
    }
  },
  "controller_type": "oculus_touch",
  "description": "BioshockVR shim bindings for Touch controllers",
  "name": "BioshockVR (shim) Touch bindings"
}
)JSON";

// Windows Mixed Reality / Reverb G2. No face buttons at all, so A/B/X/Y come
// off trackpad quadrants. UNTESTED -- written from the standard WMR input
// paths, not from a device. Treat as provisional.
static const char* kBindingsWmr = R"JSON({
  "bindings": {
    "/actions/gamepad": {
      "sources": [
        { "path": "/user/hand/left/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gamepad/in/move" },
                      "click":    { "output": "/actions/gamepad/in/thumb_l" } } },
        { "path": "/user/hand/right/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gamepad/in/turn" },
                      "click":    { "output": "/actions/gamepad/in/thumb_r" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/trigger_l" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gamepad/in/trigger_r" } } },
        { "path": "/user/hand/left/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/grip_l" } } },
        { "path": "/user/hand/right/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/grip_r" } } },
        { "path": "/user/hand/right/input/trackpad", "mode": "dpad",
          "inputs": { "east":  { "output": "/actions/gamepad/in/btn_a" },
                      "south": { "output": "/actions/gamepad/in/btn_b" },
                      "touch": { "output": "/actions/gamepad/in/rest_r" } } },
        { "path": "/user/hand/left/input/trackpad", "mode": "dpad",
          "inputs": { "west":  { "output": "/actions/gamepad/in/btn_x" },
                      "north": { "output": "/actions/gamepad/in/btn_y" },
                      "touch": { "output": "/actions/gamepad/in/rest_l" } } },
        { "path": "/user/hand/left/input/application_menu", "mode": "button",
          "inputs": { "click": { "output": "/actions/gamepad/in/menu" } } }
      ],
      "poses": [
        { "output": "/actions/gamepad/in/aim_l",   "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/gamepad/in/aim_r",   "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/gamepad/in/gpose_l", "path": "/user/hand/left/pose/handgrip" },
        { "output": "/actions/gamepad/in/gpose_r", "path": "/user/hand/right/pose/handgrip" }
      ],
      "haptics": [
        { "output": "/actions/gamepad/out/haptic_l", "path": "/user/hand/left/output/haptic" },
        { "output": "/actions/gamepad/out/haptic_r", "path": "/user/hand/right/output/haptic" }
      ]
    }
  },
  "controller_type": "holographic_controller",
  "description": "BioshockVR shim bindings for Windows Mixed Reality controllers",
  "name": "BioshockVR (shim) WMR bindings"
}
)JSON";

static bool WriteTextFile(const char* path, const char* text)
{
    FILE* f = fopen(path, "w");
    if (!f) { SLOG("!!! input: cannot write %s", path); return false; }
    fputs(text, f);
    fclose(f);
    return true;
}

static const char* XrTypeToManifestType(XrActionType t)
{
    switch (t)
    {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:  return "boolean";
    case XR_ACTION_TYPE_FLOAT_INPUT:    return "vector1";
    case XR_ACTION_TYPE_VECTOR2F_INPUT: return "vector2";
    case XR_ACTION_TYPE_POSE_INPUT:     return "pose";
    case XR_ACTION_TYPE_VIBRATION_OUTPUT: return "vibration";
    default: return "boolean";
    }
}

bool InputShim_Attach(ActionSetRec* set, ActionRec** actions, int actionCount)
{
    char dir[MAX_PATH], modDir[MAX_PATH] = {};
    GetModuleFileNameA(GetModuleHandleA("openxr_loader.dll"), modDir, MAX_PATH);
    char* slash = strrchr(modDir, '\\');
    if (slash) *slash = 0;
    _snprintf(dir, MAX_PATH, "%s\\openvr_input", modDir);
    _mkdir(dir);

    // ---- actions.json (generated from the live action list) ----------------
    char p[MAX_PATH];
    _snprintf(p, MAX_PATH, "%s\\actions.json", dir);
    FILE* f = fopen(p, "w");
    if (!f) { SLOG("!!! input: cannot write %s", p); return false; }

    fprintf(f, "{\n  \"default_bindings\": [\n"
        "    { \"controller_type\": \"knuckles\", \"binding_url\": \"bindings_knuckles.json\" },\n"
        "    { \"controller_type\": \"vive_controller\", \"binding_url\": \"bindings_vive_controller.json\" },\n"
        "    { \"controller_type\": \"oculus_touch\", \"binding_url\": \"bindings_oculus_touch.json\" },\n"
        "    { \"controller_type\": \"holographic_controller\", \"binding_url\": \"bindings_holographic_controller.json\" }\n"
        "  ],\n  \"actions\": [\n");
    for (int i = 0; i < actionCount; ++i)
    {
        const bool out = actions[i]->type == XR_ACTION_TYPE_VIBRATION_OUTPUT;
        fprintf(f, "    { \"name\": \"/actions/%s/%s/%s\", \"type\": \"%s\", \"requirement\": \"optional\" }%s\n",
            set->name, out ? "out" : "in", actions[i]->name,
            XrTypeToManifestType(actions[i]->type),
            (i + 1 < actionCount) ? "," : "");
    }
    fprintf(f, "  ],\n  \"action_sets\": [\n"
        "    { \"name\": \"/actions/%s\", \"usage\": \"leftright\" }\n"
        "  ],\n  \"localization\": [\n    { \"language_tag\": \"en_US\"", set->name);
    for (int i = 0; i < actionCount; ++i)
    {
        const bool out = actions[i]->type == XR_ACTION_TYPE_VIBRATION_OUTPUT;
        fprintf(f, ",\n      \"/actions/%s/%s/%s\": \"%s\"",
            set->name, out ? "out" : "in", actions[i]->name, actions[i]->name);
    }
    fprintf(f, "\n    }\n  ]\n}\n");
    fclose(f);

    _snprintf(p, MAX_PATH, "%s\\bindings_knuckles.json", dir);
    WriteTextFile(p, kBindingsKnuckles);
    _snprintf(p, MAX_PATH, "%s\\bindings_vive_controller.json", dir);
    WriteTextFile(p, kBindingsVive);
    _snprintf(p, MAX_PATH, "%s\\bindings_oculus_touch.json", dir);
    WriteTextFile(p, kBindingsTouch);
    _snprintf(p, MAX_PATH, "%s\\bindings_holographic_controller.json", dir);
    WriteTextFile(p, kBindingsWmr);

    _snprintf(p, MAX_PATH, "%s\\actions.json", dir);
    EVRInputError ie = g_vr.input->SetActionManifestPath((char*)p);
    if (ie != EVRInputError_VRInputError_None)
    {
        SLOG("!!! input: SetActionManifestPath('%s') -> %d", p, (int)ie);
        return false;
    }
    SLOG("input: action manifest set: %s", p);

    char handlePath[160];
    _snprintf(handlePath, sizeof(handlePath), "/actions/%s", set->name);
    ie = g_vr.input->GetActionSetHandle(handlePath, &set->vrHandle);
    if (ie != EVRInputError_VRInputError_None || !set->vrHandle)
    {
        SLOG("!!! input: GetActionSetHandle -> %d", (int)ie);
        return false;
    }

    for (int i = 0; i < actionCount; ++i)
    {
        const bool out = actions[i]->type == XR_ACTION_TYPE_VIBRATION_OUTPUT;
        _snprintf(handlePath, sizeof(handlePath), "/actions/%s/%s/%s",
                  set->name, out ? "out" : "in", actions[i]->name);
        ie = g_vr.input->GetActionHandle(handlePath, &actions[i]->vrHandle);
        if (ie != EVRInputError_VRInputError_None)
            SLOG("!!! input: GetActionHandle('%s') -> %d", handlePath, (int)ie);
    }

    set->attached = true;
    g_inputReady = true;
    SLOG("input: %d actions attached to SteamVR input", actionCount);
    return true;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(
    XrSession, const XrSessionActionSetsAttachInfo* info)
{
    if (!info || info->countActionSets < 1) return XR_ERROR_VALIDATION_FAILURE;
    ActionSetRec* set = (ActionSetRec*)info->actionSets[0];
    if (!set) return XR_ERROR_HANDLE_INVALID;
    if (!InputShim_Attach(set, g_setActions.data(), (int)g_setActions.size()))
    {
        // Attach "succeeds" so the mod keeps running; every action just reads
        // inactive, which the mod already handles by publishing a neutral pad.
        SLOG("!!! input: attach degraded -- controllers will be inactive");
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- sync/state
SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(
    XrSession, const XrActionsSyncInfo* info)
{
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (!g_inputReady) return XR_SUCCESS;

    if (g_vr.ovl && g_vr.ovl->IsDashboardVisible())
        return XR_SESSION_NOT_FOCUSED;

    vr::VRActiveActionSet_t as = {};
    as.ulActionSet = ((ActionSetRec*)info->activeActionSets[0].actionSet)->vrHandle;
    as.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    as.nPriority = 0;
    const EVRInputError ie = g_vr.input->UpdateActionState(
        (VRActiveActionSet_t*)&as, sizeof(vr::VRActiveActionSet_t), 1);
    if (ie != EVRInputError_VRInputError_None)
    {
        static EVRInputError last = EVRInputError_VRInputError_None;
        if (ie != last) { last = ie; SLOG("!!! input: UpdateActionState -> %d", (int)ie); }
    }
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(
    XrSession, const XrActionStateGetInfo* gi, XrActionStateBoolean* out)
{
    if (!gi || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = (ActionRec*)gi->action;
    out->isActive = XR_FALSE;
    out->currentState = XR_FALSE;
    out->changedSinceLastSync = XR_FALSE;
    out->lastChangeTime = 0;
    if (!a || !a->vrHandle || !g_inputReady) return XR_SUCCESS;

    vr::InputDigitalActionData_t d = {};
    if (g_vr.input->GetDigitalActionData(a->vrHandle, (InputDigitalActionData_t*)&d,
            sizeof(d), vr::k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None
        && d.bActive)
    {
        out->isActive = XR_TRUE;
        out->currentState = d.bState ? XR_TRUE : XR_FALSE;
        out->changedSinceLastSync = d.bChanged ? XR_TRUE : XR_FALSE;
        out->lastChangeTime = g_st.lastPredictedTime;
    }
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(
    XrSession, const XrActionStateGetInfo* gi, XrActionStateFloat* out)
{
    if (!gi || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = (ActionRec*)gi->action;
    out->isActive = XR_FALSE;
    out->currentState = 0.f;
    out->changedSinceLastSync = XR_FALSE;
    out->lastChangeTime = 0;
    if (!a || !a->vrHandle || !g_inputReady) return XR_SUCCESS;

    vr::InputAnalogActionData_t d = {};
    if (g_vr.input->GetAnalogActionData(a->vrHandle, (InputAnalogActionData_t*)&d,
            sizeof(d), vr::k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None
        && d.bActive)
    {
        out->isActive = XR_TRUE;
        out->currentState = d.x;
        out->changedSinceLastSync = (d.deltaX != 0.f) ? XR_TRUE : XR_FALSE;
        out->lastChangeTime = g_st.lastPredictedTime;
    }
    return XR_SUCCESS;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(
    XrSession, const XrActionStateGetInfo* gi, XrActionStateVector2f* out)
{
    if (!gi || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = (ActionRec*)gi->action;
    out->isActive = XR_FALSE;
    out->currentState.x = out->currentState.y = 0.f;
    out->changedSinceLastSync = XR_FALSE;
    out->lastChangeTime = 0;
    if (!a || !a->vrHandle || !g_inputReady) return XR_SUCCESS;

    vr::InputAnalogActionData_t d = {};
    if (g_vr.input->GetAnalogActionData(a->vrHandle, (InputAnalogActionData_t*)&d,
            sizeof(d), vr::k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None
        && d.bActive)
    {
        out->isActive = XR_TRUE;
        out->currentState.x = d.x;
        out->currentState.y = d.y;
        out->changedSinceLastSync = (d.deltaX != 0.f || d.deltaY != 0.f) ? XR_TRUE : XR_FALSE;
        out->lastChangeTime = g_st.lastPredictedTime;
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- locate
static bool SpacePoseInOrigin(SpaceRec* s, M34* out)
{
    switch (s->kind)
    {
    case SPACE_REF_LOCAL:
        *out = M34_Identity();
        return true;
    case SPACE_REF_VIEW:
        if (!g_st.hmdValid) return false;
        *out = g_st.hmd;
        return true;
    case SPACE_ACTION:
    {
        if (!s->action || !s->action->vrHandle || !g_inputReady || !g_st.haveOrigin)
            return false;
        vr::InputPoseActionData_t pd = {};
        const EVRInputError ie = g_vr.input->GetPoseActionDataForNextFrame(
            s->action->vrHandle, ETrackingUniverseOrigin_TrackingUniverseStanding,
            (InputPoseActionData_t*)&pd, sizeof(pd), vr::k_ulInvalidInputValueHandle);
        if (ie != EVRInputError_VRInputError_None || !pd.bActive || !pd.pose.bPoseIsValid)
            return false;
        *out = M34_Mul(g_st.originInv, M34_FromVr(pd.pose.mDeviceToAbsoluteTracking));
        return true;
    }
    }
    return false;
}

SHIM_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(
    XrSpace space, XrSpace base, XrTime, XrSpaceLocation* out)
{
    if (!space || !base || !out) return XR_ERROR_VALIDATION_FAILURE;
    out->locationFlags = 0;
    out->pose.orientation = { 0, 0, 0, 1 };
    out->pose.position = { 0, 0, 0 };

    M34 t, b;
    if (!SpacePoseInOrigin((SpaceRec*)space, &t)) return XR_SUCCESS;
    if (!SpacePoseInOrigin((SpaceRec*)base, &b)) return XR_SUCCESS;

    const M34 rel = M34_Mul(M34_InvRigid(b), t);
    float q[4], p[3];
    M34_ToQuatPos(rel, q, p);
    out->pose.orientation = { q[0], q[1], q[2], q[3] };
    out->pose.position = { p[0], p[1], p[2] };
    out->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                         XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}
