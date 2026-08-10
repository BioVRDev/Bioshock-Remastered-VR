---
description: Audit README.md against what the project actually is now, and update what has drifted.
---

`README.md` is the only document written for **users**, not for us. Everything
else in `docs/` assumes the reader is working on the mod; this one assumes they
just want to play it. That difference governs every edit here.

Run this when the packaged files, the install steps, the tuning keys or the
feature set have changed — not on a schedule. `/newchat` flags when it is due.

## 1. Audit, before changing anything

Check each claim against the thing it describes, and list what is wrong before
editing. Sources of truth, in order:

| README claim | Verify against |
|---|---|
| Install file list | `dist/`, plus the real game folder layout in `docs/modules/packaging.md` |
| Setup script name and behaviour | `dist/Setup.bat` — read it, do not assume |
| Runtime / headset support | `docs/modules/shim.md` (the shim is what makes SteamVR work) |
| Feature list | `docs/CODEMAP.md` and `.planning/STATE.md` — do not advertise a feature that `STATE.md` lists as open |
| Tuning keys | grep the source for `GetAsyncKeyState`; note `docs/modules/input.md` records real key collisions |
| Settings named in prose | `dist/BioshockVR.ini` and the `Config.h` defaults |
| Weapon tuning status | the per-slot tables in `docs/modules/hands.md` |

## 2. Rules for the edit

- **Say only what is true today.** A README that oversells is worse than one that
  is thin — a user who hits a feature that does not work stops trusting all of it.
- **Do not describe internals.** No offsets, no hook names, no measurements. If a
  sentence would only make sense to someone reading `docs/`, cut it.
- **Known-broken things get a line, not silence.** If a headset is untested or a
  weapon is untuned, say so plainly. This project already does this well — the
  "Weapons still to tune" section is the model.
- **Keep the voice.** Plain, direct, no marketing. Match what is there.
- **Do not touch the tone of sections that are still accurate.** This is an audit,
  not a rewrite.

## 3. Known drift to re-check every time

These have each been wrong before:

- The script is `Setup.bat`. The README has called it `FirstTimeSetup.bat`.
- The package is **not** four files. It includes both loader DLLs and
  `Uninstall.bat`.
- SteamVR support comes from the bundled shim and requires the setup script's
  runtime choice — this is a headline feature and was missing entirely.
- `Del` cycles the HUD edit property *and* fires the `GETTEST` probe; the key map
  has collisions (`docs/modules/input.md`).

## 4. Finish

Report what changed and what you deliberately left alone. If a claim could not be
verified from the repo, say so rather than quietly keeping or deleting it —
`README.md` describes a shipped release, and this working tree may be ahead of it.
