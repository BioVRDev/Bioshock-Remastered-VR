# Current state

**Updated:** 2026-08-10 · **Baseline:** `002a81a` "Pre refactor commit" ·
**Version string:** `1.0.3` (unreleased; `1.0.2` is public)

This file replaces the handoff documents that used to be written by hand between
chat sessions. It is the answer to "where is this project right now".

> **The two `BioShock_Remastered_VR_*` handoff MDs are now partly wrong.** They
> list the duplicate-world square as the open P0; it is fixed. Where they and
> `docs/INVARIANTS.md` disagree, the docs win — they were verified against source.

---

## Next step

**The next session is RESEARCH AND PLANNING ONLY — no code.** Requested
explicitly (2026-08-10, late). New features to be scoped; nothing to build until
the outstanding builds below have been in a headset.

> ## ⚠ TWO BUILDS ARE DEPLOYED AND UNTESTED
> The DLL in the game folder is `M6-S1 movement modes + plasmid/GUI locate` and
> **nothing in the last two builds has been in a headset.** Do not treat any of
> it as working, and do not stack another build on it. The tester is away until
> 2026-08-11.
>
> Untested, in deploy order:
> - **The free hand for plasmids** — the right-cluster write. Includes the
>   **bone 43 position write**, and *telekinesis release has never been tried
>   against it*. That is the one path here that can **crash** rather than look
>   wrong; the release test was asked for and not reached.
> - **`MovementMode` 0/1/2** and the double-applied-head-yaw fix.
> - **The two locate probes** (`PlasmidProbe`, the `FlashGUIController` native
>   row). Read-only, so they cannot break anything — but they have produced no
>   data yet, and M6-S4/S5 both wait on that log.
>
> **Two commits are unpushed**: `0503c0e`, `a3484ce`. The working tree is clean.

**When testing resumes**, the first run should cover, in this order: telekinesis
release with a plasmid equipped (the crash risk), then `MovementMode=0` (does
turning 90° still walk you backwards?), then cycling every plasmid to feed the
probe log.

**M6-S1 is DONE and verified.** The tracked left hand works in ordinary play and
in scripted events. **The cluster transform exists**, which is the mechanism
M6-S2 (two-handed grip) and M6-S3 (left-handed mode) are both configuration on
top of — read `docs/modules/hands.md` § *The cluster transform* before either.

**M7 is closed and verified.** The tester's words: "Perfect on every front."
Scripted events land where they intend, arms and hands appear only while the rig
is animating, the entry stall is gated, and gameplay screenshake is gone.

**M6 is the hand rig**, and its shape is already settled by
`docs/proposals/vr-features-research.md`: separate hands, two-handed grip and
left-handed mode are **one mechanism** — a rigid transform on a bone cluster.
Build it once; the three are configuration on top.

> **The trap M6 will hit, and it has already cost a cycle once.** Writing bones
> clears the dirty byte, which freezes the whole array — and M7's working
> arm feature samples bone 27 from that array to decide whether the arms show.
> A cluster write that ignores this will break a signed-off feature silently.
> `docs/INVARIANTS.md`: *you cannot hide by bone and measure by bone at the same
> time.*



**Run `.planning/sessions/M3.md` § M3-S2 — and read the ⚠ box on that card
first**, because the signature is not the one S2 was written against. Read the
card, not this section.

**M3-S1 passed on 2026-08-10, in one cycle.** `GetPropertyTextByName` is at rva
`0x7346E0` on Steam, identical across two launches, and all four property
accessors were located. The address is live in the mod today via
`EngineBridge_GetPropertyTextByName()` and **nothing calls it** —
`docs/modules/enginebridge.md`.

So for the first time in this arc the blocker is not *"can we read live engine
state"* but *"can we invoke this correctly without crashing"*. That is a
genuinely different question and it is the one S2 asks.

**What S2 inherits, and the trap in it.** The located symbol is
`exec`GetPropertyTextByName —
`void UObject::execGetPropertyTextByName(FFrame&, RESULT_DECL)`, a `__thiscall`
member taking a **bytecode frame**. There is no
`obj->GetPropertyTextByName('Health')` entry point anywhere. S2's card was
written assuming one, so its "do not tune the calling convention more than
twice" budget means *build the frame differently*, not *try another convention*.

**Why M3 and not M2:** M1-S2 was the pivotal test of the whole arc and it came
back **no**. M2 (StateBus, HUD gate, cutscene anchor, comfort) is all downstream
of a working signal, so it stays blocked until one exists. M3 — the native
property call, finding 2 — is the next untried source. M1-S3 is skipped.

### The premise, after M1 — 2026-08-09

The blocker was framed as *this UE2.5 fork has no reflection system*. That framing
is still wrong, and the corpus still says so. But **M1 turned the corpus findings
from three predictions into one measured negative and two untested claims**, and
the distinction it drew is the most useful thing to carry forward:

> **The corpus tells you what the script *can* do. It does not tell you what the
> shipped game *does*.**

1. **`myHUD.bHideHUD` — FALSIFIED LIVE (M1-S2).** The script really is written the
   way finding 1 said. The bit still never moves. Offsets confirmed and kept
   (`docs/ENGINE-MAP.md`); the *inference* that retail cutscenes run through
   `ActionCinematicEnter`/`Exit` is dead. Grave 1 in `CLAUDE.md`.
2. **`Core.Object` exposes native `GetPropertyTextByName(name)`**, and retail script
   calls it *on live instances*. **Untested.** Console `get` returns class defaults
   because the console resolves a **class** — that measurement stands, the
   conclusion drawn from it did not. **This is M3 and it is now the main line.**
3. **`LastPlayerInputContext` is also declared on `ShockPlayerController`**
   (`:42`). Every scan so far has been on the **pawn**. The controller copy has
   still never been looked at. **Untested**, and M3-S3 reads it.

Treat 2 and 3 the way 1 should have been treated: as predictions that owe a live
read. M1 cost two cycles and returned one solid measurement plus one clean
negative, which is the system working.

Do **not** re-enable the pitch latch, pitch servo, S75 unwind, or
ViewActor divergence as a shortcut. All four are falsified with evidence in
`docs/INVARIANTS.md`.

---

## Recently closed

**M6-S1 — the tracked free hand (2026-08-10).** Verified: *"Looks and feels
fantastic. Works perfectly, even in the scripted events."* The hand that is not
holding your weapon appears and follows its own controller on exactly the slots
that hide it today; the two-handed weapons are untouched. Live numpad tuning on
Numpad 9, saved back to the ini.

Four measurements came out of it, all in `docs/ENGINE-MAP.md` § *Skeleton* and
`docs/INVARIANTS.md`:

- The render bone array is a **common model space, centimetres, on the actor's
  own axes**. Confirmed, not inferred.
- **The array keeps evaluating in ordinary play with the dirty byte cleared** —
  bone 27 moved between every pair of dumps. Scoped to ordinary play; M7-S4's
  latch was real and is a different window.
- **"Apply late" was wrong for bones.** S59/S60 measured the actor *rotator*
  being erased, not the array. CalcView plus the dirty byte is the seam.
- **The build stamp does not always advance** — `__TIME__` is baked in when
  `dllmain.cpp` compiles, so an incremental build that misses that file ships a
  new DLL reporting an old time. Bump the label every session.

Two bugs the headset found, both instructive and both fixed:

1. **A sharp spike out of the palm** — vertices weighted across a bone we moved
   and a bone we did not. `CollapseArm` pins the sleeve at the wrist but runs
   *before* the cluster write, reading the engine's wrist. Diagnosed from one
   tester sentence: the spike followed the *right* hand while the rest of the
   hand followed the left.
2. **The right hand dragged the left** — the placement offset was applied in the
   actor's frame, and the actor is rotated by the weapon hand. Everything else
   cancels the actor out algebraically, so the offset was the only term that
   *could* couple them.

**The controllable scripted sequence — MEASURED and fixed (2026-08-10).** The
Big Daddy killing a splicer is a scripted scene the player walks through, and the
mod treated it like a locked one: `ScriptedAimReleased()` fired, the aim was
handed back, and head-look stopped steering. Tester at `=1`: *"test 2 worked well
and the big daddy splicer fight felt mostly normal."*

The evidence, one run, two windows, and both candidate signals separated them:

| | locked cutscene | Big Daddy |
|---|---|---|
| duration | 108 s | ~ |
| HUD drawing | 0 throughout | **1 throughout** |
| `aForward`/`aStrafe` | **0.0 every sample** | ±950, constantly |
| `hands+0x594` | `0x6` | `0x7` |

**Noticed, recorded, not acted on:** that `hands+0x594` bit-0 difference is a
third and cheaper discriminator, in a DWORD already read every frame. One sample
per case is not enough to trust it.

**The crosshair now knows what is in your hands (2026-08-10).** Hidden when
nothing is equipped and during scripted scenes. Verified both: *"the crosshair is
correctly gone during the opening"* and *"no crosshair in scripted event is
working correctly now."* The signal cost nothing — `HandsProbe` already reads
`CurrentAbility` and `CurrentHoldable` every frame, and both null is empty hands.


**M7 — scripted-event QOL, and the arc's longest bug is fixed (2026-08-10).**
Verified in the headset across several cycles. What works now:

- **Scripted sequences land where they intend.** Neither head look nor the right
  stick influences the walking direction any more. That coupling
  (`Controller.Rotation` drives view, weapon trace *and* walk direction) is the
  longest-standing bug in this project.
- **The scripted camera turns the player again**, and the right stick still
  works during a sequence — view-only, via `g_aimBase`, never the aim field.
- **Arms and hands appear only while the rig is actually animating.** Tester
  verdict: the Little Sister crawl "works perfectly now", unhiding for the bottle
  catch and hidden for the rest; the plasmid scene "pretty much perfect".

Two signals carried it, both Tier 0 and both measured:
`hands+0x594` bit 2 (`CurrentlyExecutingScriptedHandAnimationSequence`) and
rig **motion** — after `bFinishedStateAnimations` was tried and falsified.

**Still open from that arc:** the entry stall (`bIsForcingPlayerMove`, now
located at `controller+0x9E0`), and the balcony fall not rotating the player,
noted as minor.


**M3-S1 — the native accessors are LOCATED. Passed in one cycle, 2026-08-10.**

```
>>> NATIVE: GetPropertyTextByName @ 0x102D46E0 (rva 0x7346E0)
            prologue 55 8B EC 83 E4 F8 83 EC  [slot 1]
>>> NATIVE: S3 stride 12 measured, 8 of 8 neighbours are rows
>>> NATIVE: 4 of 4 accessors located, 4 distinct addresses
```

Two launches, byte-identical offsets. All four accessors landed within `0x300`
bytes of each other (one translation unit), each with an MSVC prologue and `CC`
padding, each on an executable page. Health normal both runs.

**The mechanism generalises and that is the real result.** Natives are registered
by an `int<Class>exec<Func>` wide string in `.rdata`; exactly one 12-byte row in
`.data` points at it, and the row's second DWORD is the function pointer. Now an
invariant in `docs/INVARIANTS.md` § *Engine access* — **any** registered native
is reachable this way. The scan was found by static analysis of the shipped
executables before any code was written, which also proved the row sits at a
**different RVA on Steam (`0x11BE684`) than on Epic (`0x11BD6B4`)**.

**The one real risk resolved yes:** the row's function slots are zero on disk and
are written at runtime. No backoff retry was needed.

**Not yet proven:** that it can be *called*. That is S2, and the signature is not
what S2's card assumed — see *Next step*.

**Research: roomscale, handedness, detached hands, QOL — 2026-08-10.**
Desk research only, nothing headset-tested. `docs/proposals/vr-features-research.md`.
The finding that shapes it: four requested features need **two** enablers, not
four. Handedness, detached hands and two-handed grip are all one mechanism (a
rigid bone-cluster transform on the array `ArmHide` already writes to), and
**roomscale is downstream of M3-S2** — it needs `AActor::Move`, whose symbol is
confirmed present in the exe.

**`ExcorcisingGatherer` was missing from `kContexts` — fixed.** Diffed the mod's
table against the game's own 30-entry `Contexts=` list in `User.ini`. Exactly one
gap, and it was the **Little Sister rescue** — the sequence M3-S3 exists to
detect. The game misspells it (`Excorcising`), so adding it from memory or from
the corpus would silently never match. Classified `CTX_SCRIPTED` by inference;
confirm against a logged value once the context read works. **Compiled, not
deployed, never run.**

**The duplicate-world square — FIXED.** A textured full-screen quad was landing
in the HUD capture's one-draw-per-frame slot, and the alpha repair forced it
opaque. The interface is untextured GameSWF geometry; the square was textured.
One term in the redirect condition (grep `PSSrv0Res`):

```cpp
PSSrv0Res(ctx) == nullptr
```

Works in every scene with no timers, gates or cutscene detection. **Four
timing-based approaches failed first** — see `docs/INVARIANTS.md`. It was solved
in one play session by a throttled log line printing the captured draw's
signature, after three sessions of theory.

**The console read channel — OPEN, with a known ceiling.** Per-slot output-device
thunks plus returning `1` from slot 4 (the "will you accept output?" query) make
`get` return values. `get ShockPlayer bReticleDisabled -> [True]`.

**Its ceiling is proven:** `get` reads the **class default object**, not live
state. `Health` stayed `200.0` after damage; `Location` stayed `(0,0,0)` after
walking. Useful for defaults and for confirming `set`; useless as a state source.

**M1-S1 — `myHUD` PINNED, and the PlayerController layout with it.**
`PlayerController.myHUD = controller+0x71C`, `HUD.PlayerOwner = myHUD+0x470`,
the six-bool DWORD at `myHUD+0x490` with `bHideHUD` as bit 0. Confirmed by a
self-validating back-reference on two controllers, stable over ten samples each,
and nine consecutive HUD fields matched their declared types on inspection.
Declaration-order arithmetic predicted `+0x710` and passed through seven
independent `ENGINE-MAP` anchors; the `+0xC` gap was `FMatrix` being 16-byte
aligned, so **every `PlayerController` field after `FixedRotation` shifts `+0xC`
from a naive walk** — recorded in `docs/ENGINE-MAP.md`.

**M1-S2 — `bHideHUD` FALSIFIED. The pivotal test of the arc returned no.**
The DWORD read `0x00000020` and never changed: 16 minutes, four controller
lifetimes, the bathysphere descent, a level load, the plasmid injection, combat,
barrels, both halves of a Little Sister sequence. **Eight marked boundaries,
zero transitions at any of them.** Decisive: the tester made the HUD visibly
appear and disappear by stepping in and out of the bathysphere entrance and the
DWORD did not move. So HUD visibility here is not driven by `bHideHUD` —
`HideMovie('HUD')` on the Scaleform controller is the new suspect.

**Two offsets renamed, free from S1's arithmetic.** `+0x5C0`/`+0x5C8` are
`PlayerController.aForward`/`aStrafe` — the **raw input axes**, which is *why*
the old measurement read `778–875` while pinned in a corner and provably not
moving. It remains an input-*requested* signal that geometry cannot false-fire.
And `+0x620` is `ViewTarget`, not a Pawn alias — which is why grave 2 (ViewActor
divergence) watched it track the pawn for whole sessions.

**`Core/Keybinds.cpp` is dead code.** `Key_Init` has **no caller anywhere**, so
every binding resolves to VK 0 and `Key_Down`/`Key_Fired` always return false.
Found the hard way: the M1-S2 marker key was routed through it and produced zero
marks for a whole run. Bind with `GetAsyncKeyState` until it is wired.

---

## Open — P1

### Cutscene / scripted-event detection
The longest-running problem. Everything downstream is built and waiting.

`GameState_Cutscene()` is inert: `g_cutscene` is written only by the demoted
ViewTarget path and by non-latching pitch telemetry. The context scan brackets
the right window and has **never locked** — zero `>>> CONTEXT` lines, ever.

Graveyard (evidence in `docs/INVARIANTS.md`): **`myHUD.bHideHUD`** · ViewActor
divergence · pitch-rate latch · pitch servo · S75/S78/S79 unwind · cached
view-target scans · console `get` · the input-ignored detector. **Nine now.**

Live leads, best first:
1. **The native property call (Tier 1)** — `Object::GetPropertyTextByName`, which
   retail script calls on live instances. **This is M3 and it is the main line.**
   **Located as of M3-S1** (rva `0x7346E0` on Steam, stable). What remains is
   invoking it: it is an `exec` native taking an `FFrame`, so it must be called
   through a constructed bytecode frame and validated on `Health` before anything
   trusts it. It can crash. That is M3-S2.
2. **`ShockPlayerController.LastPlayerInputContext`** — the *controller* copy,
   never examined; every scan so far was on the pawn. M3-S3. It is also the only
   lead that covers the Little Sister rescue, which pushes `NullInput` and never
   enters cinematic mode at all.
3. **`HideMovie('HUD')` on the Scaleform GUI controller** — new from M1-S2. The
   HUD demonstrably hides without `bHideHUD` moving, so something else is doing
   it, and `ShockPlayer.uc` calls exactly this during the rescue. Unexplored.
4. **`CurrentExorcismTarget`** — brackets the rescue only. The probe exists; the
   run that produced candidates contained no rescue and had a stale-snapshot bug
   on pawn change. `+0xEA4` and `+0xB58` survive. Superseded by lead 2, which
   costs the same and covers more.
5. **The UE2 metadata walk (Tier 2)** — `docs/proposals/ue2-reflection-bridge.md`.
   Parked; opens only if M3 fails.

### Aim / movement coupling
`Controller.Rotation` (`+0x1E4`) drives the view, the weapon trace **and** the
walk direction. `AimSource=1` therefore makes the character walk wherever the
controller points. `AimSource=2` cannot work; the body-follow servo did not feel
right; `ModYaw` breaks scripted movement.

**The unmeasured question that gates everything:** does the firing trace read
`Controller.Rotation`, or the weapon socket? If the socket, the seam exists.
Settle this before designing anything else here.

### Rotation comfort — parked
`ModYaw` and `FreezeGameRotation` are compiled and **default 0**. `ModYaw=1`
zeroes `sThumbRX`, which freezes `Controller.Rotation`, which forced-move
sequences steer by — the opening bathysphere walks the player into the back wall
and the projector never plays. `FreezeGameRotation` requires `ModYaw`, so both
are inert. Keep them; do not delete.

---

## Open — P2

- **`AnchorIndexCounts` is empty**, so the "What is this?" context-help screen
  does not resize with the rest of the HUD. Fill it the way the square was found:
  bring the screen up, Numpad `*` to clear, Numpad `3` to dump, take the signature
  that only appears while it is up.
- **`HeadAimMode=2` starves scripted pitch gates.** The plasmid injection scene
  waits for the view to pitch down at the syringe and hangs forever. `=1` clears
  it. Reproducible.
- **`dxgiproxy` had never built.** It compiled both `dxgi_proxy.cpp` and a
  leftover Visual Studio wizard `dllmain.cpp`, each defining `DllMain` (LNK2005).
  Template deleted 2026-08-09; all three projects now build. Worth remembering
  that the dead-code audit only covered `BioshockVR/`.
- **`Keybinds.cpp` is never initialised — CONFIRMED, and it bit us.** `Key_Init`
  has zero callers, so the whole module is inert and `[KEYS]` is read by nothing.
  Every working hotkey calls `GetAsyncKeyState` directly, which means the
  collisions the header claims to have "corrected" are all still live:
  `VK_PRIOR` three readers, `VK_DELETE` two, `VK_NEXT` two, `VK_NUMPAD9` two.
  Wiring it fixes all four plus the 103-VK per-frame sweep in `PollFovKeys`, and
  ships rebinding for users with no numpad. `dist/BioshockVR.ini` now documents
  the real compiled-in keys and says plainly that rebinding needs a code fix —
  **a `[KEYS]` section was deliberately NOT added**, because nothing would read
  it. See `DECISIONS.md`.
- **Keep the `KEY: vk` keylogger in `PollFovKeys`.** It is the sweep above and it
  looks like pure noise, but it is the only reason M1-S2's eight marker presses
  were recoverable after the marker key turned out to be dead. If the sweep is
  optimised, keep a logging path for it.
- **Plasmid hand, first equip.** On the first plasmid pull of a session it bound
  to the *right* hand instead of the left, and both plasmids showed the
  right-hand model. Cycling weapons cleared it and it did not recur. Observed
  2026-08-09 on the post-refactor build; **almost certainly pre-existing** —
  the refactor was verified semantically identical, and `ControllerLayout` (its
  one changed default) governs button layout, not hands. Smells like probe
  ordering on first equip. Low priority, but reproduce before touching
  `HandsProbe` for any other reason.
- **Index binding hazard**: `menu` (trackpad click) and `rest_l` (trackpad touch)
  share the left trackpad, so a menu press always arrives with the modifier held.
  With `DpadFlip=1` or `DpadModifier=4` that turns pause into context-help.
  Predicted from the JSON, unconfirmed on hardware.
- **WMR binding hazard**: grip bound as a digital button to an analog action
  thresholded at 0.80. Likely symptom on a Reverb G2 is no radial at all.
  Provisional profile, never hardware-tested.
- **Physical wrench** implemented but never conclusively validated.
- **Alternate-build lifecycle**: clear cached pointers immediately on pawn-null.
  GOG delta signature finds zero matches; Epic/GOG reticle Exec uses
  Steam-specific engine addresses.

---

## In flight

**M3 — the native call bridge.** `.planning/sessions/M3.md`.

- [x] **M3-S1** locate — passed 2026-08-10, one cycle. `Game/EngineBridge.cpp`.
- [ ] **M3-S2** call it on `Health`. The pivotal test of the milestone and the
      first session in the arc that can crash the game. **Its card is stale in
      one respect and now carries a ⚠ box saying so** — there is no direct
      `obj->GetPropertyTextByName('Health')` to call.
- [ ] **M3-S3** the controller's `LastPlayerInputContext`. Skip entirely if S2
      returns no.

**Committed.** That earlier claim was stale — the 2026-08-10 M3-S1 and research
work is in the history, and M6-S1 landed on top of it.

---

Repo repair and refactor prep — plan at
`~/.claude/plans/c-users-raywi-downloads-downloads-biosh-virtual-flamingo.md`.

- [x] Git repaired: `main` = `002a81a`, old lineage preserved as
      `archive/parked-experiments` (local + remote).
- [x] `.gitignore` no longer hides every `.md`; `dist/` now tracks the installer
      scripts and the shipping INI.
- [x] Doc system built.
- [x] **3a** — deleted 1,911 lines of uncompiled shim duplicates (including a
      stale `shim_main.cpp` still carrying the pre-fix SteamVR warp conversion);
      moved `BioshockVR/OpenXRShim/*` → `OpenXRShim/src/*`.
- [x] **3b** — `Config.h`/`Config.cpp`. 138 globals and 161 duplicated `extern`
      lines became one struct. `dllmain.cpp` 909 → 297 lines.
- [x] **3b verified in a headset** — config echo **byte-identical** to the
      pre-refactor baseline, all 90 lines.
- [x] **3c** — reorganised into subsystem folders (`Core/ Render/ Hud/ Camera/
      Input/ Hands/ Game/`) mapping 1:1 to `docs/modules/*`, with
      folder-qualified includes. **The planned file SPLIT was abandoned on
      measurement** — see `DECISIONS.md`. Comment audit: orphaned fragments from
      3b removed, four stale claims corrected, all 27 path headers uniform.

No behaviour changes except the `ControllerLayout` default, logged in
`DECISIONS.md`.

### Refactor prep is complete

Housekeeping since: all logs now land in `Build\Final\logs\` (the loader
breadcrumb was the last one outside it); `Bioshock-Remastered-VR.slnx` and
`deploy.bat.example` removed; `docs/STYLE.md` written; `README.md` corrected;
`/newchat`, `/readme` and `/codemap` in place. Docs no longer cite line numbers
— they rot, and this session proved it when the square fix moved from 1425 to
1404. They cite greppable anchors instead.

Both projects rebuild clean, `Release|Win32`, with only the two pre-existing
`C4244` warnings in `CameraHook.cpp` (confirmed pre-existing by a clean rebuild
of the pre-refactor tree in a throwaway worktree).

3b was verified two ways. Statically: across all 114 config reads exactly one
line differs from before — the intentional `ControllerLayout` fix — and all 72
echo lines are byte-identical. At runtime: the `=== BioshockVR config ===` block
from a real run is **byte-identical to the pre-refactor baseline**, all 90 lines.

3c changed no code at all — file moves, include paths and comments — so it
carries no behavioural risk beyond the build itself, which is clean.

**The post-3c build has now run in a headset** — several times, across M1-S1 and
M1-S2 on 2026-08-09, including a full new game, a level load, a save reload and
~16 minutes of play. No regression reported and no crash. The refactor is
settled; the rollback copy is no longer needed.

---

## Last verified in a headset

**2026-08-10 21:38, build `crosshair in scenes + unarmed head aim`.** A full run
through the opening, then a second pass on the Big Daddy scene with
`ControllableScriptedFix=1`.

**Verified:** the crosshair is gone with empty hands and gone in scripted scenes;
the Big Daddy scene plays normally with the fix on; the free hand still tracks
and the two-handed weapons are still untouched.

**Assumed, NOT verified, in this same build:** the free hand for **plasmids**
(the right-cluster write, including the bone 43 position write) and the
**telekinesis release** test that goes with it. Both were asked for and neither
was reached.

**Found by accident, and it turned into a feature:** head aim was driving
locomotion, hard — *"turning like 90 degrees left almost moves you backwards and
to the left."* That is not sensitivity, it is the head yaw applied **twice**: the
aim field carries it, and `HeadRelativeMove` rotates the movement stick by it
again. 90 twice is 180. Fixed in the next build via `MovementMode`, **untested**.

**Also reported, with a side-by-side screenshot:** the tonic equip screen. The
game's own render shows the green panels **completely empty**, so the capture is
working perfectly and only the placement is wrong — that interface is spatially
bound to world geometry and rides the flat quad instead. Plus an opaque black
"Waiting…" box absent from the game's render. Both on the M6-S5 card.

**2026-08-10 19:19, build `M6-S1 left hand tuning modes`.** The tracked left
hand, tuned live and signed off: *"Looks and feels fantastic. Works perfectly,
even in the scripted events."* Two-handed weapons visibly unchanged, scripted
events unaffected.

Health across the whole run: `EYEQ 1/1`, `hud: host found` climbing normally,
`POLL synth ~186/s realpad 0`, game thread **4.2–5.0 ms** and **~90 frames/s
submitted**, steady for two minutes with the cluster write live. That last pair
is the answer to a reported framerate drop: the mod's own numbers show no
regression, and the two real costs — the palm spike's overdraw and ~400 log lines
per Numpad 9 press — were both fixed rather than argued about.

**2026-08-10 17:14, build `M7-S6 forced-move gate + freeze on` (15:36:35).**
Bathysphere ride, two scripted sequences, ordinary play. Tester verdict:
**"Perfect on every front."**

The log carries the evidence, not just the verdict:

```
17:16:58.589  FORCEDMOVE: the game is moving the player -- aim released
17:16:59.545  FORCEDMOVE: done
17:16:59.559  SCRIPTED ANIMATION BEGAN
```

**14 ms between the forced move ending and the scripted animation starting** —
the two windows hand off seamlessly, which is the entry stall closed. The
bathysphere boarding was bracketed the same way (`17:15:17.6 → 17:15:18.9`,
immediately followed by `BATHYSPHERE MODE ON`).

Config echo confirms all three switches live: `ScriptedEventQol 1`,
`FreezeGameplayRotation 1`, `ScriptedRotationFollow 1`. Health normal —
`EYEQ depth min=1 max=1`, `hud: host found 35887`, `POLL synth 132/s realpad 0`.

**Noticed and not chased:** the bathysphere read hit a **stale pawn pointer**
twice across two runs, once as `0x32313936` and once as `0x5F333739` — both
ASCII, both during a level transition, which is exactly when a bathysphere ride
ends. The oracle gate caught the first (false ON); the second produced a
harmless early "mode off". **A cheap hardening is available and not yet done:**
that DWORD holds three bools, so any value `> 7` is garbage by construction.
Add that shape check next time the file is open.



**2026-08-10, build `M3-S1 native locate` (Aug 10 2026 00:31:20).** Two runs,
load a save and stand still. Purpose was the `>>> NATIVE:` block and nothing
else; **no presentation route was walked**, so this session says nothing about
HUD, hands or comfort.

Health correct in both: `EYEQ: depth min=1 max=1` · `hud: host found 2738` ·
`POLL: getState 105/s synth 105/s realpad 0/s hook=ON xr=ON`. No errors, no
crash. The scan cost 0–15 ms and ran exactly once per run.

**Noticed and not chased:** the module loaded at the same base (`0x0FBA0000`)
both runs, so relaunching did not stress relocation — Windows randomizes an
EXE's base per boot, not per launch. It *is* relocated from its preferred
`0x10900000`, so the scan did resolve in a moved image; a reboot would be the
cleaner test and is not worth a cycle.

> **The DLL in the game folder is the M3-S1 build and is the one that was
> verified.** `Release/BioshockVR.dll` is one commit ahead of it — it also
> carries the `ExcorcisingGatherer` context entry, which compiles but has never
> run. **The next session must build and deploy before testing anything.**

**2026-08-09, build `M1-S2 cinematic flag` (23:19:49).** New game → intro
bathysphere → level load → out of the bathysphere → plasmid injection → combat
and barrels → load a save → both halves of a Little Sister sequence. ~16 minutes.

Health signals all correct: `>>> XR: runtime = OpenVR/SteamVR via BioshockVR
shim` · `EYEQ: depth min=1 max=1` · `hud: host found 30718 frames` ·
`POLL: synth 234/s realpad 0/s hook=ON xr=ON`. No errors, no warnings, no
crash, and the `myHUD` pointer re-resolved cleanly across all four controller
changes.

**Noticed and not chased**, all reported as long-standing rather than new:
- The **vita chamber "What is this?" prompt is head-attached, not world-anchored.**
  Related to the empty `AnchorIndexCounts` in P2 above.
- **HUD behaviour in the first bathysphere is generally odd** — absent on
  regaining control, appearing only on walking out, and toggling on stepping in
  and out of the entrance. That last one is what falsified `bHideHUD`, so it is
  evidence rather than only a bug.

**Still open from the previous headset session:** the ammo counter reads slightly
more transparent since the square fix. `HudAlphaFix=0` did not change it, so it
is not the alpha repair. Candidate: the counter is itself a textured draw now
excluded by the guard, putting it on the backbuffer instead of the quad. The
`CAPTURED:` diagnostic would answer it in one run.
