# Log playbook — reading a bug report from someone else's machine

**Tracked and published on purpose.** The bundles it is derived from live in
`research/logs/`, which is gitignored because reports carry usernames and machine
details. This file is the part that outlives them, so it ships with a clone.

This is the knowledge half of `/debuglogs`. `log-triage` tells you how to read
**one** log correctly; this file is about **patterns across setups** — what a
symptom usually means, what has actually fixed it, and which signatures lie.

> **The most important thing on this page.** Several real failures in this mod
> produce a log where **every documented health signal reads green**. `EYEQ depth
> min=1 max=1`, `hud: host found` climbing, `POLL: synth high, realpad 0` — all
> present, all normal, and the game unplayable. **A healthy-looking log is not
> evidence that anything works.** The entries below marked **LIES GREEN** are the
> ones where that happens.

---

## Read in this order

`log-triage` has the full table. The short version, and you stop at the first
thing that is wrong because everything after it is meaningless:

1. `dllmain build:` — which build is this? A stale DLL has invalidated whole
   sessions here.
2. The config echo — **the authority on what the mod actually read.** If a
   setting is not in that block, it did not take.
3. `>>> INPUT: bound profile (left/right) =` and `bindings resolved N of M` —
   what hardware, and did the controls resolve.
4. `>>> XR: runtime =` — which loader and runtime.
5. `EYEQ`, `hud: host found`, `POLL:` — the health lines.
6. Then the symptom-specific section below.

---

## Symptom → cause → fix

### The controller does nothing, and the log looks perfect · **LIES GREEN**

**Signature:** `bindings resolved 0 of 15`, or a census line with `NO` against
controls the hardware certainly has. Everything else reads normal.

**Cause.** An unbound action is not an error anywhere in the stack: the runtime
returns success with `isActive == false`, every getter collapses that to zero,
and the pad publishes neutral. Before the census existed there was no way to see
this at all.

**Fix.** Read `bound profile` — if it says `NONE`, the runtime bound nothing. If
it names a profile the mod does not suggest, that controller family needs a table
in `InputHook.cpp`. If it says `simple (fallback)`, the user is on the menu tier:
no sticks, grips or triggers.

### Black or frozen headset, log completely normal · **LIES GREEN**

Two distinct causes, and they are told apart by one line each.

- **Adapter mismatch.** Look for the LUID compare line. On a hybrid laptop the
  game can land on a different GPU from the headset. Under the shim the only
  other symptom is a single rate-limited compositor error in `openxr_shim.log`.
- **Resolution changed mid-session.** The eye swapchains are sized once from the
  first frame; changing resolution in the game's own options leaves the copy
  mismatched and silently dropped. Ask whether they changed resolution — the
  shipped ini actively encourages lowering it for framerate.

### VR never starts, game runs flat on the monitor

**Signature:** `!!! XR_Init FAILED. Running flat.`

**Cause, most often.** The game launches *before* the runtime finishes starting —
measured in this project. A cold SteamVR start routinely takes longer than the
game takes to reach its first frame.

**Fix.** Start the runtime first and relaunch. If it recurs on a machine where the
runtime was already up, read the `xrCreateInstance` failure line, which now names
the result code and which loader is active.

### The game will not launch at all

**Signature:** `FAIL: BioshockVR.dll not found beside dxgi.dll` in
`BioshockVR_loader.log`, and **no `BioshockVR.log` from this run at all**.

**That message is a lie by omission — it fires on any load failure, not a missing
file.** The `GetLastError()` value distinguishes them:

| Code | Meaning |
|---|---|
| 2 | genuinely missing file |
| 126 | a dependency is missing — usually `openxr_loader.dll` or `openvr_api.dll` |
| 127 | a **missing export** — the mod calls an `xr*` function the shim lacks |
| 193 | wrong bitness — a 64-bit DLL in a 32-bit game |

**127 is a mod bug, not a user bug.** It has happened twice here and is caught
before release by comparing `dumpbin /imports BioshockVR.dll` against
`dumpbin /exports openxr_loader.dll`.

### Settings do not stick, tuning is lost on exit

**Cause: VirtualStore.** On a Program Files install without elevation, Windows
silently redirects the mod's writes to
`%LOCALAPPDATA%\VirtualStore\Program Files (x86)\...`. The open *succeeds*, the
file exists, and the user never finds it.

**Look for** the log's relocation note, and check whether the bundle contains a
VirtualStore copy of the ini. The mod already detects this for its log
(`DirTakesOurLog`) — the same technique applies to the ini.

### Cannot walk at all, on an Index / Beyond / Varjo / Somnium

**Cause.** `ControllerDpadModifier=1` resolves to the right **trackpad touch** on
those headsets, and the modifier is *held*, not pulsed. A thumb resting where the
hardware expects it to rest means the movement branch never runs.

**Fix:** `ControllerDpadModifier=2`. Setup now writes this for those headsets, so
a report showing mode 1 on an Index means Setup was not run or was overridden.

### No radial, no weapon wheel, no two-handed grip

**Cause.** The grip did not resolve. Check the census for `grip_l`/`grip_r`.
Vive wands and WMR have a **digital** grip and need `squeeze/click`, not
`squeeze/value` — binding the analog path on that hardware yields a permanent
zero against a 0.80 threshold.

---

## Confirmed fixes

Append here when a fix is **verified on the reporter's machine**, with the
evidence that proved it. Same discipline as `.planning/DECISIONS.md`: a fix
without evidence is a guess that will be re-proposed.

| Date | Symptom | Setup | Fix | Evidence |
|---|---|---|---|---|
| — | *(none yet — this file ships with the release)* | | | |

---

## What a good report contains

If a bundle is missing these, ask for them before spending time on theories:

- `BioshockVR.log` from the failing run — **and note the mod truncates it at
  startup**, so a relaunch destroys the evidence. Ask them not to relaunch first.
- `BioshockVR_loader.log` — the only file that exists when the mod fails to load.
- `openxr_shim.log` — only on the SteamVR path; **absent is not evidence of
  failure** on the standard loader.
- `Bioshock.ini` — the game's own config, which Setup modifies.
- `setup.log`.
