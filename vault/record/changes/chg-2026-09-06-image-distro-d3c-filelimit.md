---
id: chg-2026-09-06-image-distro-d3c-filelimit
type: chg
title: "sub-kernel-image de-stale: DISTRO D-3c -- the clientele generalizes from exec-only to exec + phenotype file-backed mmap, and the #194 file_limit stamp (past-EOF SIGBUS closes the uncharged demand-zero mint)"
date: 2026-09-06
arc: arc-vault
commits: ["5f58e86e"]
touched:
  - sub-kernel-image
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-kernel-image]] (updated 2026-08-03) predates DISTRO D-3c `7c119a13`
(2026-08-10), the same settled DISTRO arc already folded into
[[sub-kernel-vma]] / [[sub-kernel-elf]] / [[sub-kernel-fault]] last run -- this
is the Image-cache half. Verified in the current source (`image.c`, `image.h`,
callers in `exec.c`/`syscall.c`).

- **The clientele generalized.** `image_lookup_or_create` was exec-only at
  REVENANT; since D-3 the phenotype file-backed `mmap` arm calls it too
  (`kernel/syscall.c:6139` + `:6350` alongside `kernel/exec.c:939`), so a
  read-only `.so` mapped into many Linux-phenotype Procs dedups exactly as a
  shared `a.out`'s text does. Folded into Purpose, Caveats ("two production
  consumers, not one"), and Performance (the 128 slots are now shared between
  exec text and mmap'd library text -- the "sixty-four binaries" estimate is
  looser under a Linux workload).
- **The #194 `file_limit` stamp.** The signature gained a `u64 file_limit`; a
  fresh Burrow is stamped `fresh->file_limit = file_limit` before publication
  (private, no lock). The fault arm refuses a page wholly past
  `round_up(file_limit)` with SIGBUS -- closing the lying-ELF / uncharged
  demand-zero mint. Consumer split, MEASURED in the source: the guest-facing
  `mmap` arm fail-closes on unknown / hostile-near-2^64 (`-EIO`); exec passes
  `spoor_file_size(exe)` and TOLERATES `BURROW_FILE_LIMIT_UNKNOWN` (`(u64)-1`),
  sound only because the sole size-less backing Dev is the immutable baked
  ramfs. On a cache HIT the creation-time limit wins (one sample per image --
  close-to-open). Folded into a new Mechanism paragraph + the [[inv-i36]] note.

Note: `file_limit` is a `struct Burrow` field (`burrow.h`), NOT an
`image_entry` key field -- the key stays seven; the stamp is a per-Image value.
So Data structures and the seven-field key description are unchanged.

`updated:` -> 2026-09-06. Stale backlog 34 -> 33.
