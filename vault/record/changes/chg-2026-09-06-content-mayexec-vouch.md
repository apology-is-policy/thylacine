---
id: chg-2026-09-06-content-mayexec-vouch
type: chg
title: "sub-kernel-content de-stale: devramfs vouches may_back_exec=true (the #217 I-12 provenance floor) -- ramfs may back executable pages, /env's Dev deliberately may not"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-content
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-kernel-content]] (updated 2026-08-16), `audit: hard`, had one post-dossier
change to its files: `kernel/devramfs.c:622` gained `.may_back_exec = true`
(#217 F1, commit `141241ea`), the I-12 PROVENANCE half's allowlist floor. Small,
settled, and a clean fit -- the dossier already discusses ramfs preserving each
file's execute bit (the "defensive fallback" caveat), and the vouch is why.

Folded:
- A Mechanism paragraph on the vouch: a file-backed executable mapping is
  admitted only if BOTH the backing Dev carries `may_back_exec` AND the mount
  is not `MNOEXEC`. ramfs vouches (it serves the binaries the machine executes);
  `/env`'s Dev deliberately does not (per-process variable text, never code --
  verified: `grep may_back_exec kernel/devenv.c` = none). The vouch is the
  allowlist ENTRY, not the enforcement -- the check lives on the exec/mmap path,
  fail-closed (a Dev that forgot it has its files refused as exec backing).
- Added `[[inv-i12]]` to `guarded-by` (the dossier now composes I-12's
  provenance half, the same "composes not enforces" sense libthyla-rs's
  guarded-by already uses for inv-i12).

Scope-checked: `may_back_exec` is set only on devramfs among this dossier's
files (devenv/env/random do not serve exec content). The fact is stable
regardless of #217's round count -- a later round will not un-vouch ramfs.
`updated:` -> 2026-09-06. Stale backlog -> one fewer.
