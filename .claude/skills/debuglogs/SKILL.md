---
name: debuglogs
description: Ingest a user's log bundle, fingerprint it, match it against known patterns across setups, and record what fixed it. Use when a bug report or CollectLogs zip arrives from someone else's machine, or when looking for patterns across past reports. For reading a single local log from your own play session, use log-triage instead.
---

# Debug logs — the cross-setup corpus

`log-triage` reads **one** log and tells you what went wrong on **this** machine.
This is the layer above it: ingest a report from **someone else's** machine,
fingerprint it, compare it against every report that came before, and record what
actually fixed it.

**Read `docs/LOG-PLAYBOOK.md` first.** It carries the symptom → cause → fix
knowledge and the list of signatures that lie.

---

## The one thing to keep in mind

> **A healthy-looking log is not evidence that anything works.** Several real
> failures here leave every documented health signal green — `EYEQ depth min=1
> max=1`, `hud: host found` climbing, `POLL: synth high realpad 0` — while the
> game is unplayable. The playbook marks those **LIES GREEN**. Never close a
> report because the health lines looked fine.

---

## Workflow

### 1. Take the bundle in

The user drops a `CollectLogs` zip, or points at a folder. Extract it to
`research/logs/<YYYY-MM-DD>-<headset>-<symptom>/`, e.g.
`2026-08-14-index-cannot-walk/`.

**Redact before anything else.** Bundles contain Windows usernames in paths.
Replace the username with `<user>` throughout every text file. Do this on ingest,
not later — the redacted copy is the one that gets read and quoted.

### 2. Fingerprint it

Pull these out and write them down together. This is what makes two reports
comparable:

| Field | Where |
|---|---|
| Build | `dllmain build:` |
| Version | `BioshockVR version:` |
| Runtime / loader | `>>> XR: runtime =`, and whether `openxr_shim.log` exists |
| Headset + controllers | `>>> INPUT: bound profile (left/right) =` |
| Bindings resolved | `>>> INPUT: bindings resolved N of M` |
| Health | `EYEQ`, `hud: host found`, `POLL:` |
| **First wrong line** | the first `!!!` or the first health signal that is off |
| Settings that differ from the shipped defaults | the config echo |

**The first wrong line is the primary key.** Everything after it is downstream and
usually noise.

### 3. Match against the playbook

- **Known pattern** → name it, give the fix, mark `known`.
- **No match** → say so plainly. Do not force a report into the nearest pattern;
  a wrong match costs more than an open question. Mark `new` and add a row to the
  playbook's symptom table describing what was seen, even without a cause yet.

Then look for **repeats**: same headset, same first-wrong-line, different user.
Two of those is a pattern and should be promoted from a report to a playbook
entry.

### 4. Record it

Append one row to `research/logs/INDEX.md`. When a fix is later **verified on the
reporter's machine**, move it to `fixed` and promote it into the playbook's
*Confirmed fixes* table **with the evidence that proved it** — the log line, the
setting, the user's words. A fix without evidence is a guess, and guesses come
back.

---

## When the bundle is incomplete

Ask for what is missing before theorising. In particular:

- **`BioshockVR.log` is truncated at every startup**, so a relaunch destroys the
  evidence. If they have already relaunched, ask them to reproduce and send the
  log *before* launching again.
- **`BioshockVR_loader.log` is the only file that exists when the mod fails to
  load at all.** No mod log plus a loader log is itself a diagnosis.
- **An absent `openxr_shim.log` is not a failure** on the standard loader — the
  shim is not in the picture there.
- The bundle should carry the game's own `Bioshock.ini`, because Setup modifies
  it and it causes the two most common reports.

## What not to do

- **Do not ask the user to paste a log into chat.** Read the file.
- **Do not fix anything in this workflow.** Ingest, diagnose, record. Code changes
  are a separate decision with their own test cycle — one change per cycle is this
  project's most expensively-learned rule.
- **Do not commit bundles.** `research/` is gitignored wholesale, which is
  deliberate -- reports carry usernames and machine details. The knowledge lives
  in `docs/LOG-PLAYBOOK.md`, which IS tracked and ships with a clone, exactly as
  `CLAUDE.md` says the published knowledge base should.
