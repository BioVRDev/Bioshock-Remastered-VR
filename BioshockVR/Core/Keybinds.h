// BioshockVR/Keybinds.h
//
// ONE PLACE for every hotkey in the mod.
//
// Numpad keys are unusable on a laptop or a tenkeyless keyboard, which made
// live tuning impossible for a chunk of users with no workaround short of
// rebuilding. Every binding now comes from the [KEYS] section of
// BioshockVR.ini, and Key_Init logs the whole resolved table plus any
// duplicates -- so "the numpad isn't reaching the game" is a visible line
// instead of a guess.
//
// CORRECTED against the source. Three defaults below are deliberately NONE
// because they collided with something else in the shipped build:
//
//   NUMPAD 9 was read by BOTH HandsProbe (cycle grip edit mode) and DrawHook
//            (dump one frame's render-target structure).
//   PGUP     was read by THREE handlers: HandsProbe's mode cycle, HandsProbe's
//            HandsModeSnapshot, and GameState's SnapshotFloats.
//   PGDN     was read by TWO: HandsModeDiff and DiffFloats.
//
// One press fired all of them. The probes are debug tools, so they lose the
// binding and the user-facing functions keep it. Bind them in [KEYS] if you
// need them.
//
// USAGE
//   Key_Init(iniPath)                once, from InitThread, after the log opens
//   Key_Down(KEY_GRIP_FWD_PLUS)      is it held right now?
//   Key_Fired(KEY_HUD_DECREASE, prev) rising edge, caller owns the prev flag
#pragma once

enum KeyId
{
    // ---- weapon grip tuning (HandsProbe.cpp) -------------------------------
    // The six axis keys edit whichever of the three modes is active, so their
    // meaning changes with KEY_GRIP_MODE. See the ini for the full explanation.
    KEY_GRIP_FWD_PLUS = 0,   // NUM8   forward   / pitch up
    KEY_GRIP_FWD_MINUS,      // NUM2   back      / pitch down
    KEY_GRIP_RIGHT_PLUS,     // NUM6   right     / yaw right
    KEY_GRIP_RIGHT_MINUS,    // NUM4   left      / yaw left
    KEY_GRIP_UP_PLUS,        // NUM0   up        / roll right
    KEY_GRIP_UP_MINUS,       // NUM5   down      / roll left
    KEY_GRIP_MODE,           // NUM9   cycle POSITION -> ROTATION -> CURSOR
    KEY_GRIP_STEP,           // NUM7   cycle the step size

    // ---- HUD panel tuning (XRSession.cpp) ----------------------------------
    KEY_HUD_PARAM,           // DELETE cycle WIDTH -> DISTANCE -> PITCH -> YAW
    KEY_HUD_DECREASE,        // F11    decrease the selected parameter
    KEY_HUD_INCREASE,        // F12    increase the selected parameter

    // ---- camera (CameraHook.cpp) -------------------------------------------
    KEY_RECENTER,            // NUMDEC re-capture the seated origin
    KEY_AIM_CANDIDATE,       // NUMADD cycle head-aim field (+0x1E4 / +0x328)

    // ---- HUD capture (DrawHook.cpp) ----------------------------------------
    KEY_HUD_REDIRECT,        // HOME   toggle the HUD panel on/off live

    // ---- draw-call probes (DrawHook.cpp) -- debug only ---------------------
    KEY_DRAW_CLEAR,          // NUMMUL clear the collected draw table
    KEY_DRAW_TABLE,          // NUM3   dump the draw table to the log
    KEY_DRAW_SUPPRESS,       // NUM1   toggle SuppressIndexCounts
    KEY_DRAW_ISO_NEXT,       // NUMSUB next isolate candidate
    KEY_DRAW_ISO_OFF,        // NUMDIV isolate off
    KEY_DRAW_FRAME,          // NONE   was NUM9 -- collided with KEY_GRIP_MODE

    // ---- memory probes (GameState.cpp / HandsProbe.cpp) -- debug only ------
    KEY_PROBE_SNAPSHOT,      // PGUP   snapshot the watched float block
    KEY_PROBE_DIFF,          // PGDN   diff against the snapshot
    KEY_HANDS_SNAPSHOT,      // NONE   was PGUP -- collided with the two above
    KEY_HANDS_DIFF,          // NONE   was PGDN -- collided with the two above

    KEY_COUNT
};

// Reads [KEYS] from the ini, logs the resolved table, warns on duplicates.
// Safe to call more than once.
void Key_Init(const char* iniPath);

// Is the bound key held this instant? Always false for an unbound (NONE) key.
bool Key_Down(KeyId id);

// Rising edge. The caller owns `prev` so several call sites can watch the same
// key independently without fighting over one shared latch.
bool Key_Fired(KeyId id, bool& prev);

// The raw VK behind a binding -- for code that still wants GetAsyncKeyState.
// Returns 0 for an unbound key; GetAsyncKeyState(0) is harmless but useless,
// so check for 0 if you are building a table.
int  Key_Vk(KeyId id);

// Human-readable name of the current binding, for logs and on-screen help.
const char* Key_Name(KeyId id);
