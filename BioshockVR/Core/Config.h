// BioshockVR/Core/Config.h
#pragma once

// EVERY SETTING, IN ONE PLACE.
//
// This used to be ~138 loose globals defined in dllmain.cpp and re-declared as
// `extern` at the top of every consumer -- 35 of them in CameraHook.cpp alone.
// Adding a setting meant editing three or four files, and nothing checked that a
// consumer's `extern` matched the definition. That is not hypothetical: the
// global ControllerLayout was defined as 1 while its ini read defaulted to 0,
// two different answers to "what is the default", and it survived because no
// single place had to agree with itself.
//
// One struct, one instance, one declaration. A consumer includes this header and
// says g_cfg.hudRedirect. The compiler now checks every use against one type.
//
// FIELD NAMES ARE MECHANICAL: g_cfgHudRedirect -> g_cfg.hudRedirect. Strip the
// prefix, lowercase the first letter, change nothing else. The single exception
// is g_cfg6DofHands -> g_cfg.sixDofHands, because an identifier cannot start
// with a digit.
//
// THE INI IS THE INPUT, THE STARTUP ECHO IS THE AUTHORITY. Config_Echo prints
// every value actually read; if a change is not in that block it did not take.
// Read-only files, the wrong storefront profile and VirtualStore redirection
// have all silently defeated edits here.

// How many ExecCommandN slots the ini offers. Eight because that is more than
// any planned experiment group needs and the array costs 1.5 KB.
static const int kExecCommands = 8;

struct VrConfig
{
    // core -----------------------------------------------------------------------
    float fovDeg = 100.0f;      // MUST equal Bioshock.ini HorizontalFOV
    int   resX = 0;
    int   resY = 0;
    int   fullscreen = -1;      // -1 leave alone / 0 windowed / 1 exclusive
    bool  forceFlip = false;    // rewrite the swapchain flip-model + ALLOW_TEARING
    int   mirrorEvery = 1;      // present the desktop mirror every Nth frame. 0 = never
    float eyeSep = 3.2f;        // half-IPD, game units == cm. 3.2 = 64mm IPD
    bool  swapEyes = false;     // flip if depth is inverted
    bool  disableVSync = true;
    bool  syncGameIni = true;   // push FOV/res into Bioshock.ini; 0 = leave

    // camera & comfort -----------------------------------------------------------
    bool  cameraHook = true;    // install the camera hook at all
    bool  cameraWrite = false;  // let it MODIFY the camera (Phase 6 switch)
    bool  headTracking = false; // compose HMD orientation onto the camera
    bool  headPosition = false; // apply the head's translation
    bool  headRoll = true;
    bool  headAim = false;      // make Controller.Rotation follow the aim
    bool  disableHeadBob = false;   // start OFF until EYEHEIGHT is verified
    int   headAimMode = 1;      // 0 additive, 1 local compose, 2 pitch-decoupled
    bool  pairLock = true;      // render both eyes from the same instant
    float heightOffset = 0.0f;  // CameraHeightOffset, cm, +up
    bool  cutsceneTheater = false;  // show cutscenes on the flat quad
    bool  scriptedQol = false;  // M7-S2: during a scripted hand animation,
                                // unhide the arms, stop driving the hands from
                                // the controllers, and stop writing the aim
                                // field. Head look is untouched.
    int   deltaClamp = 0;           // 0 off, 1 player world, 2 BOTH worlds

    // weapon & arm rendering -----------------------------------------------------
    int   fgFovOffset = 0;      // ForegroundFovOffset, 0 == off
    float fgFovValue = 0.0f;    // ForegroundFovValue, 0 == use GameFovDegrees
    bool  fgFovAuto = false;    // derive ForegroundFovValue from the backbuffer
    int   fgFovSrc = 0;         // ForegroundFovSrcOffset, 0 == off
    int   worldFovOff = 0;      // controller+N -> world FOV. 0 == off
    int   worldFovOff2 = 0;     // its mirror. 0 == off
    float worldFovMax = 0.0f;   // above this, snap back. 0 == off
    float worldFovMin = 0.0f;   // below this, snap back. 0 == off
    float worldFovVal = 75.0f;  // the value to snap back to
    float handsScale = 0.0f;    // DrawScale for the arms. 0 == leave alone
    float gunScale = 0.0f;      // DrawScale for the weapon actor. 0 == off
    int   gunPtrOff = 0;        // GunPtrOffset. 0 == run the sweep
    int   gunPtrBase = 1;       // 0 == pawn, 1 == Hands
    int   gunChildren = 0;      // 0 off, 1 sweep, 2 scale all

    // 6-DOF hands ----------------------------------------------------------------
    bool  sixDofHands = false;
    bool  hideArmSleeves = false;

    // 0 == do not check this vtable. MEASURED: Steam and Epic have DIFFERENT vtable
    // RVAs (AHands 0xD8A28C vs 0xD8959C, SkeletonInstance 0xE19ACC vs 0xE190EC) and
    // the shift is not a constant -- 0xCF0 for one, 0x9E0 for the other -- so no
    // single value can serve both stores. The check was only ever a fourth guard:
    // HandsProbe identifies the actor positionally, the skeleton must point back at
    // that same actor, and the bone count must be exactly 47. Put a real RVA here
    // to re-enable it on a build you have measured.
    int   armHideHandsVt = 0;
    int   armHideSkelVt = 0;

    // M6-S1. READ-ONLY bone dump: which space is the render bone array in, what
    // are its lanes and units, and does it stay live while the sleeves are
    // hidden. Diagnostic only, writes nothing, and it stops on its own.
    bool  handRigProbe = false;

    // M6-S4. One-shot scan for the ability class list, plus a per-SWITCH search
    // for the pawn field naming the equipped plasmid. Read-only, never per
    // frame, and the match is self-validating.
    bool  plasmidProbe = true;

    // M6-S1: THE TRACKED FREE HAND.
    // 0 off, 1 position, 2 position and rotation, 3 axis sweep (diagnostic).
    // Applies to exactly the slots that HIDE the free hand today; the two-handed
    // weapons keep both hands on the gun and are left alone.
    //
    // Named OffHandTracked in the ini, and LeftHandTracked is still read as a
    // fallback -- it governed only the left hand until plasmids brought the
    // right one in, and an ini tuned before that should not stop working.
    int   offHandTracked = 0;

    // Which actor axis feeds each MODEL lane, 1-based and signed:
    // 1 forward, 2 right, 3 up. The default is the identity the M6-S1 rest-pose
    // dump PREDICTS -- right wrist +47 fwd / +27 right / -20 up, left wrist
    // hanging at -3 / -31 / -82 -- but a prediction is not a measurement, which
    // is why it lives here and not in the source. LeftHandTracked=3 measures it.
    int   leftHandAxis[3] = { 1, 2, 3 };

    // Per hand, because the two are mirror images and a value tuned for one is
    // wrong for the other. Only one is ever in use at a time -- you are holding
    // either a weapon or a plasmid.
    float leftHandOffset[3] = { 0.f, 0.f, 0.f };   // fwd,right,up cm to the wrist
    float leftHandRot[3] = { 0.f, 0.f, 0.f };   // pitch,yaw,roll deg trim
    float rightHandOffset[3] = { 0.f, 0.f, 0.f };
    float rightHandRot[3] = { 0.f, 0.f, 0.f };

    bool  handsProbe = false;
    int   handsPtrOff = 0;      // HandsPtrOffset, e.g. 0x724
    int   handsPosOff = 0;      // HandsPosOffset, e.g. 0x1D8
    float handsGrip[3] = { 0.0f, 0.0f, 0.0f };   // fwd,right,up cm
    float gripTunedFgFov = 0.0f;  // fg FOV the grip offsets were tuned at. 0 == off
    float handsRot[3] = { 0.0f, 0.0f, 0.0f };   // pitch,yaw,roll deg  LIVE
    float gripSlot[9][3] = {};                  // per-weapon position, from ini
    float rotSlot[9][3] = {};                   // per-weapon rotation, from ini
    float cursorRot[3] = { 0.f, 0.f, 0.f };     // CursorOffset p,y,r deg. LIVE
    float cursorSlot[9][3] = {};                // per-weapon, from the ini
    int   handsArmCalls = 600;    // CalcView calls before the probe arms
    int   handsRetryCalls = 600;  // calls between STAGE A retries
    int   idleAnimMode = 0;       // 0 off, 1 entry[0], 2 HandsDown, 3 Equipping
    int   idleModeSlot[9] = {};   // per-weapon override
    int   hideArmsSlot[9] = {};                 // per-weapon arm suppression
    float handsNudgeZ = 0.0f;     // probe only
    float handsNudgeYaw = 0.0f;   // probe only
    float handsNudgePitch = 0.0f; // probe only

    // aiming / crosshair ---------------------------------------------------------
    int   aimSource = 0;        // 0 head, 1 right controller

    // With nothing equipped there is no crosshair, so nothing shows where the
    // controller points -- look at a thing to pick it up instead. Moves ONLY the
    // aim direction, never the view, and never during scripted sequences.
    int   headAimUnarmed = 1;
    float aimClampDeg = 20.0f;
    float aimSmooth = 0.35f;
    float plasmidAimPitch = -50.0f;   // deg added to the plasmid hand's aim pitch
    bool  disableReticle = true;
    int   engPtrRva = 0x1375368;    // UGameEngine* location
    int   engVtRva = 0x00E0DFF4;    // its expected vtable, for verification
    int   engExecRva = 0x004C5970;  // UGameEngine::Exec
    int   engExecThis = 0x40;       // FExec subobject offset

    // HUD quad ------------------------------------------------------------------
    bool  hudRedirect = true;    // capture the interface off the eye texture
    float hudWidthDeg = 70.0f;   // angular width of the quad -- THE scale knob
    float hudDist = 2.0f;        // metres; affects vergence, not apparent size
    float hudPitchDeg = 0.0f;    // + up
    float hudYawDeg = 0.0f;      // + right
    bool  crosshair = true;
    float xhSize = 0.012f;      // dot diameter, metres, at CrosshairDistance
    float xhDist = 2.0f;
    bool  hudAlphaFix = true;
    int   hudDsvMode = 1;   // 0 none, 1 private D24S8, 2 the game's

    // controller -----------------------------------------------------------------
    bool  controller = true;
    int   controllerMode = 1;   // 0 = XInput slot 0 wins, 1 = VR replaces it
    int   controllerLayout = 1; // 0 literal Xbox, 1 jump on right-A
    bool  controllerPitch = false;
    bool  stickYToDpad = false;
    float stickDeadzone = 0.15f;
    bool  controllerLog = true;
    int   dpadModifier = 1;     // 0 off / 1 right thumbrest / 2 R3 / 3 left grip
    int   dpadFlip = 0;   // ControllerDpadFlip: 1 = left thumbrest + right stick
    int   pauseChord = 1;       // X+Y together -> START (pause)
    bool  jumpOnR3 = false;     // R3 -> jump instead of zoom

    // menus ----------------------------------------------------------------------
    bool  menuScreen = true;
    float menuSize = 1.5f;
    float menuDist = 1.75f;
    float menuHeight = 0.0f;
    int   menuMaxIndexed = 8;
    int   menuMaxDraw = 0;      // RETRACTED (S31), default 0
    char  menuList[256] = {};
    char  anchorList[256] = {};   // in-game UIs that belong on the world-locked quad

    // debug / probe --------------------------------------------------------------
    bool  drawHook = true;
    bool  gameState = true;     // read the game's own input context
    bool  nativeScan = true;    // M3-S1: locate the native property accessors
    bool  forcedMoveProbeAll = false;  // log non-bool transitions too (noisy)
    bool  forcedMoveProbe = false;  // M7-S1: diff the controller/pawn for a
                                    // scripted-event flag. A periodic diff, so
                                    // unlike nativeScan this ships OFF.
    bool  hookInstanced = false;
    char  suppressList[256] = {};
    char  isolateList[256] = {};
    char  weaponList[256] = {};
    float weaponScale = 0.0f;
    char  hudList[256] = {};
    float hudScale = 0.0f;
    char  arrowList[256] = {};   // ArrowCounts, e.g. 234i@512x512
    float arrowScale = 1.0f;     // 0 == leave size alone
    float arrowX = 0.0f;         // fraction of viewport width, + == right
    float arrowY = 0.0f;         // fraction of viewport height, - == up
    int   arrowPtrOff = 0;                    // pawn+N -> the arrow actor. 0 == off
    float arrowWorld[3] = { 0.f, 0.f, 60.f }; // fwd,right,up from the camera, cm
    int   hideInactiveHand = 1;   // HideInactiveHand
    int   hideHandSlot[9] = {};   // HideInactiveHandN, per weapon slot

    // M6-S5. Walks the decoded FlashGUIController chain and logs which named
    // Flash movie is on top, only when it changes. Read-only, nothing is called.
    int   flashGuiProbe = 1;

    // Diagnostics for two long-standing reports, both read-only and both silent
    // until the thing they measure happens. Neither changes behaviour.
    int   turnRateProbe = 1;    // is right-stick turn speed frame-rate linked?
    int   scriptedRotProbe = 1; // what turns the player during the balcony fall?

    // Follow the game's OWN camera rotation through a scripted sequence, rather
    // than only the aim field. DEFAULT OFF: the aim field is measured to receive
    // essentially nothing during the balcony fall (one 1.85-degree event in 67
    // seconds), so the camera is the remaining candidate for where that scene's
    // rotation lives. Shipped beside the probe that will confirm or kill it.
    // CONFIRMED AND SHIPPED ON, 2026-08-11. Across the balcony fall the game put
    // 0.00 deg/s into the aim field and up to 125 deg/s onto its OWN camera
    // rotation, with every gate open -- so the scene's rotation was never on the
    // field we follow, and head aim was overwriting the one it was on.
    int   scriptedCameraFollow = 1;

    // Let a scripted scene reach its OWN framing even after you have turned
    // yourself with the right stick during it. Tracks the yaw the PLAYER added
    // and spends it back down as the scene rotates.
    //   0  off      the scene's rotation lands on top of your offset
    //   1  wash out cancelled degree for degree as the scene turns
    //   2  drop it  the whole offset goes the first frame the scene turns
    // Never touches the head: the view stays 1:1 with your neck in all three.
    int   scriptedRecentre = 0;

    // ScriptedEntryHeading lived here and is GONE -- falsified in a headset,
    // 2026-08-11. It substituted a heading into the aim field as a scripted
    // window opened, on the theory that a forced move steers by whatever we last
    // left there. A forced move steers by nothing of ours: under M7-S6, which
    // never writes the field during a sequence, three falls entered at wildly
    // different controller angles landed on the SAME spot. With the substitution
    // on, both straight-on runs landed badly wrong -- the write is the damage.

    // Ground truth for the residual walk drift: where the pawn ACTUALLY went,
    // against where the mode promised. Read-only, silent unless you are moving.
    int   walkDriftProbe = 1;

    // WalkFromPawnYaw lived here and is GONE -- falsified 2026-08-11. The pawn's
    // rotator tracks the aim field exactly (60 of 62 samples read `aim-pawn
    // +0.0`, including while a 76-degree controller offset was held), so
    // measuring against it changed nothing.

    // The game's own turn sensitivity, written into Bioshock.ini. 70 is the "7"
    // the tester settled on; the shipped game default reads far too slow in VR.
    // -1 leaves whatever the game already has alone.
    int   gameTurnSpeed = 70;

    // ExecCommand1..8 -- console commands issued once at startup through
    // ExecQueue. An empty entry is skipped, so the ini can leave gaps.
    //
    // THIS IS AN EXPERIMENT CHANNEL, NOT A FEATURE. It exists so a question like
    // "what does the wrench trace actually read?" can be answered by editing one
    // ini line instead of by a build, a deploy and a headset cycle. Anything
    // that earns a permanent home gets its own setting.
    char  execCommand[kExecCommands][192] = {};
    int   hideCutsceneBars = 1;   // HideCutsceneBars
    int   cutsceneBarVerts = 29;  // CutsceneBarVertices
    int   swingEnabled = 1;
    float swingThreshold = 2.0f;
    float swingRearm = 1.5f;
    int   swingCooldownMs = 180;
    int   swingPulseMs = 120;
    int   swingDelayMs = 0;
    int   swingLog = 0;
    float swingOutFrac = 0.60f;
    float swingTravel = 0.15f;
    float gripThreshold = 0.80f;
    float gripHysteresis = 0.15f;
    int   headRelativeMove = 1;   // LEGACY -- seeds movementMode, see below

    // WHO STEERS YOUR WALKING. LOCOMOTION ONLY -- this no longer decides who
    // AIMS. That split is what makes four modes possible; see headAimAlways.
    //
    //   0 NEITHER     right stick only. Neither looking nor pointing changes
    //                 where you walk -- a flat-screen shooter's locomotion.
    //   1 CONTROLLER  where you point. Looking around does not steer. (default)
    //   2 HEAD        where you look. Pointing does not steer.
    //   3 BOTH        where you point, plus where you look on top of it. What
    //                 the mod did for its whole life before these four existed.
    //
    // HOW, because it is not obvious and the obvious reading is wrong. The aim
    // field carries the CONTROLLER in all four -- it is never rewritten here.
    // The game measures the walk direction from that field and then applies the
    // stick angle, so rotating the STICK by R redirects walking while leaving
    // aim, the weapon trace and forced-move sequences untouched:
    //
    //     walk = aimFieldYaw + stickAngle + R
    //
    // With H = head yaw and O = the controller's clamped offset from the head
    // (so the controller's absolute yaw is C = H + O):
    //
    //     0 NEITHER     R = -(H + O)      1 CONTROLLER  R = 0
    //     2 HEAD        R = -O            3 BOTH        R = H
    //
    // This is the same trick HeadRelativeMove has always used for mode 3; the
    // other three are just different values of R. It is ALSO why graveyard entry
    // 13 does not apply: that entry binds AIM to Controller.Rotation, and this
    // never touches Controller.Rotation.
    //
    // WHY MODE 3 IS NOT THE DEFAULT ANY MORE. Pointing and looking both steering
    // means a 90-degree head turn plus a 90-degree point walks you backwards.
    // That was reported as "too extreme" and diagnosed as a double application.
    // It is a legitimate mode, not a bug -- but it is a surprising default.
    //
    // Seeded from AimSource and HeadRelativeMove when the key is absent, so an
    // ini written before this behaves exactly as it did.
    int   movementMode = 1;

    // PURE HEAD AIM, independent of who steers. The aim field carries your head
    // instead of your controller, so the crosshair, the weapon trace and the
    // view all follow where you look.
    //
    // SEPARATE FROM movementMode ON PURPOSE. It used to be welded to mode 0,
    // which meant you could not have head aim with any other locomotion, and
    // could not have head-steered locomotion without also losing controller aim.
    // Expected to become the default once the two-handed weapon grip lands.
    int   headAimAlways = 0;
    int   snapTurn = 0;
    float snapTurnDeg = 45.0f;

    // THE GAME'S MOVEMENT DEADZONE IS SQUARE -- per axis, 0.225, straight out of
    // its own binding file. Rotating the stick to redirect walking moves
    // magnitude between the two axes and the game then shrinks each one
    // independently, which distorts the DIRECTION by up to ~11 degrees and
    // collapses it to pure strafe once the forward lane falls under the
    // threshold. That is the whole of the long-reported walk drift.
    //
    // StickPrecomp inverts it on the way out. GameStickDeadzone is exposed so a
    // profile with a different threshold can be corrected without a rebuild.
    int   stickPrecomp = 1;
    float gameStickDeadzone = 0.225f;

    // TURN RESPONSE. The game's own turn rate is nearly vertical at the top of
    // the stick -- 0.98 gives ~105 deg/s, 0.99 ~140, 1.00 ~200, measured. So the
    // same push landing 2% differently doubled the speed. These remap the axis
    // into a range that excludes the cliff, trading top speed for repeatability.
    // Raise turnAxisMax toward 1.0 to get the spike back.
    float turnAxisMax = 0.95f;
    float turnAxisExp = 1.0f;   // 1.0 linear; >1 finer control near centre
    int   freezeGameRot = 0;   // discard the game's pitch and roll (shake/kick)
    // M7-S6: discards the game's rotation during ordinary play only -- shake,
    // weapon kick, the auto-pan toward enemies. Gated on the stick being centred
    // so your own turning survives without ModYaw, and excluded during scripted
    // animations, FORCED MOVES and BATHYSPHERE RIDES, all three measured. Now on
    // by default; the exclusions it was waiting for exist.
    int   freezeGameplayRot = 1;

    // A scripted sequence the player can still WALK THROUGH keeps the aim.
    //
    // ON, and MEASURED 2026-08-10 rather than assumed. Two windows in one run,
    // and both candidate signals separated them outright:
    //   locked cutscene, 108 s : hud 0 throughout, aForward/aStrafe 0.0 EVERY
    //                            sample -- the player provably was not moving
    //   the Big Daddy scene    : hud 1 throughout, axes swinging +-950
    int   controllableScripted = 1;
    bool  scriptedProbe = true;
    // COMFORT. 1 = the scripted camera turns you to face the action (default,
    // and what makes cutscenes read correctly). 0 = the view holds still and you
    // turn yourself with the right stick, for people the automatic motion makes
    // sick.
    int   scriptedRotFollow = 1;
    // How much rig motion counts as "animating" for the scripted arm gate.
    // Calibrated from the logged raw/smoothed values -- see ScriptedHandsMoving.
    float scriptedHandsMotion = 0.02f;

    // How long the arm gate keeps the hands up after the rig stops moving.
    // TUNABLE BECAUSE THE RIGHT ANSWER DIFFERS BY SCENE: the plasmid injection
    // holds a pose for 2.5-4.5 s mid-animation and the hands vanish, while the
    // Little Sister crawl needs the gate sharp so they stay hidden after the
    // bottle catch. 300 was hardcoded and is kept as the default.
    int   scriptedHandsHoldMs = 300;

    int   modYaw = 0;          // mod owns yaw: stick turns g_aimBase directly
    float modYawSpeed = 90.0f; // deg/sec at full deflection
    int   forceFocus = 1;   // ForceWindowFocus
    int   pitchServo = 0;   // OFF: feeding RY back in fights the head-aim accumulator and freezes the view. See CameraHook.
    float pitchServoGain = 0.030f;
    float pitchServoDead = 2.0f;
    float pitchServoMax = 0.80f;
    int   xhFromShot = 1;   // CrosshairFromShot
};

// The one instance. Defined in Config.cpp.
extern VrConfig g_cfg;

// Reads BioshockVR.ini into g_cfg, applies the cross-setting fixups, and emits
// the startup echo. `iniPath` is retained for the live-tuning writer below.
// Read-only with respect to the ini except through Cfg_WriteVec3.
void Config_Load(const char* iniPath);

// Live-tuned values written straight back to the ini, so a grip found in the
// headset cannot be lost by closing the game. Same "%.2f,%.2f,%.2f" shape the
// reader parses, so a value written here reloads exactly as it was.
void Cfg_WriteVec3(const char* key, const float v[3]);
