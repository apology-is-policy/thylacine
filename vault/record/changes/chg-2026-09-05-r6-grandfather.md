---
id: chg-2026-09-05-r6-grandfather
type: chg
title: "R6 grandfathers committed history: a grown mirror set stops failing past chgs"
date: 2026-09-05
arc: arc-vault
commits: ["86ad7e8c"]
touched: [dec-2026-07-31-quaestor]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
R6 held that a chg touching an [[abi-boot-banner|abi]] with mirrors must carry
a `mirrors-checked` set covering it. The check compared `len(mirrors-checked)`
against the **current** mirror count (`validate.go`), which made it
**retroactive**: the moment the resume-sync merge grew the boot-banner mirror
set from 15 to 28 (13 new consumer gates arrived from main), the three
historical chgs that had honestly checked 15 all failed -- and the Record plane
is append-only (R3), so their `mirrors-checked` could never be brought to 28.
A grown mirror set therefore wedged the vault: no session could commit until it
cleared, and it could not be cleared. main hit exactly this (yip 0049).

## What changed

`validate` now computes `gitDirtySet(vaultRoot(reg))` once -- the notes that
differ from HEAD (staged, unstaged, or untracked, via `git status
--porcelain`). The R6 shortfall is a failure only for a chg that is being
**authored now** (present in that set); a committed, unchanged chg is
grandfathered.

## Why this is the right form of "as of its commit"

A committed, unchanged chg already passed this same gate at its commit, when
the gate measured it against the mirror set that existed then. That prior pass
IS the "as of its commit" validation the operator asked for -- so trusting it
is exact, not an approximation, and it needs no per-chg `git show` archaeology
of the abi note's history. The obligation still bites where it must: a chg that
GROWS the set is itself dirty, so it is held to the new count and must check the
full set (the establishing [[chg-2026-08-16-boot-banner-mirror-set]] did
exactly that at 15).

## Alternatives rejected

- **Put the 13 new consumers in `literal-mentions`** to keep `mirrors` at 15 and
  dodge R6. Rejected: they are `.exp`/`.sh` gates that `expect`/watch the
  literal and BREAK if it changes (several silently -- a crash-watcher that stops
  matching `EXTINCTION:`). `literal-mentions` means "goes stale, does not break";
  stuffing breaking consumers there corrupts a field future readers trust.
- **Grandfather every committed chg unconditionally** (skip the dirty test).
  Nearly equivalent, but it also forgives a committed chg that was edited in
  the working tree -- which is a re-authoring that should be re-checked. The
  dirty test costs one `git status` and closes that.

## Verification

`TestMirrorsCheckedGrandfathersCommitted` (git fixture): commit a chg that
checks both of an abi's two mirrors, grow the abi to three, and the committed
chg does NOT fail; then edit that chg and it fails again (dirty -> current set
applies). `TestMirrorsCheckedRequired` (no-git fixture) still passes: without
git, `gitDirtySet` returns nil and the caller enforces on every chg, the
original behaviour, so a genuinely-new shortfall is still caught. Full
`go test ./` green.

This is the tooling half of [[dec-2026-08-15-cutover]]'s interim: the yip
`quaestor owner` handoff routes upstream consumers into vault-carried abi
mirror sets, and those sets must be able to GROW without breaking the history
that records how they got there.
