---
id: dec-2026-10-19-loom-create-refusal-eacces
type: dec
title: "Loom's refused create answers EACCES, existing name or not"
date: 2026-10-19
status: standing
decided-by: user-vote
affects: [sub-kernel-loom, sub-kernel-syscall-abi]
created: 2026-10-19
---
## Fork

M7 refuses a create through a 9P session unless the creator is at one
of the session's two ends. The sync create first checks whether the
name exists and answers `EEXIST` to a caller who may search the
directory (ERRORS.md, B-3b's create order), so a refused create of an
existing name still learns that the name exists. Loom's `MKDIR`, `MKNOD`
and `SYMLINK` ask M7 at submit and complete with `EACCES` without
looking, so the same create answers `EEXIST` on one path and `EACCES` on
the other. Holotype round 1 (F2) asked which ERRORS.md should record.

## Research

- **POSIX.** When more than one error applies, any one of them may be
  returned, because the order of detection is undefined (XSH 2.3).
  Linux's `may_create` checks existence before write permission.
- **The tree.** Loom's gate runs in the submitter's context and sends
  nothing for a refused op. An existence probe would add a walk to the
  gate of every refused create. No in-tree caller of the three ops
  depends on `EEXIST` from a refused create: symlink-probe, the one
  user, runs as a session end.

## Options

1. **Record `EACCES` as it is.**
2. **Probe for the name in Loom's gate** and answer `EEXIST` as the sync
   create does.

## The call

Option 1 (operator, 2026-10-19). ERRORS.md records that Loom's three
create ops complete with `EACCES` when M7 refuses, whether or not the
name exists. The sync create keeps its `EEXIST` order.

## Rationale

Both answers are permitted, and the refusal is the same on both paths.
The probe would add a walk to the gate to refine an error that a refused
caller who may search the directory can learn with a stat.
