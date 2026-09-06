---
id: chg-2026-09-06-capstorage-doc-absorb
type: chg
title: "absorb docs/reference/98-capability-storage (I-23 + FS-delta O_PATH): fold the A-1.7 F1 monotonic-bound reconciliation into inv-i23, multi-redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["86637f88"]
touched: [inv-i23]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
Capability-scoped service storage (I-23, NOVEL #10) + FS-delta (T_OPATH / O_PATH).
Verified atom-by-atom.

ALREADY COVERED (verified, not assumed):
- The I-23 model (cooperative chroot enforcement, the post-service-FIRST/chroot-
  SECOND ordering, the boot-time confinement proof, the "blind-to" a non-chrooting
  service) -> inv-i23 (note exists).
- FS-delta / T_OPATH -> CWALKONLY (the O_PATH navigation handle: walkable +
  create/rename/unlink/chroot base, byte-I/O-blocked) -> sub-kernel-spoor (:144)
  + sub-kernel-stalk + the walk-open handler in sub-kernel-syscall-dispatch.
- The corvus consumer (chroot-to-fd-0, mkdir_or_open O_PATH mkdir -p, the boot
  smoke) -> sub-corvus; the shared-9P-session lifetime (corvus outlives joey via
  p9_attached_ref) -> sub-kernel-ninep-attach + sub-kernel-ninep-dev9p.

THE FOLD (the atom that lived only in the doc):
The A-1.7 audit F1 reconciliation. inv-i23 had the cooperative model but not the
CORRECTION of a false earlier claim (doc:163): withholding RIGHT_TRANSFER does NOT
block a grantee from re-handing its capability -- the spawn-fd endow +
handle_dup gate on kind + a rights SUBSET, never on TRANSFER. A grantee handed only
R|W can still delegate to its children, which is SOUND: the delegate stays <= R +
same subtree (I-6/I-4) and cannot manufacture rights it lacks, so the MONOTONIC
BOUND is the load-bearing property. Withholding TRANSFER is least-authority
hardening, not the enforcement. Folded into inv-i23's Enforcement so a future
reader cannot re-assert the corrected claim (I-6/I-4 written plain -- no notes).

NOT REFUTED: everything else is current. Zero code change. Multi-redirect stub.
