---
id: chg-2026-09-06-fs-mutation-doc-absorb
type: chg
title: "absorb docs/reference/96-fs-mutation (FS-mutation syscalls): zero-fold, multi-redirect"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/96-fs-mutation.md -> ABSORBED

Absorbed the 509-line FS-mutation reference doc (SYS_WALK_CREATE=54 / FSYNC=55 /
READDIR=56 / RENAME=57 / UNLINK=58) into a multi-redirect stub. Verified covered:

- the syscall handlers + rwx gates (sys_walk_create/rename/unlink_handler, gated
  on dev->perm_enforced) -> sub-kernel-syscall-dispatch (line 263-266).
- the dev9p vtable + the #99 create-errno propagation (the #102 errno-loss fix:
  create_errno read once via dev9p_create_errno, clamped [-4095,-2]; plus the
  later #99-F1 spurious-ENOENT SMP-gate P1) -> sub-kernel-ninep-dev9p (line
  131-147).
- the libthyla-rs wrappers (LS-3b) -> sub-pouch-fs.

Zero fold. The atomic rename+unlink mechanism is the dev9p dossier's; corvus's
use of it (write-tmp->rename-swap identity DB) is sub-corvus-mint's. The doc's
Status is a snapshot (FS-*-audit-clean; the #713 eret-window root-cause).

94 -> 95 absorbed of 157. lint 0-fail.
