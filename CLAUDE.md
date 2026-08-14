# BioShock Remastered VR — working notes for Claude

Native VR mod for BioShock Remastered (`BioshockHD.exe`, 32-bit, D3D11, UE2.5
fork with no reflection system). Everything here was found by scanning and
measuring, not read out of an SDK.

## Read this before touching code — the context ladder

> **Uncertainty is a signal to load more context, not to guess.** Reading a doc
> costs tokens. Guessing costs a headset cycle, and this project's entire
> graveyard is made of confident guesses. When in doubt, escalate and say you did.

**Rung 1 — always.** This file (it auto-loads, so it is free) plus your session
card: `.planning/sessions/M<n>.md`, the section for your session. The card is your
task. `.planning/ROADMAP.md` says which one is next.

> **`.planning/` is git-ignored, and if you cloned this repo you will not have
> it.** That is deliberate: the planning documents describe features well before
> they exist, and this project announces a feature when it works rather than when
> it is designed. Nothing is withheld from the record — every measurement
> graduates into `docs/` in the commit that uses it, which is where you would look
> for it anyway. **`docs/` is the published knowledge base and is complete on its
> own.** Working from a clone, start at `docs/CODEMAP.md`.

**Rung 2 — the default for any code session.** Plus the `docs/modules/*.md` the
card names, the `docs/INVARIANTS.md` section it names, and `.planning/STATE.md`
§ *Next step*. A session that reads Rung 2 and makes an anchored edit is behaving
correctly and needs no permission to go further.

**Rung 3 — escalate freely.** No asking, no apology; just note which rung you went
to and why.

| When | Load |
|---|---|
| Touching an offset, address or pattern scan | `docs/ENGINE-MAP.md` + `engine-offset` skill |
| An anchor isn't where the doc says, or a doc contradicts the source | The module doc, then the source region — **and report the drift** |
| An idea occurs that the card didn't specify | Full `docs/INVARIANTS.md`, both lists, before proposing it |
| Cross-module work, or you can't tell which module owns something | `docs/CODEMAP.md` |
| Writing more than a few lines of new code | `docs/STYLE.md` — no auto-formatter, deliberately; never bulk-reformat |
| A setting seems inert | `ini-check` skill + `docs/modules/config.md` |
| Reading a log or diagnosing a play session | `log-triage` skill |
| "What does the game actually do here?" | `docs/UNREALSCRIPT.md`, then grep `research/uscript/` |
| Two rungs in and still unsure about behaviour | Read the source region |

**Rung 4 — subagents. Only when the arc has genuinely dead-ended** and a fan-out
search across many files would actually help. A cold subagent re-derives context
you already hold; it is the most expensive move available here.

Avoid whole-file reads over ~400 lines — `CameraHook.cpp` (3759) and
`DrawHook.cpp` (2161) are ~60k tokens together. Grep the banner anchor and read a
window. If the window genuinely isn't enough, read more; under-reading is worse.

**Current arc:** live state → cutscene detection → QOL. Findings and design in
**`docs/ARCHITECTURE.md`**. The long-held premise that this fork has no usable
reflection is **false** — read it before proposing anything about reading engine
state.

## The graveyard — check this before proposing anything

One line each; evidence in `docs/INVARIANTS.md` § *Falsified*. **Every one of these
is plausible enough to be proposed again — that is why it is here.** It lives in
this file so the check costs nothing.

*Cutscene detection:*
1. **`myHUD.bHideHUD`** — the offsets are right (`controller+0x71C`, bool DWORD `+0x490` bit 0, back-reference confirmed). The DWORD **never changed once** in 16 minutes across bathysphere, plasmid, rescue and combat — and did not move even while the HUD visibly appeared and disappeared. Suspect `HideMovie('HUD')` instead.
2. **ViewActor divergence** — never leaves the pawn; `+0x450`/`+0x620`/`+0x914` track it for whole sessions.
3. **Pitch-rate latch** — latched during ordinary combat for four straight seconds.
4. **Pitch servo** — runaway feedback loop; froze the view and the hand.
5. **S75/S78/S79 render-side unwind** — made scripted sequences worse.
6. **Cached view-target scans** — no signal.
7. **`LastPlayerInputContext` on the pawn** — window correct, has *never* locked. (The **controller** copy is untried — `docs/ARCHITECTURE.md` finding 3.)
8. **Console `get`** — returns the class default object, not live state.
9. **Input-ignored detector** — sound, but needs the player to push the stick, so it is silent when a cutscene starts standing still.

*Aim and movement:*
10. **`AimSource=2`** cannot exist — the game's heading freezes permanently.
11. **Body-follow yaw servo** — ported from the reference mod; did not feel right.
12. **`ModYaw` alone** — zeroing `sThumbRX` freezes `Controller.Rotation`, which forced-move sequences steer by. The opening bathysphere walks into the back wall.
13. **The coupling is structural — TRUE OF AIM ONLY.** `Controller.Rotation` (`+0x1E4`) drives view, weapon trace *and* walk direction, and no arrangement of that one field separates the first two. **Locomotion was always separable** and is now solved: rotate the movement *stick*, never the field. Do not cite entry 13 against a locomotion idea.

*Walking:*
14. **Refining the stick rotation** to remove the residual walk drift. `R` was algebraically exact from the first attempt; the distortion was the game's own **square (per-axis) deadzone of 0.225** applied after the value left us, reproduced seven for seven. Fixed by pre-compensation. **When a correction is provably exact and the symptom survives, go and measure what the other side received.**
15. **`WalkFromPawnYaw`** — the pawn's rotator tracks the aim field exactly, 60 of 62 samples at `+0.0` under a held 76° offset.

*Scripted scenes:*
16. **Substituting a heading into the aim field as a scripted window opens.** A forced move steers by **nothing of ours** — three balcony falls entered far right, straight on and far left all landed on the same spot. With the substitution on, both straight-on runs landed badly wrong. **The write itself is the damage.** Never write `Controller.Rotation` while a sequence is moving the player, and never let the window break mid-scene: a per-frame "are you still in control" predicate over the HUD did exactly that and threw one landing 3.7 m. `docs/INVARIANTS.md` § *Locomotion and the aim field*.
17. **Following the aim field AND the game's camera during a scripted window.** They are not independent — the balcony's opening snap moves both by `41.03 deg/s`, so it applied twice and the view finished a whole snap past the authored heading. The camera is downstream; follow it alone. **The measurement that justified following both was a deg/s average over 67 seconds, and a rate cannot see a one-frame spike.**

*HUD:* the scene-sampling leak guard, `DrawHook_NoWorldRender()`,
`g_gameplayConfirmed`, `MenuMaxIndexed=0` — all dead. The square was solved by one
term: `PSSrv0Res(ctx) == nullptr` (the interface is untextured; the square was
textured). **Do not disturb it.**

## Build

Two projects, both **`Release | Win32`**. Win32 is mandatory — the game is 32-bit.

```bash
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" BioshockVR/BioshockVR.vcxproj -p:Configuration=Release -p:Platform=Win32 "-p:SolutionDir=C:\dev\Bioshock-Remastered-VR\" -v:minimal
```

**`-p:SolutionDir` is required** when building a `.vcxproj` directly. Both
projects resolve `third_party\` and `packages\` through `$(SolutionDir)`, which
is empty outside a solution build — the result is
`Cannot open include file: 'MinHook.h'`. The trailing backslash matters.

- `BioshockVR.vcxproj` → `Release/BioshockVR.dll`, the mod.
- `OpenXRShim/OpenXRShim.vcxproj` → `Release/openxr_loader.dll`, the SteamVR shim.
  Sources are in `OpenXRShim/src/`.
  **`Release|Win32` is the only config that produces a DLL.** The other three are
  `ConfigurationType=Application` and silently build an `.exe`.
- `/MT` runtime, so there is no VC++ redistributable dependency. Keep it.
- The shim must never be linked into `BioshockVR.dll`.

There is no deploy script. Claude copies the built DLL into the game folder
directly (see Test loop below) — it is writable without elevation. The DLL goes
**directly beside `BioshockHD.exe`**; there is no `BioPlugins` folder.

A third project, `dxgiproxy/dxgiproxy.vcxproj` → `Release/dxgi.dll`, is the
loader that pulls the mod in. It rarely changes. `Release|Win32` is the only
config with `ModuleDefinitionFile` set, so the other three build a DLL that
exports nothing the game looks for.

## Test loop

**Claude builds and deploys. The user only launches the game.**

1. Build both projects as above.
2. **Copy `Release/BioshockVR.dll` into the game folder yourself** — it is
   writable without elevation. Do not ask the user to run a deploy script.
   Back up the existing DLL first if the change is structural.
3. Tell the user what to do in the headset and what to look for.
4. **Read `…\Build\Final\logs\BioshockVR.log` directly** — do not ask them to
   paste it. The mod truncates it at startup, so it always contains one run.

The game must be closed before copying. There is no test suite and no way to
automate the verification step itself; a human has to put the headset on.

**Check the build stamp first, every time.** A stale DLL has invalidated seven
sessions of this project. `dllmain build: …` in the log must have advanced.

Then: `>>> XR: runtime =` correct · `EYEQ: depth min=1 max=1` · `hud: host found`
nonzero · `POLL:` showing `synth` high and `realpad 0`.

Full route when a change could touch presentation:
main menu → load → combat → crate → vending machine → **Little Sister rescue** →
pause. The rescue is the reproducible trigger for the HUD square.

## How to work here

- **NO DASHES IN ANYTHING A USER READS.** No em dash, no en dash, and no ` -- `
  standing in for one. That covers everything in `dist/`, release notes, the
  READMEs, changelogs and Discord posts. Rewrite the sentence instead: a comma, a
  colon, brackets, or two sentences. **The source tree is exempt** and keeps its
  ` -- ` house style; `docs/` follows the source.
  Applies to material written or edited from 2026-08-13 on, so existing comments
  in `BioshockVR.ini` are not a cleanup task.
- **Claude edits the files.** Do the work with the Edit/Write tools — never hand
  the user a diff or a "paste this into Visual Studio" instruction. They do not
  apply edits by hand any more. Claude edits, builds, deploys and reads the log;
  the user launches the game and reports what they saw.
- **Still no line numbers**, in docs or in commentary. They rot — the square fix
  moved from `DrawHook.cpp:1425` to `:1404` in a single commit. Cite greppable
  anchors: banner text (`the classifier`, `ALPHA REPAIR`) or code
  (`PSSrv0Res(ctx) == nullptr`).
- **One change per test cycle.** Build, deploy, run, read the log, then continue.
  Batched changes cannot be attributed when something breaks.
- **Say whether a change is diagnostic**, so the tester knows whether to expect a
  visible difference or only a log line.
- **Prefer an INI switch to a rebuild.** New behaviour ships default-off.
- **Measure before theorising.** Every long dead end in this project came from
  reasoning about what the code should do instead of logging what it did. The
  HUD square was solved in one run by a throttled log line after three sessions
  of theory.
- **Fail closed.** Verify a hook's prologue, an object's identity, a vtable's
  value before writing. A refused feature beats corrupted game state.
- **Never add a per-frame memory scan.** One-shot scans that stop after locking
  are fine; use backoff for retries.
- **Retire disproved theories explicitly** in `.planning/DECISIONS.md`, with the
  evidence that killed them. Otherwise they come back.

**The user is not a programmer.** Do not ask them to read code, choose between
implementations, or diagnose a compiler error. Ask them only what they can
actually answer: what they saw in the headset, what they heard, what they did.
Everything else is Claude's job, including deciding when a change is too risky
to make.

## Config

~130 settings live in `BioshockVR.ini` next to the DLL. The shipping default is
`dist/BioshockVR.ini`. **The startup config echo in the log is the authority** —
not the file in the editor. Read-only files, wrong storefront profiles, and
VirtualStore redirection have all silently defeated edits here.
